#!/usr/bin/env python3
"""RemoteBSP 最小只读 Runtime HTTP API。"""

from __future__ import annotations

import argparse
import ipaddress
import json
import secrets
import socket
import sys
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable
from urllib.parse import ParseResult, parse_qsl, unquote, urlparse

from .auth import (
    MAXIMUM_API_KEY_BYTES,
    RUNTIME_READ_PERMISSION,
    ApiKeyAuthenticator,
    AuthenticatedPrincipal,
    AuthConfigurationError,
    load_api_key_authenticator,
)
from .audit import (
    BoundedAuditSink,
    DEFAULT_AUDIT_CAPACITY,
    new_audit_record,
)
from .events import (
    DEFAULT_EVENT_CAPACITY,
    DEFAULT_EVENT_PAGE_LIMIT,
    EVENT_SCHEMA_VERSION,
    MAXIMUM_EVENT_CAPACITY,
    MAXIMUM_EVENT_PAGE_LIMIT,
    EventLogError,
    ExpiredEventCursor,
    InvalidEventCursor,
    RuntimeEventLog,
    validate_event_cursor,
)
from .models import RUNTIME_SNAPSHOT_SCHEMA_VERSION
from .provider import (
    FileSnapshotProvider,
    MockSnapshotProvider,
    RuntimeProvider,
    RuntimeProviderError,
    SnapshotRead,
)
from .toolbusd_provider import RemoteCliIpcClient, ToolbusdSnapshotProvider


API_VERSION = "v1"
_QUERY_CREDENTIAL_NAMES = {
    "api_key", "api-key", "apikey", "x-api-key", "access_token", "token",
}


class BoundedThreadingHTTPServer(ThreadingHTTPServer):
    """限制活动请求线程数；满载时由监听队列自然施加背压。"""

    def __init__(self, server_address, request_handler_class, *,
                 maximum_workers: int = 32):
        if maximum_workers < 1 or maximum_workers > 256:
            raise ValueError("HTTP工作线程数必须位于1～256")
        self._worker_slots = threading.BoundedSemaphore(maximum_workers)
        super().__init__(server_address, request_handler_class)

    def process_request(self, request, client_address) -> None:
        self._worker_slots.acquire()
        try:
            super().process_request(request, client_address)
        except BaseException:
            self._worker_slots.release()
            raise

    def process_request_thread(self, request, client_address) -> None:
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._worker_slots.release()

    def server_close(self) -> None:
        try:
            super().server_close()
        finally:
            audit_sink = getattr(self, "audit_sink", None)
            close = getattr(audit_sink, "close", None)
            if callable(close):
                close()


class IPv6ThreadingHTTPServer(BoundedThreadingHTTPServer):
    address_family = socket.AF_INET6


class RuntimeRequestHandler(BaseHTTPRequestHandler):
    server_version = "RemoteBSP-Runtime/0.1"

    @property
    def provider(self) -> RuntimeProvider:
        return self.server.provider  # type: ignore[attr-defined]

    @property
    def authenticator(self) -> ApiKeyAuthenticator | None:
        return self.server.authenticator  # type: ignore[attr-defined]

    @property
    def audit_sink(self) -> BoundedAuditSink:
        return self.server.audit_sink  # type: ignore[attr-defined]

    @property
    def event_log(self) -> RuntimeEventLog:
        return self.server.event_log  # type: ignore[attr-defined]

    def _begin_request_audit(self) -> None:
        self._audit_request_id = secrets.token_hex(16)
        self._audit_key_id: str | None = None
        self._audit_path_category = self._classify_path(self.path)
        self._audit_emitted = False

    @staticmethod
    def _classify_path(target: str) -> str:
        try:
            path = urlparse(target).path
            parts = [
                unquote(part, encoding="utf-8", errors="strict")
                for part in path.strip("/").split("/") if part
            ]
        except (UnicodeDecodeError, ValueError):
            return "unknown"
        if parts == ["api", API_VERSION]:
            return "root"
        if len(parts) >= 3 and parts[:2] == ["api", API_VERSION] and \
                parts[2] in {"health", "snapshot", "nodes", "resources",
                             "alerts", "events"}:
            return parts[2]
        return "unknown"

    def _emit_audit(self, result: str) -> None:
        if getattr(self, "_audit_emitted", False):
            return
        self._audit_emitted = True
        method_category = {
            "GET": "read", "HEAD": "read", "POST": "write",
            "PUT": "write", "PATCH": "write", "DELETE": "write",
            "OPTIONS": "options",
        }.get(self.command, "other")
        record = new_audit_record(
            request_id=self._audit_request_id,
            key_id=self._audit_key_id,
            method_category=method_category,
            path_category=self._audit_path_category,
            result=result)
        try:
            self.audit_sink.emit(record)
        except Exception:
            # 外部审计输出端故障不能改变 API 响应或阻塞后续请求。
            pass

    def _send_json(self, value: object, status: HTTPStatus = HTTPStatus.OK,
                   *, headers: dict[str, str] | None = None,
                   audit_result: str = "allowed") -> None:
        encoded = json.dumps(value, ensure_ascii=False,
                             separators=(",", ":")).encode("utf-8")
        self._emit_audit(audit_result)
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Request-ID", self._audit_request_id)
        for key, header_value in (headers or {}).items():
            self.send_header(key, header_value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(encoded)

    def _error(self, status: HTTPStatus, code: str, message: str,
               *, headers: dict[str, str] | None = None,
               details: dict | None = None) -> None:
        error_payload = {"code": code, "message": message}
        if details is not None:
            error_payload["details"] = details
        self._send_json({
            "api_version": API_VERSION,
            "ok": False,
            "error": error_payload,
        }, status, headers=headers, audit_result=code)

    def _success(self, data: object, status: HTTPStatus = HTTPStatus.OK,
                 *, read: SnapshotRead | None = None,
                 audit_result: str = "allowed") -> None:
        payload = {
            "api_version": API_VERSION,
            "ok": True,
            "data": data,
        }
        headers = None
        if read is not None:
            freshness = {
                "cache_status": read.cache_status,
                "age_ms": read.age_ms,
                "cache_ttl_ms": read.cache_ttl_ms,
            }
            payload["meta"] = {"snapshot_freshness": freshness}
            headers = {
                "X-RemoteBSP-Snapshot-Cache": read.cache_status,
                "X-RemoteBSP-Snapshot-Age-Ms": (
                    "unknown" if read.age_ms is None else str(read.age_ms)),
            }
        self._send_json(payload, status, headers=headers,
                        audit_result=audit_result)

    def _send_empty(self, status: HTTPStatus, *,
                    headers: dict[str, str] | None = None,
                    audit_result: str = "allowed") -> None:
        self._emit_audit(audit_result)
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Request-ID", self._audit_request_id)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()

    def _parse_request_target(self, *,
                              allow_event_query: bool = False
                              ) -> ParseResult | None:
        try:
            parsed = urlparse(self.path)
        except ValueError:
            self._error(HTTPStatus.BAD_REQUEST, "invalid_path", "请求路径无效")
            return None
        self._query_pairs: list[tuple[str, str]] = []
        if parsed.query:
            try:
                self._query_pairs = parse_qsl(
                    parsed.query, keep_blank_values=True, max_num_fields=64)
                names = {name.lower() for name, _ in self._query_pairs}
            except ValueError:
                self._error(HTTPStatus.BAD_REQUEST, "invalid_query",
                            "查询参数编码无效")
                return None
            if names & _QUERY_CREDENTIAL_NAMES:
                self._error(HTTPStatus.BAD_REQUEST, "credential_in_query",
                            "认证信息不得放入URL查询参数")
            elif allow_event_query and \
                    parsed.path == f"/api/{API_VERSION}/events":
                return parsed
            else:
                self._error(HTTPStatus.BAD_REQUEST, "query_not_supported",
                            "当前只读API不接受查询参数")
            return None
        if parsed.fragment:
            self._error(HTTPStatus.BAD_REQUEST, "query_not_supported",
                        "当前只读API不接受查询参数")
            return None
        return parsed

    def _event_parameters(self) -> tuple[str | None, int] | None:
        values: dict[str, str] = {}
        for name, value in self._query_pairs:
            if name not in {"cursor", "limit"} or name in values:
                self._error(HTTPStatus.BAD_REQUEST, "event_query_invalid",
                            "事件查询只允许各一个cursor和limit参数")
                return None
            values[name] = value
        cursor = values.get("cursor")
        if cursor == "":
            self._error(HTTPStatus.BAD_REQUEST, "event_cursor_invalid",
                        "事件游标不能为空")
            return None
        if cursor is not None:
            try:
                validate_event_cursor(cursor)
            except InvalidEventCursor as error:
                self._error(HTTPStatus.BAD_REQUEST, "event_cursor_invalid",
                            str(error))
                return None
        limit_text = values.get("limit")
        if limit_text is None:
            limit = DEFAULT_EVENT_PAGE_LIMIT
        elif len(limit_text) > 3 or not limit_text.isascii() or \
                not limit_text.isdigit() or \
                (len(limit_text) > 1 and limit_text.startswith("0")):
            self._error(HTTPStatus.BAD_REQUEST, "event_limit_invalid",
                        "事件页大小必须是规范十进制整数")
            return None
        else:
            limit = int(limit_text)
            if limit < 1 or limit > MAXIMUM_EVENT_PAGE_LIMIT:
                self._error(
                    HTTPStatus.BAD_REQUEST, "event_limit_invalid",
                    f"事件页大小必须位于1～{MAXIMUM_EVENT_PAGE_LIMIT}")
                return None
        return cursor, limit

    def _path_parts(self, path: str) -> list[str] | None:
        try:
            return [
                unquote(part, encoding="utf-8", errors="strict")
                for part in path.strip("/").split("/") if part
            ]
        except UnicodeDecodeError:
            self._error(HTTPStatus.BAD_REQUEST, "invalid_path", "路径编码无效")
            return None

    def _authorize(self, path: str, *, public_health: bool,
                   required_permission: str | None
                   ) -> AuthenticatedPrincipal | str | None:
        if self.authenticator is None:
            return "disabled_loopback"

        authorization = self.headers.get_all("Authorization", [])
        api_keys = self.headers.get_all("X-API-Key", [])
        if not authorization and not api_keys and public_health and \
                path == f"/api/{API_VERSION}/health":
            return "public_health"
        if not authorization and not api_keys:
            self._error(
                HTTPStatus.UNAUTHORIZED, "authentication_required",
                "需要有效的API密钥",
                headers={"WWW-Authenticate":
                         'Bearer realm="RemoteBSP Runtime"'})
            return None
        if len(authorization) + len(api_keys) != 1:
            self._error(HTTPStatus.BAD_REQUEST, "invalid_auth_header",
                        "必须且只能使用一种认证请求头")
            return None

        if authorization:
            pieces = authorization[0].split(" ")
            if len(pieces) != 2 or pieces[0].lower() != "bearer" or \
                    not pieces[1]:
                self._error(HTTPStatus.BAD_REQUEST, "invalid_auth_header",
                            "Authorization必须使用Bearer格式")
                return None
            candidate = pieces[1]
        else:
            candidate = api_keys[0]
            if not candidate or candidate.strip() != candidate:
                self._error(HTTPStatus.BAD_REQUEST, "invalid_auth_header",
                            "X-API-Key格式无效")
                return None
        principal = None if len(candidate) > MAXIMUM_API_KEY_BYTES else \
            self.authenticator.authenticate(candidate)
        if principal is None:
            self._error(
                HTTPStatus.UNAUTHORIZED, "authentication_required",
                "需要有效的API密钥",
                headers={"WWW-Authenticate":
                         'Bearer realm="RemoteBSP Runtime"'})
            return None
        self._audit_key_id = principal.key_id
        if required_permission is not None and \
                required_permission not in principal.permissions:
            self._error(HTTPStatus.FORBIDDEN, "permission_denied",
                        "当前API密钥没有所需只读权限")
            return None
        return principal

    def _snapshot(self) -> SnapshotRead | None:
        try:
            read = self.provider.read_snapshot()
        except RuntimeProviderError as error:
            self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                        "provider_unavailable", str(error))
            return None
        self.event_log.observe(read.snapshot)
        return read

    def _runtime_capabilities(self) -> dict:
        return self.provider.runtime_capabilities()

    @staticmethod
    def _node_summary(node: dict, alerts: list[dict]) -> dict:
        return {
            key: node[key] for key in (
                "node_id", "board_type", "display_name", "state",
                "last_seen_ms", "links")
        } | {
            "resource_count": len(node["resources"]),
            "active_alert_count": sum(
                alert["active"] and alert["node_id"] == node["node_id"]
                for alert in alerts),
        }

    def _handle_read(self) -> None:
        parsed = self._parse_request_target(allow_event_query=True)
        if parsed is None:
            return
        parts = self._path_parts(parsed.path)
        if parts is None:
            return

        api_path = len(parts) >= 2 and parts[:2] == ["api", API_VERSION]
        if api_path:
            auth_context = self._authorize(
                parsed.path, public_health=self.command in ("GET", "HEAD"),
                required_permission=RUNTIME_READ_PERMISSION)
            if auth_context is None:
                return
            if auth_context == "public_health":
                self._success({"status": "ok", "scope": "liveness"},
                              audit_result="public_probe")
                return

        if parts == ["api", API_VERSION]:
            self._success({
                "snapshot_schema_version": RUNTIME_SNAPSHOT_SCHEMA_VERSION,
                "capabilities": {
                    "read_only": True,
                    "write_commands": False,
                    "authentication": self.authenticator is not None,
                    "authentication_mode": (
                        "api_key" if self.authenticator is not None
                        else "disabled_loopback"),
                    "event_stream": False,
                    "incremental_events": {
                        "available": True,
                        "schema_version": EVENT_SCHEMA_VERSION,
                        "transport": "short_poll",
                        "maximum_page_size": MAXIMUM_EVENT_PAGE_LIMIT,
                        "capacity": self.event_log.capacity,
                    },
                    **self._runtime_capabilities(),
                },
                "endpoints": ["health", "snapshot", "nodes", "resources",
                              "alerts", "events"],
            })
            return
        if parts == ["api", API_VERSION, "health"]:
            read = self._snapshot()
            if read is not None:
                snapshot = read.snapshot
                self._success({"status": "ok",
                               "snapshot_id": snapshot["snapshot_id"],
                               "capabilities":
                                   self._runtime_capabilities()},
                              read=read)
            return
        if parts == ["api", API_VERSION, "events"]:
            parameters = self._event_parameters()
            if parameters is None:
                return
            cursor, limit = parameters
            if cursor is not None:
                try:
                    self.event_log.validate_cursor_incarnation(cursor)
                except ExpiredEventCursor as error:
                    self._error(
                        HTTPStatus.CONFLICT, "event_cursor_expired",
                        str(error),
                        details={"reset_cursor": error.reset_cursor})
                    return
            read = self._snapshot()
            if read is None:
                return
            try:
                page = self.event_log.read_page(cursor, limit)
            except ExpiredEventCursor as error:
                self._error(
                    HTTPStatus.CONFLICT, "event_cursor_expired", str(error),
                    details={"reset_cursor": error.reset_cursor})
                return
            except InvalidEventCursor as error:
                self._error(HTTPStatus.BAD_REQUEST, "event_cursor_invalid",
                            str(error))
                return
            except EventLogError as error:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "event_log_unavailable", str(error))
                return
            self._success({
                "event_schema_version": EVENT_SCHEMA_VERSION,
                "events": [event.to_dict() for event in page.events],
                "next_cursor": page.next_cursor,
                "has_more": page.has_more,
            }, read=read)
            return
        if len(parts) < 3 or parts[:2] != ["api", API_VERSION]:
            self._error(HTTPStatus.NOT_FOUND, "not_found", "API路径不存在")
            return

        read = self._snapshot()
        if read is None:
            return
        snapshot = read.snapshot
        endpoint = parts[2]
        if parts == ["api", API_VERSION, "snapshot"]:
            self._success(snapshot, read=read)
            return
        if parts == ["api", API_VERSION, "nodes"]:
            self._success([
                self._node_summary(node, snapshot["alerts"])
                for node in snapshot["nodes"]], read=read)
            return
        if parts == ["api", API_VERSION, "resources"]:
            self._success([
                {"node_id": node["node_id"], **resource}
                for node in snapshot["nodes"]
                for resource in node["resources"]], read=read)
            return
        if parts == ["api", API_VERSION, "alerts"]:
            self._success(snapshot["alerts"], read=read)
            return
        if endpoint == "nodes" and len(parts) in (4, 5):
            node_id = parts[3]
            node = next((item for item in snapshot["nodes"]
                         if item["node_id"] == node_id), None)
            if node is None:
                self._error(HTTPStatus.NOT_FOUND, "node_not_found",
                            f"节点不存在：{node_id}")
                return
            if len(parts) == 4:
                self._success(node, read=read)
                return
            if parts[4] == "resources":
                self._success(node["resources"], read=read)
                return
            if parts[4] == "alerts":
                self._success([alert for alert in snapshot["alerts"]
                               if alert["node_id"] == node_id], read=read)
                return
        self._error(HTTPStatus.NOT_FOUND, "not_found", "API路径不存在")

    def do_GET(self) -> None:  # noqa: N802
        self._begin_request_audit()
        self._handle_read()

    def do_HEAD(self) -> None:  # noqa: N802
        self._begin_request_audit()
        self._handle_read()

    def do_POST(self) -> None:  # noqa: N802
        self._begin_request_audit()
        parsed = self._parse_request_target()
        if parsed is None:
            return
        parts = self._path_parts(parsed.path)
        if parts is None:
            return
        if len(parts) >= 2 and parts[:2] == ["api", API_VERSION]:
            if self._authorize(parsed.path, public_health=False,
                               required_permission=None) is None:
                return
        self._error(HTTPStatus.METHOD_NOT_ALLOWED, "read_only",
                    "本轮Runtime API只允许读取",
                    headers={"Allow": "GET, HEAD, OPTIONS"})

    do_PUT = do_POST  # type: ignore[assignment]
    do_PATCH = do_POST  # type: ignore[assignment]
    do_DELETE = do_POST  # type: ignore[assignment]

    def do_OPTIONS(self) -> None:  # noqa: N802
        self._begin_request_audit()
        parsed = self._parse_request_target()
        if parsed is None:
            return
        parts = self._path_parts(parsed.path)
        if parts is None:
            return
        if len(parts) >= 2 and parts[:2] == ["api", API_VERSION]:
            self._send_empty(HTTPStatus.NO_CONTENT,
                             headers={"Allow": "GET, HEAD, OPTIONS"},
                             audit_result="options")
            return
        self._error(HTTPStatus.NOT_FOUND, "not_found", "API路径不存在")

    def log_message(self, format: str, *args: object) -> None:
        if args and str(args[1]).startswith(("4", "5")):
            # 请求目标可能含被拒绝的查询参数，不能把密钥写入日志。
            sys.stderr.write(f"Runtime HTTP请求失败：{args[1]}\n")


def make_server(host: str, port: int,
                provider: RuntimeProvider, *,
                maximum_workers: int = 32,
                authenticator: ApiKeyAuthenticator | None = None,
                audit_capacity: int = DEFAULT_AUDIT_CAPACITY,
                audit_output: Callable[[dict], None] | None = None,
                event_capacity: int = DEFAULT_EVENT_CAPACITY,
                event_incarnation: str | None = None
                ) -> ThreadingHTTPServer:
    try:
        loopback = ipaddress.ip_address(host).is_loopback
    except ValueError as error:
        raise ValueError("--host必须是数字IP地址；不能使用主机名") from error
    if not loopback and authenticator is None:
        raise ValueError("非回环监听必须配置API密钥认证")
    event_log = RuntimeEventLog(
        event_capacity, incarnation=event_incarnation)
    server_type = IPv6ThreadingHTTPServer if ":" in host \
        else BoundedThreadingHTTPServer
    server = server_type((host, port), RuntimeRequestHandler,
                         maximum_workers=maximum_workers)
    server.provider = provider  # type: ignore[attr-defined]
    server.authenticator = authenticator  # type: ignore[attr-defined]
    server.audit_sink = BoundedAuditSink(  # type: ignore[attr-defined]
        capacity=audit_capacity, output=audit_output)
    server.event_log = event_log  # type: ignore[attr-defined]
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1",
                        help="监听数字IP地址；非回环地址必须配置API密钥")
    parser.add_argument("--port", type=int, default=8780, help="监听端口")
    parser.add_argument("--api-key-file", type=Path,
                        help="版本化API密钥JSON文件；不支持命令行明文密钥")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--snapshot", type=Path,
                        help="Runtime v1 快照JSON；省略数据源时使用内置Mock")
    source.add_argument("--toolbusd-socket", type=Path,
                        help="通过remote-cli连接的toolbusd本地套接字")
    parser.add_argument("--remote-cli",
                        help="remote-cli可执行文件，默认从PATH查找")
    parser.add_argument("--ipc-timeout-ms", type=int, default=2000,
                        help="每次只读IPC调用超时，默认2000毫秒")
    parser.add_argument("--snapshot-cache-ms", type=int, default=250,
                        help="toolbusd快照短时缓存，默认250毫秒")
    parser.add_argument("--snapshot-refresh-wait-ms", type=int, default=5000,
                        help="等待同批快照刷新的上限，默认5000毫秒")
    parser.add_argument("--maximum-resource-queries", type=int, default=128,
                        help="单次快照资源状态查询上限，默认128项")
    parser.add_argument("--status-query-workers", type=int, default=8,
                        help="资源状态IPC并发上限，默认8路")
    parser.add_argument("--http-workers", type=int, default=32,
                        help="活动HTTP请求线程上限，默认32个")
    parser.add_argument("--event-capacity", type=int,
                        default=DEFAULT_EVENT_CAPACITY,
                        help="进程内增量事件保留条数，默认1024条")
    parser.add_argument(
        "--clock-error-warning-ns", type=int, default=250_000,
        help="主机时钟模型估计误差上界告警阈值，默认250000纳秒")
    parser.add_argument(
        "--clock-sample-age-warning-ms", type=int, default=1_000,
        help="主机时钟模型样本年龄告警阈值，默认1000毫秒")
    parser.add_argument(
        "--toolbusd-legacy-text", action="store_true",
        help="显式兼容旧版remote-cli文本输出；不会自动回退")
    args = parser.parse_args()
    if args.port < 0 or args.port > 65535:
        parser.error("--port必须位于0～65535")
    if args.ipc_timeout_ms < 100 or args.ipc_timeout_ms > 10000:
        parser.error("--ipc-timeout-ms必须位于100～10000")
    if args.snapshot_cache_ms < 0 or args.snapshot_cache_ms > 60000:
        parser.error("--snapshot-cache-ms必须位于0～60000")
    if args.snapshot_refresh_wait_ms < 1 or \
            args.snapshot_refresh_wait_ms > 60000:
        parser.error("--snapshot-refresh-wait-ms必须位于1～60000")
    if args.maximum_resource_queries < 1 or \
            args.maximum_resource_queries > 4096:
        parser.error("--maximum-resource-queries必须位于1～4096")
    if args.status_query_workers < 1 or args.status_query_workers > 32:
        parser.error("--status-query-workers必须位于1～32")
    if args.http_workers < 1 or args.http_workers > 256:
        parser.error("--http-workers必须位于1～256")
    if args.event_capacity < 1 or args.event_capacity > MAXIMUM_EVENT_CAPACITY:
        parser.error(f"--event-capacity必须位于1～{MAXIMUM_EVENT_CAPACITY}")
    if args.clock_error_warning_ns < 1 or \
            args.clock_error_warning_ns > 1_000_000_000:
        parser.error("--clock-error-warning-ns必须位于1～1000000000")
    if args.clock_sample_age_warning_ms < 1 or \
            args.clock_sample_age_warning_ms > 60_000:
        parser.error("--clock-sample-age-warning-ms必须位于1～60000")
    if args.remote_cli and not args.toolbusd_socket:
        parser.error("--remote-cli必须与--toolbusd-socket一起使用")
    if args.toolbusd_legacy_text and not args.toolbusd_socket:
        parser.error("--toolbusd-legacy-text必须与--toolbusd-socket一起使用")
    if not args.toolbusd_legacy_text and \
            args.maximum_resource_queries > 128:
        parser.error("结构化Runtime快照最多允许128项资源")
    authenticator = None
    if args.api_key_file:
        try:
            authenticator = load_api_key_authenticator(args.api_key_file)
        except AuthConfigurationError as error:
            parser.error(f"--api-key-file无效：{error}")
    try:
        loopback = ipaddress.ip_address(args.host).is_loopback
    except ValueError:
        parser.error("--host必须是数字IP地址；不能使用主机名")
    if not loopback and authenticator is None:
        parser.error("非回环监听必须配置--api-key-file")
    if args.toolbusd_socket:
        provider: RuntimeProvider = ToolbusdSnapshotProvider(
            RemoteCliIpcClient(
                args.toolbusd_socket, args.remote_cli or "remote-cli",
                timeout_seconds=args.ipc_timeout_ms / 1000.0,
                structured_output=not args.toolbusd_legacy_text),
            maximum_resources_per_snapshot=args.maximum_resource_queries,
            cache_ttl_ms=args.snapshot_cache_ms,
            maximum_concurrent_status_queries=args.status_query_workers,
            refresh_wait_timeout_ms=args.snapshot_refresh_wait_ms,
            maximum_clock_error_bound_ns=args.clock_error_warning_ns,
            maximum_clock_sample_age_ms=
                args.clock_sample_age_warning_ms)
    elif args.snapshot:
        provider = FileSnapshotProvider(args.snapshot)
    else:
        provider = MockSnapshotProvider()
    server = make_server(args.host, args.port, provider,
                         maximum_workers=args.http_workers,
                         authenticator=authenticator,
                         event_capacity=args.event_capacity)
    display_host = f"[{args.host}]" if ":" in args.host else args.host
    auth_mode = "API密钥认证" if authenticator is not None else "回环开发模式"
    print(f"RemoteBSP Runtime 只读API已启动：http://{display_host}:"
          f"{server.server_port}/api/{API_VERSION}（{auth_mode}）")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

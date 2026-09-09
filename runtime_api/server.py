#!/usr/bin/env python3
"""RemoteBSP 最小只读 Runtime HTTP API。"""

from __future__ import annotations

import argparse
import json
import socket
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

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
_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}


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


class IPv6ThreadingHTTPServer(BoundedThreadingHTTPServer):
    address_family = socket.AF_INET6


class RuntimeRequestHandler(BaseHTTPRequestHandler):
    server_version = "RemoteBSP-Runtime/0.1"

    @property
    def provider(self) -> RuntimeProvider:
        return self.server.provider  # type: ignore[attr-defined]

    def _send_json(self, value: object, status: HTTPStatus = HTTPStatus.OK,
                   *, headers: dict[str, str] | None = None) -> None:
        encoded = json.dumps(value, ensure_ascii=False,
                             separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        for key, header_value in (headers or {}).items():
            self.send_header(key, header_value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(encoded)

    def _error(self, status: HTTPStatus, code: str, message: str,
               *, headers: dict[str, str] | None = None) -> None:
        self._send_json({
            "api_version": API_VERSION,
            "ok": False,
            "error": {"code": code, "message": message},
        }, status, headers=headers)

    def _success(self, data: object, status: HTTPStatus = HTTPStatus.OK,
                 *, read: SnapshotRead | None = None) -> None:
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
        self._send_json(payload, status, headers=headers)

    def _snapshot(self) -> SnapshotRead | None:
        try:
            return self.provider.read_snapshot()
        except RuntimeProviderError as error:
            self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                        "provider_unavailable", str(error))
            return None

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
        parsed = urlparse(self.path)
        if parsed.query or parsed.fragment:
            self._error(HTTPStatus.BAD_REQUEST, "query_not_supported",
                        "当前只读API不接受查询参数")
            return
        try:
            parts = [unquote(part) for part in parsed.path.strip("/").split("/")
                     if part]
        except UnicodeDecodeError:
            self._error(HTTPStatus.BAD_REQUEST, "invalid_path", "路径编码无效")
            return

        if parts == ["api", API_VERSION]:
            self._success({
                "snapshot_schema_version": RUNTIME_SNAPSHOT_SCHEMA_VERSION,
                "capabilities": {
                    "read_only": True,
                    "write_commands": False,
                    "authentication": False,
                    "event_stream": False,
                },
                "endpoints": ["health", "snapshot", "nodes", "resources",
                              "alerts"],
            })
            return
        if parts == ["api", API_VERSION, "health"]:
            read = self._snapshot()
            if read is not None:
                snapshot = read.snapshot
                self._success({"status": "ok",
                               "snapshot_id": snapshot["snapshot_id"]},
                              read=read)
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
        self._handle_read()

    def do_HEAD(self) -> None:  # noqa: N802
        self._handle_read()

    def do_POST(self) -> None:  # noqa: N802
        self._error(HTTPStatus.METHOD_NOT_ALLOWED, "read_only",
                    "本轮Runtime API只允许读取",
                    headers={"Allow": "GET, HEAD"})

    do_PUT = do_POST  # type: ignore[assignment]
    do_PATCH = do_POST  # type: ignore[assignment]
    do_DELETE = do_POST  # type: ignore[assignment]

    def log_message(self, format: str, *args: object) -> None:
        if args and str(args[1]).startswith(("4", "5")):
            super().log_message(format, *args)


def make_server(host: str, port: int,
                provider: RuntimeProvider, *,
                maximum_workers: int = 32) -> ThreadingHTTPServer:
    if host not in _LOOPBACK_HOSTS:
        raise ValueError("认证尚未实现，Runtime服务仅允许绑定本机回环地址")
    server_type = IPv6ThreadingHTTPServer if host == "::1" \
        else BoundedThreadingHTTPServer
    server = server_type((host, port), RuntimeRequestHandler,
                         maximum_workers=maximum_workers)
    server.provider = provider  # type: ignore[attr-defined]
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1",
                        help="监听地址；无认证阶段仅允许本机回环地址")
    parser.add_argument("--port", type=int, default=8780, help="监听端口")
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
    parser.add_argument(
        "--toolbusd-legacy-text", action="store_true",
        help="显式兼容旧版remote-cli文本输出；不会自动回退")
    args = parser.parse_args()
    if args.host not in _LOOPBACK_HOSTS:
        parser.error("认证尚未实现，--host仅允许本机回环地址")
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
    if args.remote_cli and not args.toolbusd_socket:
        parser.error("--remote-cli必须与--toolbusd-socket一起使用")
    if args.toolbusd_legacy_text and not args.toolbusd_socket:
        parser.error("--toolbusd-legacy-text必须与--toolbusd-socket一起使用")
    if args.toolbusd_socket:
        provider: RuntimeProvider = ToolbusdSnapshotProvider(
            RemoteCliIpcClient(
                args.toolbusd_socket, args.remote_cli or "remote-cli",
                timeout_seconds=args.ipc_timeout_ms / 1000.0,
                structured_output=not args.toolbusd_legacy_text),
            maximum_resources_per_snapshot=args.maximum_resource_queries,
            cache_ttl_ms=args.snapshot_cache_ms,
            maximum_concurrent_status_queries=args.status_query_workers,
            refresh_wait_timeout_ms=args.snapshot_refresh_wait_ms)
    elif args.snapshot:
        provider = FileSnapshotProvider(args.snapshot)
    else:
        provider = MockSnapshotProvider()
    server = make_server(args.host, args.port, provider,
                         maximum_workers=args.http_workers)
    display_host = f"[{args.host}]" if ":" in args.host else args.host
    print(f"RemoteBSP Runtime 只读API已启动：http://{display_host}:"
          f"{server.server_port}/api/{API_VERSION}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""RemoteBSP 最小只读 Runtime HTTP API。"""

from __future__ import annotations

import argparse
import json
import socket
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
)


API_VERSION = "v1"
_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}


class IPv6ThreadingHTTPServer(ThreadingHTTPServer):
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

    def _success(self, data: object,
                 status: HTTPStatus = HTTPStatus.OK) -> None:
        self._send_json({
            "api_version": API_VERSION,
            "ok": True,
            "data": data,
        }, status)

    def _snapshot(self) -> dict | None:
        try:
            return self.provider.get_snapshot()
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
            snapshot = self._snapshot()
            if snapshot is not None:
                self._success({"status": "ok",
                               "snapshot_id": snapshot["snapshot_id"]})
            return
        if len(parts) < 3 or parts[:2] != ["api", API_VERSION]:
            self._error(HTTPStatus.NOT_FOUND, "not_found", "API路径不存在")
            return

        snapshot = self._snapshot()
        if snapshot is None:
            return
        endpoint = parts[2]
        if parts == ["api", API_VERSION, "snapshot"]:
            self._success(snapshot)
            return
        if parts == ["api", API_VERSION, "nodes"]:
            self._success([
                self._node_summary(node, snapshot["alerts"])
                for node in snapshot["nodes"]])
            return
        if parts == ["api", API_VERSION, "resources"]:
            self._success([
                {"node_id": node["node_id"], **resource}
                for node in snapshot["nodes"]
                for resource in node["resources"]])
            return
        if parts == ["api", API_VERSION, "alerts"]:
            self._success(snapshot["alerts"])
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
                self._success(node)
                return
            if parts[4] == "resources":
                self._success(node["resources"])
                return
            if parts[4] == "alerts":
                self._success([alert for alert in snapshot["alerts"]
                               if alert["node_id"] == node_id])
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
                provider: RuntimeProvider) -> ThreadingHTTPServer:
    if host not in _LOOPBACK_HOSTS:
        raise ValueError("认证尚未实现，Runtime服务仅允许绑定本机回环地址")
    server_type = IPv6ThreadingHTTPServer if host == "::1" \
        else ThreadingHTTPServer
    server = server_type((host, port), RuntimeRequestHandler)
    server.provider = provider  # type: ignore[attr-defined]
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1",
                        help="监听地址；无认证阶段仅允许本机回环地址")
    parser.add_argument("--port", type=int, default=8780, help="监听端口")
    parser.add_argument("--snapshot", type=Path,
                        help="Runtime v1 快照JSON；省略时使用内置Mock")
    args = parser.parse_args()
    if args.host not in _LOOPBACK_HOSTS:
        parser.error("认证尚未实现，--host仅允许本机回环地址")
    if args.port < 0 or args.port > 65535:
        parser.error("--port必须位于0～65535")
    provider: RuntimeProvider = (
        FileSnapshotProvider(args.snapshot) if args.snapshot
        else MockSnapshotProvider())
    server = make_server(args.host, args.port, provider)
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

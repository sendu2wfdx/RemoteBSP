#!/usr/bin/env python3
"""RemoteBSP 本地配置与数字孪生 GUI 服务。仅使用 Python 标准库。"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import mimetypes
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote, urlparse

from firmware_builder import (
    DEFAULT_OUTPUT_ROOT,
    FirmwareBuildError,
    build_firmware_project,
    resolve_artifact,
)
from project_config import ProjectConfigError, generate_project_config
from project_contract import CURRENT_PROJECT_SCHEMA_VERSION


GUI_ROOT = Path(__file__).resolve().parent
CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
def demo_state() -> dict:
    return {
        "schema_version": 1,
        "board_name": "Mock Generic Board",
        "online": True,
        "elapsed_ms": 0,
        "source": "demo",
        "gpio": [
            {"pin": pin, "direction": "output" if pin < 8 else "input",
             "value": pin in {1, 4, 7, 10}}
            for pin in range(16)
        ],
        "motion": {
            "state": "idle", "fault": "none", "queue_depth": 0,
            "queue_capacity": 32,
            "axes": [
                {"resource_id": 200 + axis, "enabled": False,
                 "direction_positive": True, "step_level": False,
                 "steps_per_revolution": 200,
                 "position_steps": axis * 160, "emitted_steps": axis * 160}
                for axis in range(3)
            ],
        },
        "pwm": [
            {"resource_id": 0x06000000, "channel": 0,
             "frequency_hz": 20000, "duty": 4200,
             "active_low": False, "running": True},
            {"resource_id": 0x06000001, "channel": 1,
             "frequency_hz": 1000, "duty": 7500,
             "active_low": False, "running": False},
        ],
        "ws2812": {
            "supported": True,
            "strips": [{"resource_id": 300, "pixels": [
                "#ff6b35", "#ffb627", "#43d9bd", "#46a7ff",
                "#9b7bff", "#ff5ca8", "#111827", "#111827",
            ]}],
        },
    }


class GuiRequestHandler(SimpleHTTPRequestHandler):
    server_version = "RemoteBSP-GUI/0.1"

    def __init__(self, *args, directory=None, **kwargs):
        super().__init__(*args, directory=str(GUI_ROOT / "static"), **kwargs)

    @property
    def state_path(self) -> Path | None:
        return getattr(self.server, "state_path", None)

    @property
    def build_jobs(self) -> int:
        return getattr(self.server, "build_jobs", 32)

    @property
    def build_output_root(self) -> Path:
        return getattr(self.server, "build_output_root", DEFAULT_OUTPUT_ROOT)

    def _send_json(self, value: object, status: HTTPStatus = HTTPStatus.OK) -> None:
        encoded = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def _send_artifact(self, path: Path) -> None:
        content_type = mimetypes.guess_type(path.name)[0] or \
            "application/octet-stream"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(path.stat().st_size))
        self.send_header(
            "Content-Disposition", f'attachment; filename="{path.name}"')
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        with path.open("rb") as stream:
            while chunk := stream.read(65536):
                self.wfile.write(chunk)

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == "/api/health":
            self._send_json({"ok": True})
            return
        if path == "/api/catalog":
            try:
                self._send_json(json.loads(CATALOG_PATH.read_text(encoding="utf-8")))
            except (OSError, json.JSONDecodeError) as error:
                self._send_json({"error": f"读取引脚目录失败：{error}"}, HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        if path == "/api/state":
            state = demo_state()
            if self.state_path and self.state_path.exists():
                try:
                    state = json.loads(self.state_path.read_text(encoding="utf-8"))
                    state["source"] = "mock_mcu"
                except (OSError, json.JSONDecodeError) as error:
                    state["source"] = "demo"
                    state["warning"] = f"Mock 状态暂不可读：{error}"
            self._send_json(state)
            return
        if path == "/api/project/target":
            self._send_json({
                "api_version": 1,
                "mode": "static-firmware",
                "enabled": True,
                "build_enabled": True,
                "parallel_jobs": self.build_jobs,
                "project_schema_version": CURRENT_PROJECT_SCHEMA_VERSION,
                "runtime_control_enabled": False,
            })
            return
        if path.startswith("/api/project/artifacts/"):
            parts = path.strip("/").split("/")
            if len(parts) != 5:
                self._send_json({"error": "产物地址无效"}, HTTPStatus.NOT_FOUND)
                return
            try:
                artifact = resolve_artifact(
                    unquote(parts[3]), unquote(parts[4]),
                    self.build_output_root)
                self._send_artifact(artifact)
            except (OSError, FirmwareBuildError) as error:
                self._send_json({"error": str(error)}, HTTPStatus.NOT_FOUND)
            return
        if path == "/":
            self.path = "/index.html"
        super().do_GET()

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path not in ("/api/project/inspect", "/api/project/generate",
                        "/api/project/build"):
            self._send_json({"error": "未知API"}, HTTPStatus.NOT_FOUND)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 262144:
                raise ProjectConfigError("请求长度无效或超过256 KiB")
            request = json.loads(self.rfile.read(length).decode("utf-8"))
            catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
            project = request.get("project")
            if path in ("/api/project/inspect", "/api/project/generate"):
                result = generate_project_config(project, catalog)
                response = {
                    "ok": True,
                    "format": "PROJECT_INSPECTION" if
                              path == "/api/project/inspect" else "KCONFIG",
                    "board_id": result.board_id,
                    "firmware_target": result.firmware_target,
                    "resource_count": result.resource_count,
                    "project_schema_version":
                        result.project_schema_version,
                    "project_original_schema_version":
                        result.original_schema_version,
                    "project_migrations": list(result.migrations),
                    "project_sha256": result.project_sha256,
                    "summary": result.summary,
                }
                if path == "/api/project/generate":
                    config_bytes = result.config.encode("utf-8")
                    response.update({
                        "config_sha256":
                            hashlib.sha256(config_bytes).hexdigest(),
                        "byte_count": len(config_bytes),
                        "filename": f"{result.board_id}.config",
                        "config_base64": base64.b64encode(
                            config_bytes).decode("ascii"),
                    })
            else:
                result = build_firmware_project(
                    project, catalog, jobs=self.build_jobs,
                    output_root=self.build_output_root)
                response = {
                    "ok": True,
                    "format": "FIRMWARE_BUILD",
                    "build_id": result.build_id,
                    "board_id": result.board_id,
                    "firmware_target": result.firmware_target,
                    "config_sha256": result.config_sha256,
                    "project_sha256":
                        result.record.get("project_sha256"),
                    "project_schema_version": result.record.get(
                        "project_schema_version",
                        CURRENT_PROJECT_SCHEMA_VERSION),
                    "project_migrations": result.record.get(
                        "project_migrations", []),
                    "summary": result.record.get("project_summary", {}),
                    "memory": result.record.get("memory", {}),
                    "artifacts": [{
                        "filename": artifact.filename,
                        "size": artifact.size,
                        "sha256": artifact.sha256,
                        "url": "/api/project/artifacts/" +
                               quote(result.build_id) + "/" +
                               quote(artifact.filename),
                    } for artifact in result.artifacts],
                }
            self._send_json(response)
        except FirmwareBuildError as error:
            self._send_json({"ok": False, "error": str(error)},
                            HTTPStatus.UNPROCESSABLE_ENTITY)
        except (OSError, AttributeError, json.JSONDecodeError,
                UnicodeDecodeError, TypeError, ValueError,
                ProjectConfigError) as error:
            self._send_json({"ok": False, "error": str(error)},
                            HTTPStatus.BAD_REQUEST)

    def log_message(self, format: str, *args: object) -> None:
        # 保留错误日志，避免状态轮询刷满终端。
        if args and str(args[1]).startswith(("4", "5")):
            super().log_message(format, *args)


def make_server(host: str, port: int,
                state_path: Path | None, *, build_jobs: int = 32,
                build_output_root: Path = DEFAULT_OUTPUT_ROOT
                ) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((host, port), GuiRequestHandler)
    server.state_path = state_path  # type: ignore[attr-defined]
    server.build_jobs = build_jobs  # type: ignore[attr-defined]
    server.build_output_root = build_output_root  # type: ignore[attr-defined]
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="监听地址，默认仅本机")
    parser.add_argument("--port", type=int, default=8765, help="监听端口")
    parser.add_argument("--state", type=Path, help="mock_mcu --visual-state 输出的 JSON 文件")
    parser.add_argument("--build-jobs", type=int, default=32,
                        help="固件构建并行任务数，默认32")
    args = parser.parse_args()
    if args.build_jobs < 1 or args.build_jobs > 64:
        parser.error("--build-jobs必须位于1～64")
    mimetypes.add_type("text/javascript", ".js")
    server = make_server(
        args.host, args.port, args.state, build_jobs=args.build_jobs)
    print(f"RemoteBSP GUI 已启动：http://{args.host}:{args.port}")
    print("未连接 Mock 状态文件时会自动显示演示数据；按 Ctrl+C 退出。")
    print(f"固件构建使用{args.build_jobs}个并行任务。")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

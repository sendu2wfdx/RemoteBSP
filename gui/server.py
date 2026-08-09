#!/usr/bin/env python3
"""RemoteBSP 本地配置与数字孪生 GUI 服务。仅使用 Python 标准库。"""

from __future__ import annotations

import argparse
import json
import mimetypes
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


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

    def _send_json(self, value: object, status: HTTPStatus = HTTPStatus.OK) -> None:
        encoded = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

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
        if path == "/":
            self.path = "/index.html"
        super().do_GET()

    def log_message(self, format: str, *args: object) -> None:
        # 保留错误日志，避免状态轮询刷满终端。
        if args and str(args[1]).startswith(("4", "5")):
            super().log_message(format, *args)


def make_server(host: str, port: int, state_path: Path | None) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((host, port), GuiRequestHandler)
    server.state_path = state_path  # type: ignore[attr-defined]
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="监听地址，默认仅本机")
    parser.add_argument("--port", type=int, default=8765, help="监听端口")
    parser.add_argument("--state", type=Path, help="mock_mcu --visual-state 输出的 JSON 文件")
    args = parser.parse_args()
    mimetypes.add_type("text/javascript", ".js")
    server = make_server(args.host, args.port, args.state)
    print(f"RemoteBSP GUI 已启动：http://{args.host}:{args.port}")
    print("未连接 Mock 状态文件时会自动显示演示数据；按 Ctrl+C 退出。")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

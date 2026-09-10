"""Studio Runtime PWM认证代理测试。"""

from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from runtime_pwm_proxy import RuntimePwmProxy  # noqa: E402
from server import make_server  # noqa: E402


class UpstreamHandler(BaseHTTPRequestHandler):
    requests: list[tuple[str, str, bytes]] = []

    def _reply(self, document: dict) -> None:
        encoded = json.dumps(document).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self) -> None:  # noqa: N802
        self.requests.append((self.command, self.path,
                              self.headers.get("Authorization", "").encode()))
        self._reply({"api_version": "v1", "ok": True,
                     "data": {"nodes": []}})

    def do_POST(self) -> None:  # noqa: N802
        body = self.rfile.read(int(self.headers["Content-Length"]))
        self.requests.append((self.command, self.path,
                              self.headers.get("Authorization", "").encode() + b"\0" + body))
        self._reply({"api_version": "v1", "ok": True, "data": {
            "operation": {"state": "committed", "replayed": False,
                          "result": {"object_id": 7}}}})

    def log_message(self, format: str, *args: object) -> None:
        pass


class RuntimePwmProxyTest(unittest.TestCase):
    def setUp(self) -> None:
        UpstreamHandler.requests = []
        self.upstream = ThreadingHTTPServer(("127.0.0.1", 0), UpstreamHandler)
        self.upstream_thread = threading.Thread(
            target=self.upstream.serve_forever, daemon=True)
        self.upstream_thread.start()
        upstream_url = f"http://127.0.0.1:{self.upstream.server_address[1]}"
        self.proxy = RuntimePwmProxy(upstream_url, "server-only-secret")

    def tearDown(self) -> None:
        self.upstream.shutdown()
        self.upstream.server_close()
        self.upstream_thread.join(timeout=2)

    def test_rejects_non_loopback_and_url_paths(self) -> None:
        for upstream in ("http://192.0.2.1:8080", "https://127.0.0.1:8080",
                         "http://127.0.0.1:8080/api", "http://name:8080"):
            with self.assertRaises(ValueError):
                RuntimePwmProxy(upstream, "secret")
        with self.assertRaises(ValueError):
            make_server("0.0.0.0", 0, None, runtime_pwm_proxy=self.proxy)

    def test_gui_proxy_is_default_closed_and_keeps_key_server_side(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            disabled = make_server("127.0.0.1", 0, None,
                                   history_root=Path(temporary) / "disabled")
            thread = threading.Thread(target=disabled.serve_forever, daemon=True)
            thread.start()
            try:
                base = f"http://127.0.0.1:{disabled.server_address[1]}"
                target = json.load(urlopen(base + "/api/project/target"))
                self.assertFalse(target["runtime_pwm"]["available"])
                self.assertNotIn("server-only-secret", json.dumps(target))
                with self.assertRaises(HTTPError) as rejected:
                    urlopen(Request(base + "/api/runtime/pwm/configure",
                                    data=b"{}", method="POST",
                                    headers={"Content-Type": "application/json"}))
                self.assertEqual(rejected.exception.code, 503)
            finally:
                disabled.shutdown(); disabled.server_close(); thread.join(timeout=2)

    def test_only_fixed_pwm_routes_are_forwarded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            server = make_server("127.0.0.1", 0, None,
                history_root=Path(temporary), runtime_pwm_proxy=self.proxy)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                base = f"http://127.0.0.1:{server.server_address[1]}"
                target = json.load(urlopen(base + "/api/project/target"))
                capability = target["runtime_pwm"]
                self.assertTrue(capability["available"] and capability["auth_proxy"])
                self.assertNotIn("server-only-secret", json.dumps(target))
                configure = {"lease_id": "a" * 32, "node_id": "node-" + "b" * 32,
                    "resource_id": "resource-06000000", "idempotency_key": "configure-1",
                    "frequency_hz": 20000, "duty": 4200, "active_low": False}
                response = json.load(urlopen(Request(
                    base + capability["configure_path"],
                    data=json.dumps(configure).encode(), method="POST",
                    headers={"Content-Type": "application/json"})))
                self.assertEqual(response["data"]["operation"]["result"]["object_id"], 7)
                json.load(urlopen(base + capability["snapshot_path"]))
                self.assertEqual([item[:2] for item in UpstreamHandler.requests], [
                    ("POST", "/api/v1/control/pwm/configure"),
                    ("GET", "/api/v1/snapshot")])
                self.assertTrue(all(b"Bearer server-only-secret" in item[2]
                                    for item in UpstreamHandler.requests))
                bad = dict(configure, unexpected=True)
                with self.assertRaises(HTTPError) as rejected:
                    urlopen(Request(base + capability["configure_path"],
                                    data=json.dumps(bad).encode(), method="POST",
                                    headers={"Content-Type": "application/json"}))
                self.assertEqual(rejected.exception.code, 400)
                self.assertEqual(len(UpstreamHandler.requests), 2)
                with self.assertRaises(HTTPError) as rejected:
                    urlopen(Request(base + capability["configure_path"] + "?path=stop",
                                    data=json.dumps(configure).encode(), method="POST",
                                    headers={"Content-Type": "application/json"}))
                self.assertEqual(rejected.exception.code, 400)
                self.assertEqual(len(UpstreamHandler.requests), 2)
            finally:
                server.shutdown(); server.server_close(); thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()

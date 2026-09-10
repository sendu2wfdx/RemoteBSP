import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock
from urllib.error import HTTPError
from urllib.request import Request, urlopen

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from server import make_server  # noqa: E402


class WebDeploymentHttpTest(unittest.TestCase):
    @staticmethod
    def _post(base, path, body):
        request = Request(
            base + path, data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        return json.loads(urlopen(request).read())

    def test_preflight_execute_and_unknown_input_boundary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            controller = Mock()
            controller.preflight.return_value = {
                "ok": True, "format": "STUDIO_WEB_DEPLOY_PREFLIGHT_V1",
                "confirmation_token": "token", "planned_steps": []}
            controller.execute.return_value = {
                "ok": True, "format": "STUDIO_WEB_DEPLOY_RESULT_V1",
                "verified": True}
            server = make_server(
                "127.0.0.1", 0, None, history_root=root / "history",
                deployment_controller=controller)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"
            try:
                target = json.loads(urlopen(
                    base + "/api/project/target").read())
                self.assertTrue(target["stlink_deployment_enabled"])
                self._post(base, "/api/deployment/preflight", {
                    "build_id": "weact-g431-core-v10-01234567",
                    "expected_uuid": "ab" * 16})
                controller.preflight.assert_called_once()
                self._post(base, "/api/deployment/execute", {
                    "confirmation_token": "token",
                    "confirmation": "FLASH_VERIFIED_FIRMWARE",
                    "flash_timeout": 120, "reconnect_timeout": 10,
                    "poll_interval": .25})
                controller.execute.assert_called_once()
                with self.assertRaises(HTTPError) as caught:
                    self._post(base, "/api/deployment/preflight", {
                        "build_id": "weact-g431-core-v10-01234567",
                        "expected_uuid": "ab" * 16,
                        "command": "erase-all"})
                self.assertEqual(caught.exception.code, 400)
                self.assertEqual(controller.preflight.call_count, 1)
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()

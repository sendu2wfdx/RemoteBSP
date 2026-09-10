"""Web USB Katapult 两阶段部署的软件闭环测试。"""
import hashlib, json, sys, tempfile, threading, unittest
from pathlib import Path
from unittest.mock import Mock
from urllib.error import HTTPError
from urllib.request import Request, urlopen

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))
from firmware_deployment import DeviceIdentity  # noqa: E402
from server import make_server  # noqa: E402
from web_deployment import (  # noqa: E402
    WEB_DEPLOY_CONFIRMATION, WebUsbKatapultDeploymentController)

class Reader:
    def __init__(self, identity): self.identity = identity
    def read_identity(self): return self.identity

class WebUsbKatapultDeploymentTest(unittest.TestCase):
    @staticmethod
    def _build(root):
        build_id = "weact-g431-core-v10-usb-katapult-01234567"
        directory = root / build_id; directory.mkdir()
        files = {"firmware.bin": b"usb-katapult-app",
                 "firmware.config": b"CONFIG_APP_LAYOUT_KATAPULT_8K=y\n"}
        artifacts = []
        for name, content in files.items():
            (directory / name).write_bytes(content)
            artifacts.append({"filename": name, "size": len(content),
                              "sha256": hashlib.sha256(content).hexdigest()})
        record = {"schema_version": 1, "build_id": build_id,
            "board_id": "weact-g431-core-v10", "project_sha256": "a" * 64,
            "config_sha256": "b" * 64, "firmware_input_sha256": "c" * 64,
            "artifacts": artifacts}
        (directory / "build-record.json").write_text(json.dumps(record))
        return build_id, DeviceIdentity(record["board_id"], "a" * 64,
            "b" * 64, "c" * 64, "ab" * 16)

    def test_two_phase_fixed_device_identity_and_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); build_id, identity = self._build(root)
            tool = root / "flashtool.py"; tool.write_text("# fixed\n")
            device = "/dev/serial/by-id/usb-katapult_1d50_6177-ABC"
            commands = []
            controller = WebUsbKatapultDeploymentController(
                output_root=root, record_root=root / "records",
                usb_device=device, flashtool=tool,
                reader_factory=lambda _: Reader(identity),
                runner=lambda command, timeout: commands.append(tuple(command)))
            preflight = controller.preflight(build_id=build_id,
                                             expected_uuid="ab" * 16)
            self.assertEqual(commands, [])
            result = controller.execute(
                confirmation_token=preflight["confirmation_token"],
                confirmation=WEB_DEPLOY_CONFIRMATION, flash_timeout=30,
                reconnect_timeout=2, poll_interval=.1)
            self.assertTrue(result["verified"])
            self.assertEqual(commands[0][-4:], ("-d", device, "-f",
                str(root / build_id / "firmware.bin")))
            self.assertTrue((root / "records" /
                             result["deployment_record_filename"]).is_file())

    def test_http_rejects_browser_selected_path_or_command(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); controller = Mock()
            controller.usb_device = "/dev/serial/by-id/fixed-device"
            controller.preflight.return_value = {"ok": True}
            server = make_server("127.0.0.1", 0, None,
                history_root=root / "history",
                usb_katapult_deployment_controller=controller)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start(); base = f"http://127.0.0.1:{server.server_port}"
            def post(body):
                return urlopen(Request(base + "/api/deployment/usb-katapult/preflight",
                    data=json.dumps(body).encode(), method="POST",
                    headers={"Content-Type": "application/json"}))
            try:
                target = json.load(urlopen(base + "/api/project/target"))
                self.assertTrue(target["usb_katapult_deployment_enabled"])
                json.load(post({"build_id": "weact-g431-core-v10-usb-katapult-01234567",
                                "expected_uuid": "ab" * 16}))
                for extra in ({"usb_device": "/dev/ttyACM0"},
                              {"flashtool": "/tmp/evil.py"}, {"command": "-q"}):
                    with self.assertRaises(HTTPError) as rejected:
                        post({"build_id": "weact-g431-core-v10-usb-katapult-01234567",
                              "expected_uuid": "ab" * 16, **extra})
                    self.assertEqual(rejected.exception.code, 400)
            finally:
                server.shutdown(); server.server_close(); thread.join(timeout=2)

if __name__ == "__main__": unittest.main()

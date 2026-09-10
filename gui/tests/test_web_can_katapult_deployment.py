"""Web CAN Katapult 两阶段部署的软件闭环测试。"""

import hashlib
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

from firmware_deployment import DeviceIdentity, FirmwareDeploymentError  # noqa: E402
from web_deployment import (  # noqa: E402
    WEB_DEPLOY_CONFIRMATION, WebCanKatapultDeploymentController)
from server import make_server  # noqa: E402


class Reader:
    def __init__(self, identity): self.identity = identity
    def read_identity(self): return self.identity


class WebCanKatapultDeploymentTest(unittest.TestCase):
    @staticmethod
    def _build(root: Path):
        build_id = "weact-g431-core-v10-katapult-01234567"
        directory = root / build_id
        directory.mkdir()
        files = {"firmware.bin": b"katapult-app",
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
        identity = DeviceIdentity(record["board_id"], "a" * 64, "b" * 64,
                                  "c" * 64, "ab" * 16)
        return build_id, identity

    def test_two_phase_fixed_plan_identity_and_self_hashed_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_id, identity = self._build(root)
            flashtool = root / "flashtool.py"
            flashtool.write_text("# fixed test tool\n")
            commands = []
            controller = WebCanKatapultDeploymentController(
                output_root=root, record_root=root / "records",
                can_interface="can0", flashtool=flashtool,
                reader_factory=lambda _: Reader(identity),
                runner=lambda command, timeout: commands.append((command, timeout)))
            preflight = controller.preflight(build_id=build_id,
                expected_uuid="ab" * 16, can_interface="can0",
                katapult_uuid="1234abcdef")
            self.assertFalse(preflight["firmware_flash_performed"])
            self.assertEqual(commands, [])
            result = controller.execute(
                confirmation_token=preflight["confirmation_token"],
                confirmation=WEB_DEPLOY_CONFIRMATION, flash_timeout=30,
                reconnect_timeout=2, poll_interval=.1)
            self.assertTrue(result["verified"])
            self.assertEqual(commands[0][0][0], "python3")
            self.assertEqual(commands[0][0][2:6],
                             ("-i", "can0", "-u", "1234abcdef"))
            record = result["deployment_record"]
            self.assertEqual(len(record["record_sha256"]), 64)
            self.assertEqual(len(result["deployment_record_sha256"]), 64)
            self.assertTrue((root / "records" /
                             result["deployment_record_filename"]).is_file())
            with self.assertRaisesRegex(FirmwareDeploymentError, "已使用"):
                controller.execute(confirmation_token=preflight["confirmation_token"],
                    confirmation=WEB_DEPLOY_CONFIRMATION, flash_timeout=30,
                    reconnect_timeout=2, poll_interval=.1)

    def test_rejects_interface_uuid_layout_and_tool_path_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_id, identity = self._build(root)
            flashtool = root / "flashtool.py"; flashtool.write_text("# test\n")
            controller = WebCanKatapultDeploymentController(
                output_root=root, record_root=root / "records",
                can_interface="can0", flashtool=flashtool,
                reader_factory=lambda _: Reader(identity),
                runner=lambda *_: self.fail("预检失败不得烧录"))
            for values in ({"can_interface": "can1", "katapult_uuid": "1234abcdef"},
                           {"can_interface": "can0", "katapult_uuid": "-q"}):
                with self.assertRaises(FirmwareDeploymentError):
                    controller.preflight(build_id=build_id,
                        expected_uuid="ab" * 16, **values)
            (root / build_id / "firmware.config").write_text("CONFIG_APP_LAYOUT_STANDARD=y\n")
            with self.assertRaises(FirmwareDeploymentError):
                controller.preflight(build_id=build_id, expected_uuid="ab" * 16,
                    can_interface="can0", katapult_uuid="1234abcdef")

    def test_http_is_default_closed_and_rejects_extra_command_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            controller = Mock()
            controller.can_interface = "can0"
            controller.preflight.return_value = {"ok": True,
                "format": "STUDIO_WEB_CAN_KATAPULT_PREFLIGHT_V1"}
            server = make_server("127.0.0.1", 0, None,
                history_root=root / "history",
                can_katapult_deployment_controller=controller)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"
            def post(body):
                return urlopen(Request(base + "/api/deployment/can-katapult/preflight",
                    data=json.dumps(body).encode(), method="POST",
                    headers={"Content-Type": "application/json"}))
            try:
                target = json.load(urlopen(base + "/api/project/target"))
                self.assertTrue(target["can_katapult_deployment_enabled"])
                self.assertEqual(target["can_katapult_deployment"]["can_interface"],
                                 "can0")
                json.load(post({"build_id": "weact-g431-core-v10-katapult-01234567",
                    "expected_uuid": "ab" * 16, "can_interface": "can0",
                    "katapult_uuid": "1234abcdef"}))
                controller.preflight.assert_called_once()
                with self.assertRaises(HTTPError) as rejected:
                    post({"build_id": "weact-g431-core-v10-katapult-01234567",
                        "expected_uuid": "ab" * 16, "can_interface": "can0",
                        "katapult_uuid": "1234abcdef", "command": "-q"})
                self.assertEqual(rejected.exception.code, 400)
                self.assertEqual(controller.preflight.call_count, 1)
            finally:
                server.shutdown(); server.server_close(); thread.join(timeout=2)


if __name__ == "__main__": unittest.main()

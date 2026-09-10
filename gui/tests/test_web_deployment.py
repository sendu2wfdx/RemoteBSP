import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from firmware_deployment import DeviceIdentity, FirmwareDeploymentError  # noqa: E402
from web_deployment import (  # noqa: E402
    WEB_DEPLOY_CONFIRMATION, WebDeploymentController,
)


class Reader:
    def __init__(self, identity):
        self.identity = identity

    def read_identity(self):
        return self.identity


class WebDeploymentTest(unittest.TestCase):
    def _build(self, root: Path):
        build_id = "weact-g431-core-v10-01234567"
        directory = root / build_id
        directory.mkdir()
        files = {"firmware.bin": b"bin", "firmware.elf": b"elf"}
        artifacts = []
        for filename, content in files.items():
            (directory / filename).write_bytes(content)
            artifacts.append({"filename": filename, "size": len(content),
                              "sha256": hashlib.sha256(content).hexdigest()})
        record = {"schema_version": 1, "build_id": build_id,
                  "board_id": "weact-g431-core-v10",
                  "project_sha256": "a" * 64,
                  "config_sha256": "b" * 64,
                  "firmware_input_sha256": "c" * 64,
                  "artifacts": artifacts}
        (directory / "build-record.json").write_text(
            json.dumps(record), encoding="utf-8")
        return build_id, DeviceIdentity(
            record["board_id"], "a" * 64, "b" * 64, "c" * 64,
            "ab" * 16)

    def test_two_phase_flow_uses_once_token_and_writes_verified_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_id, identity = self._build(root)
            flashes = []
            controller = WebDeploymentController(
                output_root=root, record_root=root / "records",
                reader_factory=lambda expected_uuid: Reader(identity),
                runner=lambda command, timeout: flashes.append(
                    (tuple(command), timeout)))
            preflight = controller.preflight(
                build_id=build_id, expected_uuid="ab" * 16)
            self.assertTrue(preflight["hardware_access_performed"])
            self.assertFalse(preflight["firmware_flash_performed"])
            self.assertEqual(len(flashes), 0)
            result = controller.execute(
                confirmation_token=preflight["confirmation_token"],
                confirmation=WEB_DEPLOY_CONFIRMATION, flash_timeout=30,
                reconnect_timeout=2, poll_interval=.1)
            self.assertTrue(result["verified"])
            self.assertEqual(len(flashes), 1)
            self.assertTrue((root / "records" /
                             result["deployment_record_filename"]).is_file())
            with self.assertRaisesRegex(FirmwareDeploymentError, "已使用"):
                controller.execute(
                    confirmation_token=preflight["confirmation_token"],
                    confirmation=WEB_DEPLOY_CONFIRMATION, flash_timeout=30,
                    reconnect_timeout=2, poll_interval=.1)

    def test_wrong_confirmation_uuid_board_and_unknown_fields_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_id, identity = self._build(root)
            controller = WebDeploymentController(
                output_root=root, record_root=root / "records",
                reader_factory=lambda expected_uuid: Reader(identity),
                runner=lambda *_: self.fail("确认失败时不得烧录"))
            with self.assertRaisesRegex(FirmwareDeploymentError, "UUID"):
                controller.preflight(build_id=build_id,
                                     expected_uuid="not-a-uuid")
            preflight = controller.preflight(
                build_id=build_id, expected_uuid="ab" * 16)
            with self.assertRaisesRegex(FirmwareDeploymentError, "确认短语"):
                controller.execute(
                    confirmation_token=preflight["confirmation_token"],
                    confirmation="yes", flash_timeout=30,
                    reconnect_timeout=2, poll_interval=.1)


if __name__ == "__main__":
    unittest.main()

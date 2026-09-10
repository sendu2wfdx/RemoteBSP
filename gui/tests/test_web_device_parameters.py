import base64
import json
import sys
import tempfile
import unittest
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from device_parameters import DeviceParameterError, DeviceParameterManager  # noqa: E402
from parameter_audit import ParameterAuditStore  # noqa: E402
from test_device_parameters import FakeRunner, UUID  # noqa: E402
from web_device_parameters import (  # noqa: E402
    WEB_PARAMETER_CONFIRMATION, WebDeviceParameterController,
)


class WebDeviceParameterTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        key = self.root / "audit.key"
        key.write_bytes(b"k" * 32)
        self.runner = FakeRunner()
        self.manager = DeviceParameterManager(
            "/tmp/toolbusd.sock", 7, runner=self.runner)
        self.audit = ParameterAuditStore(self.root / "audit", key)
        self.controller = WebDeviceParameterController(
            self.manager, self.audit)

    def tearDown(self):
        self.temporary.cleanup()

    def test_write_preflight_once_confirmation_and_redacted_audit(self):
        preflight = self.controller.preflight_write(
            expected_uuid=UUID, expected_generation=4,
            parameter_id=0x100,
            value_base64=base64.b64encode(b"new-secret").decode())
        self.assertFalse(preflight["hardware_write_performed"])
        self.assertNotIn("new-secret", json.dumps(preflight))
        result = self.controller.execute(
            confirmation_token=preflight["confirmation_token"],
            confirmation=WEB_PARAMETER_CONFIRMATION)
        self.assertEqual(result["generation_after"], 5)
        audit_file = next((self.root / "audit").glob("audit-*.json"))
        self.assertNotIn("new-secret", audit_file.read_text(encoding="utf-8"))
        with self.assertRaisesRegex(DeviceParameterError, "已使用"):
            self.controller.execute(
                confirmation_token=preflight["confirmation_token"],
                confirmation=WEB_PARAMETER_CONFIRMATION)

    def test_stale_generation_cross_uuid_and_wrong_confirmation_fail_closed(self):
        value = base64.b64encode(b"new").decode()
        with self.assertRaisesRegex(DeviceParameterError, "UUID"):
            self.controller.preflight_write(
                expected_uuid="cd" * 16, expected_generation=4,
                parameter_id=0x100, value_base64=value)
        preflight = self.controller.preflight_write(
            expected_uuid=UUID, expected_generation=4,
            parameter_id=0x100, value_base64=value)
        self.runner.generation = 5
        with self.assertRaisesRegex(DeviceParameterError, "代数已变化"):
            self.controller.execute(
                confirmation_token=preflight["confirmation_token"],
                confirmation=WEB_PARAMETER_CONFIRMATION)
        self.assertEqual(self.runner.value, b"old")

        fresh = self.controller.preflight_write(
            expected_uuid=UUID, expected_generation=5,
            parameter_id=0x100, value_base64=value)
        with self.assertRaisesRegex(DeviceParameterError, "确认短语"):
            self.controller.execute(
                confirmation_token=fresh["confirmation_token"],
                confirmation="yes")
        self.assertEqual(self.runner.value, b"old")

    def test_restore_preflight_validates_backup_digest_and_executes_manager(self):
        backup = self.manager.backup()
        self.runner.value = b"changed"
        self.runner.generation = 5
        preflight = self.controller.preflight_restore(
            expected_uuid=UUID, expected_generation=5, backup=backup)
        result = self.controller.execute(
            confirmation_token=preflight["confirmation_token"],
            confirmation=WEB_PARAMETER_CONFIRMATION)
        self.assertEqual(result["generation_after"], 6)
        self.assertEqual(self.runner.value, b"old")
        changed = json.loads(json.dumps(backup))
        changed["parameters"][0]["value_base64"] = "YmFk"
        with self.assertRaisesRegex(DeviceParameterError, "完整性"):
            self.controller.preflight_restore(
                expected_uuid=UUID, expected_generation=6, backup=changed)


if __name__ == "__main__":
    unittest.main()

import hashlib
import itertools
import json
import sys
import tempfile
import unittest
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from firmware_deployment import (  # noqa: E402
    DeviceIdentity, FirmwareDeploymentError, deploy_stlink,
    JsonIdentityFileReader, expected_identity, make_stlink_plan)


class Reader:
    def __init__(self, values):
        self.values = iter(values)

    def read_identity(self):
        value = next(self.values)
        if isinstance(value, Exception):
            raise value
        return value


class FirmwareDeploymentTest(unittest.TestCase):
    def test_json_identity_reader_is_strict_and_bounded(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "identity.json"
            identity = {
                "board_id": "weact-g431-core-v10",
                "project_sha256": "a" * 64,
                "config_sha256": "b" * 64,
                "firmware_identity_sha256": "c" * 64,
                "device_uuid": "uuid-1",
            }
            path.write_text(json.dumps(identity), encoding="utf-8")
            observed = JsonIdentityFileReader(path).read_identity()
            self.assertEqual(observed.device_uuid, "uuid-1")
            path.write_text('{"board_id":"x","board_id":"y"}',
                            encoding="utf-8")
            with self.assertRaisesRegex(FirmwareDeploymentError, "重复字段"):
                JsonIdentityFileReader(path).read_identity()
            path.write_bytes(b" " * (JsonIdentityFileReader.MAX_BYTES + 1))
            with self.assertRaisesRegex(FirmwareDeploymentError, "16 KiB"):
                JsonIdentityFileReader(path).read_identity()

    def test_non_finite_wait_is_rejected_before_flash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_id = self._build(root)
            called = []
            with self.assertRaisesRegex(FirmwareDeploymentError,
                                        "重连等待参数无效"):
                deploy_stlink(
                    build_id, Reader([]), output_root=root,
                    reconnect_timeout=float("nan"),
                    runner=lambda command, timeout: called.append(command))
            self.assertEqual(called, [])

    def _build(self, root: Path):
        build_id = "weact-test-01234567"
        directory = root / build_id
        directory.mkdir()
        files = {"firmware.bin": b"binary", "firmware.elf": b"elf"}
        artifacts = []
        for name, content in files.items():
            (directory / name).write_bytes(content)
            artifacts.append({"filename": name, "size": len(content),
                              "sha256": hashlib.sha256(content).hexdigest()})
        record = {
            "schema_version": 1, "build_id": build_id,
            "board_id": "weact-g431-core-v10",
            "project_sha256": "a" * 64, "config_sha256": "b" * 64,
            "firmware_input_sha256": "d" * 64,
            "artifacts": artifacts,
        }
        (directory / "build-record.json").write_text(
            json.dumps(record), encoding="utf-8")
        return build_id

    def test_plan_uses_verified_artifact_and_no_shell_tokens(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            plan = make_stlink_plan(build_id, output_root=root,
                                    probe_serial="ABC-123")
            self.assertEqual(plan.backend, "stlink-openocd")
            self.assertIn("target/stm32g4x.cfg", plan.command)
            self.assertIn("verify reset exit", plan.command[-1])
            self.assertNotIn(";", "".join(plan.command))

    def test_rejects_probe_serial_injection(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            with self.assertRaisesRegex(FirmwareDeploymentError, "序列号"):
                make_stlink_plan(build_id, output_root=root,
                                 probe_serial="x; shutdown")

    def test_tampered_firmware_is_rejected_before_flash(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            (root / build_id / "firmware.elf").write_bytes(b"tampered")
            with self.assertRaisesRegex(FirmwareDeploymentError, "不可用于部署"):
                make_stlink_plan(build_id, output_root=root)

    def test_flash_retry_and_four_way_identity_verification(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            expected = expected_identity(build_id, output_root=root)
            observed = DeviceIdentity(
                expected.board_id, expected.project_sha256,
                expected.config_sha256, expected.firmware_identity_sha256,
                "uuid-1")
            commands = []
            result = deploy_stlink(
                build_id, Reader([TimeoutError("booting"), observed]),
                output_root=root, runner=lambda command, timeout:
                commands.append((tuple(command), timeout)), sleeper=lambda _: None)
            self.assertTrue(result.verified)
            self.assertEqual(result.attempts, 2)
            self.assertEqual(len(commands), 1)

    def test_identity_mismatch_never_becomes_success(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            expected = expected_identity(build_id, output_root=root)
            bad = DeviceIdentity(
                expected.board_id, expected.project_sha256, "c" * 64,
                expected.firmware_identity_sha256, "uuid-1")
            with self.assertRaisesRegex(FirmwareDeploymentError, "身份核对"):
                deploy_stlink(
                    build_id, Reader(itertools.repeat(bad)), output_root=root,
                    reconnect_timeout=.01, poll_interval=.001,
                    runner=lambda command, timeout: None, sleeper=lambda _: None)


if __name__ == "__main__":
    unittest.main()

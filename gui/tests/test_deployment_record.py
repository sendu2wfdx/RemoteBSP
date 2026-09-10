import copy
import hashlib
import json
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from deployment_record import (  # noqa: E402
    create_deployment_record, load_deployment_record,
    validate_deployment_record,
)
from firmware_deployment import (  # noqa: E402
    DeploymentResult, DeviceIdentity, FirmwareDeploymentError,
    FirmwareIdentity,
)


class DeploymentRecordTest(unittest.TestCase):
    def _fixture(self, root: Path, *, verified=True):
        build_id = "weact-g431-core-v10-01234567"
        directory = root / build_id
        directory.mkdir()
        files = {"firmware.elf": b"verified-elf", "firmware.bin": b"bin"}
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
        expected = FirmwareIdentity(
            record["board_id"], "a" * 64, "b" * 64, "c" * 64)
        observed = DeviceIdentity(
            record["board_id"], "a" * 64, "b" * 64, "c" * 64,
            "ab" * 16)
        return DeploymentResult(build_id, "stlink-openocd", expected,
                                observed, 3, verified)

    def test_verified_result_creates_self_hashed_record_with_untrusted_time(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = create_deployment_record(
                self._fixture(root), output_root=root,
                clock=lambda: datetime(2026, 9, 10, 8, 30,
                                       tzinfo=timezone.utc))
            self.assertEqual(result.record["status"],
                             "firmware_flash_verified")
            self.assertEqual(result.record["recorded_time"], {
                "value_utc": "2026-09-10T08:30:00Z",
                "source": "host_system_clock", "trusted": False,
                "note": "主机系统时钟未经过可信时间源证明；仅用于排序，不作为审计时间戳。",
            })
            self.assertEqual(result.record["deployment"]["attempts"], 3)
            self.assertEqual(result.sha256,
                             hashlib.sha256(result.content).hexdigest())
            self.assertEqual(result.record["source_evidence"]
                             ["flashed_artifact"]["size"], 12)

    def test_failed_or_mismatched_result_never_creates_success_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            failed = self._fixture(root, verified=False)
            with self.assertRaisesRegex(FirmwareDeploymentError, "核验成功"):
                create_deployment_record(failed, output_root=root)
            mismatched = DeploymentResult(
                failed.build_id, failed.backend, failed.expected,
                DeviceIdentity(failed.observed.board_id, "d" * 64,
                               "b" * 64, "c" * 64, "ab" * 16), 1, True)
            with self.assertRaisesRegex(FirmwareDeploymentError, "身份不一致"):
                create_deployment_record(mismatched, output_root=root)

    def test_tamper_duplicate_oversize_and_non_regular_file_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            made = create_deployment_record(self._fixture(root), output_root=root)
            tampered = copy.deepcopy(made.record)
            tampered["deployment"]["attempts"] = 4
            with self.assertRaisesRegex(FirmwareDeploymentError, "自哈希不匹配"):
                validate_deployment_record(tampered)
            path = root / "record.json"
            path.write_text('{"schema_version":1,"schema_version":1}',
                            encoding="utf-8")
            with self.assertRaisesRegex(FirmwareDeploymentError, "重复字段"):
                load_deployment_record(path)
            path.write_bytes(b"x" * (32 * 1024 + 1))
            with self.assertRaisesRegex(FirmwareDeploymentError, "32 KiB"):
                load_deployment_record(path)
            directory = root / "directory"
            directory.mkdir()
            with self.assertRaisesRegex(FirmwareDeploymentError, "普通文件"):
                load_deployment_record(directory)


if __name__ == "__main__":
    unittest.main()

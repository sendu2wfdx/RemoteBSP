import copy
import hashlib
import json
import sys
import unittest
from pathlib import Path


GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from production_record import (  # noqa: E402
    MAX_BUILD_ARTIFACTS,
    MAX_BUILD_RECORD_BYTES,
    generate_production_record,
    production_record_response,
)
from project_artifacts import generate_project_reports  # noqa: E402
from project_config import ProjectConfigError  # noqa: E402
from production_batch import export_production_batch  # noqa: E402


CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"


def default_project(board: dict) -> dict:
    return {
        "schema_version": 2,
        "board_id": board["id"],
        "gpio": {"resources": copy.deepcopy(board["gpio_defaults"])},
        "uart": {"ports": copy.deepcopy(board.get("uart_defaults", []))},
        "motion": {"axes": copy.deepcopy(board["motion_defaults"])},
        "pwm": {"channels": copy.deepcopy([
            item for item in board["waveform"]["pwm"] if item["enabled"]])},
        "timed_bitstream": {"ws2812": copy.deepcopy([
            item for item in board["waveform"]["ws2812"]
            if item["enabled"]])},
        "i2c": {"buses": [], "devices": []},
        "spi": {"buses": [], "devices": []},
    }


def complete_build_record(project: dict, catalog: dict) -> dict:
    reports = generate_project_reports(project, catalog)
    config_sha256 = "c" * 64
    artifact_names = (
        "studio-project.json", "firmware.config", "build.log",
        "firmware.elf", "firmware.bin", "firmware.hex", "firmware.map",
    )
    artifacts = []
    for index, filename in enumerate(artifact_names):
        sha256 = (reports.project_sha256 if filename == "studio-project.json"
                  else config_sha256 if filename == "firmware.config"
                  else f"{index + 1:064x}")
        artifacts.append({"filename": filename, "size": index + 1,
                          "sha256": sha256})
    return {
        "schema_version": 1,
        "build_id": "mellow-fly-d5-v1-0123456789abcdef",
        "built_at_utc": "2026-09-09T00:00:00+00:00",
        "board_id": reports.board_id,
        "firmware_target": "mellow-fly-d5",
        "resource_count": reports.resource_count,
        "project_sha256": reports.project_sha256,
        "config_sha256": config_sha256,
        "firmware_input_sha256": "c" * 64,
        "git_revision": "a" * 40,
        "git_dirty": False,
        "parallel_jobs": 32,
        "tool_versions": {
            "cmake": "cmake version 3.28.3",
            "ninja": "1.11.1",
            "arm_none_eabi_gcc": "arm-none-eabi-gcc 12.3.1",
        },
        "memory": {
            "ram": {"used_bytes": 1024, "capacity_bytes": 20480,
                    "used_percent": 5.0},
            "flash": {"used_bytes": 4096, "capacity_bytes": 129024,
                      "used_percent": 3.17},
        },
        "artifacts": artifacts,
    }


class ProductionRecordTest(unittest.TestCase):
    def setUp(self):
        self.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.board = self.catalog["boards"][0]
        self.project = default_project(self.board)

    def test_design_only_record_is_deterministic_and_never_claims_hardware(self):
        first = generate_production_record(self.project, self.catalog)
        second = generate_production_record(self.project, self.catalog)
        self.assertEqual(first.content, second.content)
        self.assertEqual(first.sha256, second.sha256)
        record = first.record
        self.assertEqual(record["schema_version"], 1)
        self.assertEqual(record["status"], "design_only")
        self.assertEqual(record["execution_status"]["software_build"],
                         "not_built")
        self.assertEqual(record["execution_status"]["firmware_flash"],
                         "not_performed")
        self.assertEqual(record["execution_status"]["hardware_validation"],
                         "not_performed")
        self.assertFalse(record["execution_status"]
                         ["hardware_connected_by_this_operation"])
        self.assertGreater(len(record["missing_or_invalid_fields"]), 10)
        self.assertRegex(record["design_evidence"]["project_sha256"],
                         r"^[0-9a-f]{64}$")
        self.assertRegex(record["design_evidence"]["resource_set_sha256"],
                         r"^[0-9a-f]{64}$")
        self.assertRegex(record["design_evidence"]
                         ["project_reports_archive"]["sha256"],
                         r"^[0-9a-f]{64}$")

    def test_complete_build_record_links_git_toolchain_and_artifact_hashes(self):
        build_record = complete_build_record(self.project, self.catalog)
        source = json.dumps(build_record, sort_keys=True).encode()
        source_sha256 = hashlib.sha256(source).hexdigest()
        result = generate_production_record(
            self.project, self.catalog, build_record=build_record,
            build_record_sha256=source_sha256)
        record = result.record
        self.assertEqual(record["status"], "software_build_recorded")
        self.assertEqual(record["missing_or_invalid_fields"], [])
        self.assertTrue(all(value is True for value in
                            record["evidence_checks"].values()))
        build = record["software_build_evidence"]
        self.assertEqual(build["source_record_sha256"], source_sha256)
        self.assertEqual(build["git"]["revision"], "a" * 40)
        self.assertEqual(build["tool_versions"]["ninja"], "1.11.1")
        self.assertEqual(len(build["artifacts"]), 7)
        self.assertEqual(record["execution_status"]["firmware_flash"],
                         "not_performed")
        response = production_record_response(result)
        self.assertEqual(response["format"], "PRODUCTION_RECORD_V1")
        self.assertRegex(response["record_sha256"], r"^[0-9a-f]{64}$")

    def test_verified_deployment_promotes_record_and_batch_compatible_status(self):
        build_record = complete_build_record(self.project, self.catalog)
        build_hash = "e" * 64
        deployment = {
            "schema_version": 1,
            "format": "REMOTEBSP_DEPLOYMENT_RECORD_V1",
            "status": "firmware_flash_verified",
            "deployment": {
                "build_id": build_record["build_id"],
                "board_id": build_record["board_id"],
                "device_uuid": "ab" * 16,
                "backend": "stlink-openocd", "attempts": 1,
                "verified": True,
            },
            "observed_identity": {
                "project_sha256": build_record["project_sha256"],
                "config_sha256": build_record["config_sha256"],
                "firmware_identity_sha256": "c" * 64,
            },
            "source_evidence": {
                "build_record": {"filename": "build-record.json",
                                 "size": 123, "sha256": build_hash},
                "flashed_artifact": {"filename": "firmware.elf",
                                     "size": 456, "sha256": "f" * 64},
            },
            "recorded_time": {
                "value_utc": "2026-09-10T08:30:00Z",
                "source": "host_system_clock", "trusted": False,
                "note": "主机系统时钟未经过可信时间源证明；仅用于排序，不作为审计时间戳。",
            },
            "declaration": "已完成写入及身份核验；不证明外设功能。",
        }
        unsigned = (json.dumps(deployment, ensure_ascii=False, allow_nan=False,
                               sort_keys=True, indent=2) + "\n").encode()
        deployment["record_sha256"] = hashlib.sha256(unsigned).hexdigest()
        result = generate_production_record(
            self.project, self.catalog, build_record=build_record,
            build_record_sha256=build_hash, deployment_record=deployment)
        self.assertEqual(result.record["status"],
                         "firmware_deployed_verified")
        self.assertEqual(result.record["execution_status"]["firmware_flash"],
                         "performed_and_verified")
        self.assertTrue(result.record["evidence_checks"]
                        ["deployment_matches_build_and_project"])
        batch = export_production_batch(
            batch_id="verified-deployment-001", name="已核验部署",
            note="不含跨板测试", production_records=[result.record],
            comparison_exports=[])
        self.assertTrue(batch.validation["valid"])

        tampered = copy.deepcopy(deployment)
        tampered["deployment"]["attempts"] = 2
        with self.assertRaisesRegex(ProjectConfigError, "自哈希不匹配"):
            generate_production_record(
                self.project, self.catalog, build_record=build_record,
                build_record_sha256=build_hash,
                deployment_record=tampered)

    def test_missing_fields_and_mismatch_are_not_presented_as_success(self):
        incomplete = generate_production_record(
            self.project, self.catalog, build_record={})
        self.assertEqual(incomplete.record["status"], "build_incomplete")
        self.assertGreater(len(incomplete.record["missing_or_invalid_fields"]),
                           10)

        build_record = complete_build_record(self.project, self.catalog)
        build_record["project_sha256"] = "f" * 64
        build_record["artifacts"][0]["sha256"] = "f" * 64
        mismatch = generate_production_record(
            self.project, self.catalog, build_record=build_record)
        self.assertEqual(mismatch.record["status"], "build_mismatch")
        self.assertFalse(mismatch.record["evidence_checks"]
                         ["project_sha256_matches_build"])
        self.assertTrue(any("工程哈希" in warning
                            for warning in mismatch.record["warnings"]))

    def test_dirty_and_unknown_toolchain_are_explicit(self):
        build_record = complete_build_record(self.project, self.catalog)
        build_record["git_dirty"] = True
        build_record["tool_versions"]["ninja"] = "unknown"
        result = generate_production_record(
            self.project, self.catalog, build_record=build_record)
        self.assertEqual(result.record["status"], "build_incomplete")
        self.assertTrue(any("未提交改动" in warning
                            for warning in result.record["warnings"]))
        self.assertTrue(any(item["field"].endswith("tool_versions.ninja")
                            for item in result.record
                            ["missing_or_invalid_fields"]))

        invalid = complete_build_record(self.project, self.catalog)
        invalid["memory"]["ram"]["used_bytes"] = "1024"
        invalid["artifacts"][0]["filename"] = "../studio-project.json"
        invalid_result = generate_production_record(
            self.project, self.catalog, build_record=invalid)
        invalid_fields = {item["field"] for item in invalid_result.record
                          ["missing_or_invalid_fields"]}
        self.assertIn("build_record.memory.ram.used_bytes", invalid_fields)
        self.assertIn("build_record.artifacts[0].filename", invalid_fields)
        self.assertEqual(invalid_result.record["status"], "build_incomplete")

    def test_build_record_bounds_and_future_schema_are_rejected(self):
        oversized = {"padding": "x" * MAX_BUILD_RECORD_BYTES}
        with self.assertRaisesRegex(ProjectConfigError, "128 KiB"):
            generate_production_record(
                self.project, self.catalog, build_record=oversized)

        too_many = complete_build_record(self.project, self.catalog)
        too_many["artifacts"] = [
            {"filename": f"file-{index}.bin", "size": 1,
             "sha256": f"{index + 1:064x}"}
            for index in range(MAX_BUILD_ARTIFACTS + 1)]
        with self.assertRaisesRegex(ProjectConfigError, "产物超过"):
            generate_production_record(
                self.project, self.catalog, build_record=too_many)

        future = complete_build_record(self.project, self.catalog)
        future["schema_version"] = 2
        with self.assertRaisesRegex(ProjectConfigError, "高于当前支持"):
            generate_production_record(
                self.project, self.catalog, build_record=future)


if __name__ == "__main__":
    unittest.main()

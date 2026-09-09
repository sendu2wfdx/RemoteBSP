import copy
import hashlib
import io
import json
import sys
import unittest
import zipfile
from pathlib import Path


GUI_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = GUI_ROOT.parent
sys.path.insert(0, str(GUI_ROOT))
sys.path.insert(0, str(GUI_ROOT / "tests"))

from comparison_export import export_project_comparison  # noqa: E402
from production_batch import (  # noqa: E402
    MAX_BATCH_COMPARISONS,
    MAX_BATCH_MANIFEST_BYTES,
    MAX_BATCH_RECORDS,
    export_production_batch,
    validate_production_batch_manifest,
)
from production_record import generate_production_record  # noqa: E402
from project_config import ProjectConfigError  # noqa: E402
from test_production_record import complete_build_record  # noqa: E402


CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
PROJECT_PATH = REPO_ROOT / "tests" / "data" / "studio_bus_project_v2.json"


class ProductionBatchTest(unittest.TestCase):
    def setUp(self):
        self.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.left = json.loads(PROJECT_PATH.read_text(encoding="utf-8"))
        self.right = copy.deepcopy(self.left)
        self.right["i2c"]["buses"][0]["maximum_clock_hz"] = 100_000
        build_record = complete_build_record(self.left, self.catalog)
        self.left_record = generate_production_record(
            self.left, self.catalog, build_record=build_record)
        self.right_record = generate_production_record(self.right, self.catalog)
        self.comparison = export_project_comparison(
            self.left, self.right, self.catalog)

    def _export(self, records=None, comparisons=None):
        return export_production_batch(
            batch_id="prototype-001", name="首轮软件归档", note="只整理软件证据",
            production_records=records or [
                {"record": self.left_record.record,
                 "record_sha256": self.left_record.sha256},
                {"record": self.right_record.record,
                 "record_sha256": self.right_record.sha256},
            ],
            comparison_exports=comparisons if comparisons is not None else [
                self.comparison.comparison],
        )

    @staticmethod
    def _files(result):
        with zipfile.ZipFile(io.BytesIO(result.archive)) as archive:
            return {name: archive.read(name) for name in archive.namelist()}

    def test_export_is_deterministic_and_traceable_by_all_required_keys(self):
        first = self._export()
        second = self._export(records=[
            {"record": self.right_record.record,
             "record_sha256": self.right_record.sha256},
            {"record": self.left_record.record,
             "record_sha256": self.left_record.sha256},
        ])
        self.assertEqual(first.archive, second.archive)
        self.assertEqual(first.archive_sha256, second.archive_sha256)
        self.assertTrue(first.validation["valid"])
        trace = first.manifest["trace_index"]
        self.assertEqual(len(trace["by_project_sha256"]), 2)
        self.assertEqual(len(trace["by_board_id"]), 1)
        self.assertEqual(len(trace["by_record_sha256"]), 2)
        self.assertEqual(len(trace["by_build_id"]), 1)
        self.assertEqual(trace["by_build_id"][0]["key"],
                         "mellow-fly-d5-v1-0123456789abcdef")
        self.assertEqual(first.manifest["records"], sorted(
            first.manifest["records"], key=lambda item: item["record_sha256"]))
        files = self._files(first)
        self.assertEqual(len(files), 3)
        manifest_name = next(name for name in files
                             if name.endswith("生产批次清单-v1.json"))
        summary_name = next(name for name in files
                            if name.endswith("生产批次摘要-v1.md"))
        self.assertEqual(json.loads(files[manifest_name]), first.manifest)
        summary = files[summary_name].decode("utf-8")
        self.assertIn(first.manifest["manifest_sha256"], summary)
        self.assertIn("未执行构建、烧录、硬件连接或实测", summary)
        sums = files["SHA256SUMS"].decode("utf-8")
        for name in (manifest_name, summary_name):
            self.assertIn(
                f"{hashlib.sha256(files[name]).hexdigest()}  {name}", sums)

    def test_import_validation_detects_tamper_duplicate_and_missing_reference(self):
        manifest = self._export().manifest
        tampered = copy.deepcopy(manifest)
        tampered["records"][0]["board_id"] = "another-board"
        validation = validate_production_batch_manifest(tampered)
        self.assertFalse(validation["valid"])
        self.assertIn("hash_mismatch",
                      {item["code"] for item in validation["issues"]})

        duplicated = copy.deepcopy(manifest)
        duplicated["records"].append(copy.deepcopy(duplicated["records"][0]))
        validation = validate_production_batch_manifest(duplicated)
        self.assertIn("duplicate",
                      {item["code"] for item in validation["issues"]})

        missing = copy.deepcopy(manifest)
        missing["comparisons"][0]["right_project_sha256"] = "0" * 64
        validation = validate_production_batch_manifest(missing)
        self.assertIn("missing_reference",
                      {item["code"] for item in validation["issues"]})

    def test_expected_evidence_hashes_are_verified(self):
        broken_record = {"record": self.left_record.record,
                         "record_sha256": "0" * 64}
        with self.assertRaisesRegex(ProjectConfigError, "可能已被修改"):
            self._export(records=[broken_record], comparisons=[])

        broken_comparison = {
            "comparison": self.comparison.comparison,
            "archive_sha256": "0" * 64,
        }
        with self.assertRaisesRegex(ProjectConfigError, "可能已被修改"):
            self._export(comparisons=[broken_comparison])

    def test_capacity_and_manifest_size_are_bounded(self):
        evidence = {"record": self.left_record.record,
                    "record_sha256": self.left_record.sha256}
        with self.assertRaisesRegex(ProjectConfigError,
                                    str(MAX_BATCH_RECORDS)):
            self._export(records=[evidence] * (MAX_BATCH_RECORDS + 1),
                         comparisons=[])
        with self.assertRaisesRegex(ProjectConfigError,
                                    str(MAX_BATCH_COMPARISONS)):
            self._export(comparisons=[self.comparison.comparison] *
                         (MAX_BATCH_COMPARISONS + 1))
        oversized = copy.deepcopy(self._export().manifest)
        oversized["extra"] = "大" * MAX_BATCH_MANIFEST_BYTES
        with self.assertRaisesRegex(ProjectConfigError, "128 KiB"):
            validate_production_batch_manifest(oversized)

    def test_missing_build_reference_is_not_accepted(self):
        broken = copy.deepcopy(self.left_record.record)
        broken["software_build_evidence"]["build_id"] = None
        with self.assertRaisesRegex(ProjectConfigError, "缺少构建ID"):
            self._export(records=[broken], comparisons=[])

    def test_markdown_escapes_untrusted_batch_and_record_text(self):
        record = copy.deepcopy(self.left_record.record)
        record["design_evidence"]["board_id"] = "board\n## 伪造板卡"
        record["software_build_evidence"]["build_id"] = \
            "build\n- 已烧录 | `true`"
        result = export_production_batch(
            batch_id="escape-test", name="名称\n## 伪造标题",
            note="说明\n- 已实测 | `true` \\ end",
            production_records=[record], comparison_exports=[])
        files = self._files(result)
        summary_name = next(name for name in files
                            if name.endswith("生产批次摘要-v1.md"))
        markdown = files[summary_name].decode("utf-8")
        self.assertNotIn("\n## 伪造", markdown)
        self.assertNotIn("\n- 已烧录", markdown)
        self.assertNotIn("\n- 已实测", markdown)
        self.assertIn(r"\#\# 伪造标题", markdown)
        self.assertIn(r"\- 已实测 \| \`true\`", markdown)
        self.assertIn(r"\- 已烧录 \| \`true\`", markdown)
        self.assertIn("SHA-256用于内容一致性核对，不是签名", markdown)


if __name__ == "__main__":
    unittest.main()

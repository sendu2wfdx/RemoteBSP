import copy
import io
import json
import sys
import unittest
import zipfile
from pathlib import Path


GUI_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = GUI_ROOT.parent
sys.path.insert(0, str(GUI_ROOT))

from comparison_export import export_project_comparison  # noqa: E402
from project_compare import MAX_PROJECT_BYTES, compare_projects  # noqa: E402
from project_config import ProjectConfigError  # noqa: E402


CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
PROJECT_PATH = REPO_ROOT / "tests" / "data" / "studio_bus_project_v2.json"


class ComparisonExportTest(unittest.TestCase):
    def setUp(self):
        self.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.left = json.loads(PROJECT_PATH.read_text(encoding="utf-8"))
        self.right = copy.deepcopy(self.left)
        self.right["i2c"]["buses"][0]["maximum_clock_hz"] = 100_000

    @staticmethod
    def _files(result) -> dict[str, bytes]:
        with zipfile.ZipFile(io.BytesIO(result.archive)) as archive:
            return {name: archive.read(name) for name in archive.namelist()}

    def test_json_and_markdown_reuse_the_exact_comparison_result(self):
        expected = compare_projects(self.left, self.right, self.catalog)
        result = export_project_comparison(
            self.left, self.right, self.catalog)
        self.assertEqual(result.comparison, expected)
        files = self._files(result)
        self.assertEqual(len(files), 3)
        json_name = next(name for name in files
                         if name.endswith("工程差异-v1.json"))
        markdown_name = next(name for name in files
                             if name.endswith("工程差异报告-v1.md"))
        self.assertEqual(json.loads(files[json_name]), expected)
        markdown = files[markdown_name].decode("utf-8")
        self.assertIn(expected["summary"]["headline"], markdown)
        self.assertIn(expected["comparison_sha256"], markdown)
        for change in expected["changes"]:
            self.assertIn(change["summary"], markdown)
        self.assertIn("差异详情未截断", markdown)
        self.assertIn("未执行构建、烧录或板卡访问", markdown)
        self.assertIn("SHA256SUMS", files)

    def test_export_is_deterministic_and_records_input_migration(self):
        legacy = copy.deepcopy(self.left)
        legacy.pop("schema_version")
        first = export_project_comparison(legacy, self.right, self.catalog)
        second = export_project_comparison(
            json.loads(json.dumps(legacy, sort_keys=True)),
            json.loads(json.dumps(self.right, sort_keys=True)), self.catalog)
        self.assertEqual(first.archive, second.archive)
        self.assertEqual(first.archive_sha256, second.archive_sha256)
        markdown_name = next(name for name in self._files(first)
                             if name.endswith("工程差异报告-v1.md"))
        markdown = self._files(first)[markdown_name].decode("utf-8")
        self.assertIn("原始 v0 → 规范 v2", markdown)
        self.assertIn("v0->v1", markdown)

    def test_truncation_is_explicit_in_both_exports(self):
        left = copy.deepcopy(self.left)
        left["spi"] = {"buses": [], "devices": []}
        template = left["i2c"]["devices"][0]
        left["i2c"]["devices"] = []
        for address in range(1, 128):
            device = copy.deepcopy(template)
            device.update({"name": f"sensor_{address:03d}",
                           "address": address, "initial_data": []})
            left["i2c"]["devices"].append(device)
        right = copy.deepcopy(left)
        for device in right["i2c"]["devices"]:
            device.update({
                "maximum_clock_hz": 100_000,
                "maximum_transfer_bytes": 15,
                "queue_capacity": 1,
                "minimum_timeout_us": 200,
                "maximum_timeout_us": 9_000,
                "maximum_operations_per_second": 500,
                "flags": ["recovery"],
            })
        result = export_project_comparison(left, right, self.catalog)
        self.assertTrue(result.comparison["summary"]["truncated"])
        files = self._files(result)
        markdown_name = next(name for name in files
                             if name.endswith("工程差异报告-v1.md"))
        markdown = files[markdown_name].decode("utf-8")
        omitted = result.comparison["summary"]["omitted_field_changes"]
        self.assertIn(f"{omitted} 条因有界输出限制未展开", markdown)
        json_name = next(name for name in files
                         if name.endswith("工程差异-v1.json"))
        self.assertTrue(json.loads(files[json_name])["summary"]["truncated"])

    def test_validation_and_input_bounds_are_inherited(self):
        broken = copy.deepcopy(self.right)
        broken["i2c"]["devices"][0]["address"] = 0
        with self.assertRaisesRegex(ProjectConfigError, "address"):
            export_project_comparison(self.left, broken, self.catalog)

        oversized = copy.deepcopy(self.left)
        oversized["说明"] = "大" * MAX_PROJECT_BYTES
        with self.assertRaisesRegex(ProjectConfigError, "128 KiB"):
            export_project_comparison(oversized, self.right, self.catalog)


if __name__ == "__main__":
    unittest.main()

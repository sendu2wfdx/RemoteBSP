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

from project_artifacts import generate_project_reports  # noqa: E402
from project_config import ProjectConfigError  # noqa: E402


CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
PROJECT_PATH = REPO_ROOT / "tests" / "data" / "studio_bus_project_v2.json"


class ProjectArtifactsTest(unittest.TestCase):
    def setUp(self):
        self.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.project = json.loads(PROJECT_PATH.read_text(encoding="utf-8"))

    @staticmethod
    def _files(bundle) -> dict[str, bytes]:
        with zipfile.ZipFile(io.BytesIO(bundle.archive)) as archive:
            return {name: archive.read(name) for name in archive.namelist()}

    def test_reports_include_chinese_wiring_summary_manifest_and_checksums(self):
        bundle = generate_project_reports(self.project, self.catalog)
        self.assertEqual(bundle.resource_count, 4)
        self.assertRegex(bundle.project_sha256, r"^[0-9a-f]{64}$")
        self.assertRegex(bundle.resource_set_sha256, r"^[0-9a-f]{64}$")
        self.assertRegex(bundle.archive_sha256, r"^[0-9a-f]{64}$")

        files = self._files(bundle)
        self.assertEqual(len(files), 4)
        wiring_name = next(name for name in files if name.endswith("接线表.md"))
        summary_name = next(
            name for name in files if name.endswith("资源占用摘要.json"))
        manifest_name = next(
            name for name in files if name.endswith("稳定资源清单.json"))
        wiring = files[wiring_name].decode("utf-8")
        self.assertIn("# STM32F103CBT6 / WeAct BluePill Plus 接线表", wiring)
        self.assertIn("仅 Mock 数字孪生", wiring)
        self.assertIn("I2C设备", wiring)
        self.assertIn("0x48", wiring)
        self.assertIn("板卡固定占用", wiring)

        summary = json.loads(files[summary_name])
        self.assertEqual(summary["resource_count"], 4)
        self.assertEqual(summary["resource_counts"]["i2c_bus"], 1)
        self.assertEqual(len(summary["mock_only_resource_keys"]), 4)
        self.assertIn("PB12", {item["pin"]
                               for item in summary["pin_usage"]})
        self.assertIn("PB8", {item["pin"]
                              for item in summary["reserved_pin_usage"]})

        manifest = json.loads(files[manifest_name])
        self.assertEqual(manifest["resource_set_sha256"],
                         bundle.resource_set_sha256)
        self.assertEqual([item["key"] for item in manifest["resources"]],
                         sorted(item["key"]
                                for item in manifest["resources"]))
        checksums = files["SHA256SUMS"].decode("utf-8")
        for artifact in bundle.artifacts[:-1]:
            self.assertIn(f"{artifact.sha256}  {artifact.filename}\n",
                          checksums)

    def test_reports_are_deterministic_for_equivalent_json_key_order(self):
        first = generate_project_reports(self.project, self.catalog)
        reordered = json.loads(json.dumps(self.project, sort_keys=True))
        second = generate_project_reports(reordered, self.catalog)
        self.assertEqual(first.project_sha256, second.project_sha256)
        self.assertEqual(first.resource_set_sha256,
                         second.resource_set_sha256)
        self.assertEqual(first.archive, second.archive)
        self.assertEqual(first.archive_sha256, second.archive_sha256)

        changed = copy.deepcopy(self.project)
        changed["i2c"]["devices"][0]["address"] = 0x49
        third = generate_project_reports(changed, self.catalog)
        self.assertNotEqual(first.project_sha256, third.project_sha256)
        self.assertNotEqual(first.resource_set_sha256,
                            third.resource_set_sha256)

    def test_reports_reuse_validation_and_reject_invalid_project(self):
        broken = copy.deepcopy(self.project)
        broken["i2c"]["devices"][0]["address"] = 0
        with self.assertRaisesRegex(ProjectConfigError, "address"):
            generate_project_reports(broken, self.catalog)

    def test_summary_combines_physical_and_mock_only_resources(self):
        mixed = copy.deepcopy(self.project)
        mixed["gpio"]["resources"].append({
            "name": "button", "pin": "PA0", "direction": "input",
            "pull": "down", "active_low": False, "safe_level": None,
            "debounce_ms": 10,
        })
        bundle = generate_project_reports(mixed, self.catalog)
        summary_name = next(
            name for name in self._files(bundle)
            if name.endswith("资源占用摘要.json"))
        summary = json.loads(self._files(bundle)[summary_name])
        self.assertEqual(summary["resource_count"], 5)
        self.assertEqual(summary["resource_counts"]["gpio"], 1)
        self.assertEqual(len(summary["mock_only_resource_keys"]), 4)
        self.assertIn("PA0", {item["pin"]
                              for item in summary["pin_usage"]})


if __name__ == "__main__":
    unittest.main()

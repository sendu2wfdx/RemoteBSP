import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


GUI_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = GUI_ROOT.parent
sys.path.insert(0, str(GUI_ROOT))
sys.path.insert(0, str(GUI_ROOT / "tests"))

from comparison_export import export_project_comparison  # noqa: E402
from production_batch import export_production_batch  # noqa: E402
from production_history import ProductionHistoryStore  # noqa: E402
from production_record import generate_production_record  # noqa: E402
from project_config import ProjectConfigError  # noqa: E402
from test_production_record import complete_build_record  # noqa: E402


CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
PROJECT_PATH = REPO_ROOT / "tests" / "data" / "studio_bus_project_v2.json"


class ProductionHistoryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "history"
        self.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        left = json.loads(PROJECT_PATH.read_text(encoding="utf-8"))
        right = copy.deepcopy(left)
        right["i2c"]["buses"][0]["maximum_clock_hz"] = 100_000
        built = generate_production_record(
            left, self.catalog,
            build_record=complete_build_record(left, self.catalog))
        design = generate_production_record(right, self.catalog)
        comparison = export_project_comparison(left, right, self.catalog)
        self.batch = export_production_batch(
            batch_id="history-001", name="本地历史测试", note="纯软件",
            production_records=[built.record, design.record],
            comparison_exports=[comparison.comparison])

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def _rehash(manifest):
        value = copy.deepcopy(manifest)
        value.pop("manifest_sha256", None)
        encoded = (json.dumps(value, ensure_ascii=False, allow_nan=False,
                              sort_keys=True, separators=(",", ":")) +
                   "\n").encode("utf-8")
        import hashlib
        manifest["manifest_sha256"] = hashlib.sha256(encoded).hexdigest()

    def test_atomic_save_is_immutable_idempotent_and_path_safe(self):
        store = ProductionHistoryStore(self.root)
        first = store.save(self.batch.manifest)
        self.assertTrue(first["stored"])
        key = self.batch.manifest["manifest_sha256"]
        expected = self.root / f"{key}.json"
        self.assertTrue(expected.is_file())
        self.assertEqual(json.loads(expected.read_text(encoding="utf-8")),
                         self.batch.manifest)
        repeated = store.save(self.batch.manifest)
        self.assertFalse(repeated["stored"])
        self.assertTrue(repeated["already_exists"])
        with self.assertRaisesRegex(ProjectConfigError, "64位"):
            store.get("../../outside")

        malicious = copy.deepcopy(self.batch.manifest)
        malicious["batch"]["batch_id"] = "../../outside"
        self._rehash(malicious)
        with self.assertRaisesRegex(ProjectConfigError, "校验失败"):
            store.save(malicious)
        self.assertFalse((self.root.parent / "outside.json").exists())
        self.assertEqual([expected], list(self.root.glob("*.json")))

    def test_unmarked_nonempty_directory_is_refused_without_moving_files(self):
        unsafe = self.root.parent / "ordinary-directory"
        unsafe.mkdir()
        original = unsafe / "important.json"
        original.write_text('{"keep": true}\n', encoding="utf-8")
        with self.assertRaisesRegex(ProjectConfigError, "保护原文件"):
            ProductionHistoryStore(unsafe)
        self.assertTrue(original.is_file())
        self.assertEqual(original.read_text(encoding="utf-8"),
                         '{"keep": true}\n')
        self.assertFalse((unsafe / "quarantine").exists())

    def test_marker_read_is_bounded_and_rejects_oversized_content(self):
        unsafe = self.root.parent / "oversized-marker"
        unsafe.mkdir()
        marker = unsafe / ".remotebsp-production-history-v1"
        marker.write_bytes(b"REMOTEBSP_PRODUCTION_HISTORY_V1\n" + b"x" * 4096)
        with self.assertRaisesRegex(ProjectConfigError, "标记损坏"):
            ProductionHistoryStore(unsafe)
        self.assertEqual(marker.stat().st_size, 4128)
        self.assertFalse((unsafe / "quarantine").exists())

    def test_capacity_single_record_and_failed_replace_are_bounded(self):
        small = ProductionHistoryStore(self.root, max_records=1)
        small.save(self.batch.manifest)
        another = copy.deepcopy(self.batch.manifest)
        another["batch"]["batch_id"] = "history-002"
        another["batch"]["name"] = "第二批"
        self._rehash(another)
        with self.assertRaisesRegex(ProjectConfigError, "1条容量上限"):
            small.save(another)

        tiny_root = self.root.parent / "tiny"
        tiny = ProductionHistoryStore(tiny_root, max_record_bytes=16)
        with self.assertRaisesRegex(ProjectConfigError, "单条历史记录"):
            tiny.save(self.batch.manifest)

        failed_root = self.root.parent / "failed"
        failed = ProductionHistoryStore(failed_root)
        with patch("production_history.os.replace",
                   side_effect=OSError("模拟原子替换失败")):
            with self.assertRaisesRegex(OSError, "模拟原子替换失败"):
                failed.save(self.batch.manifest)
        self.assertEqual([], list(failed_root.glob("*.json")))
        self.assertEqual([], list(failed_root.glob(".pending-*")))

    def test_startup_audit_quarantines_corrupt_wrong_name_and_symlink(self):
        store = ProductionHistoryStore(self.root)
        store.save(self.batch.manifest)
        (self.root / "broken.json").write_text("{not-json", encoding="utf-8")
        wrong_name = self.root / ("0" * 64 + ".json")
        wrong_name.write_text(json.dumps(
            self.batch.manifest, ensure_ascii=False, sort_keys=True, indent=2) +
            "\n", encoding="utf-8")
        outside = self.root.parent / "outside.json"
        outside.write_text("不得读取或移动", encoding="utf-8")
        symlink = self.root / "linked.json"
        try:
            symlink.symlink_to(outside)
        except OSError:
            symlink = None

        restarted = ProductionHistoryStore(self.root)
        status = restarted.status()
        expected_issues = 3 if symlink is not None else 2
        self.assertEqual(status["valid_record_count"], 1)
        self.assertGreaterEqual(status["startup_issue_count"], expected_issues)
        self.assertGreaterEqual(status["quarantined_count"], expected_issues)
        self.assertEqual(status["status"], "attention")
        self.assertTrue(outside.is_file())
        self.assertEqual(outside.read_text(encoding="utf-8"), "不得读取或移动")
        self.assertFalse((self.root / "broken.json").exists())
        self.assertFalse(wrong_name.exists())

    def test_search_and_get_cover_all_required_trace_keys(self):
        store = ProductionHistoryStore(self.root)
        store.save(self.batch.manifest)
        manifest = self.batch.manifest
        record = manifest["records"][0]
        queries = {
            "project_sha256": record["project_sha256"][:16],
            "build_id": next(item["build_id"] for item in manifest["records"]
                             if item["build_id"]),
            "board_id": record["board_id"],
            "record_sha256": record["record_sha256"][:16],
        }
        for field, query in queries.items():
            with self.subTest(field=field):
                result = store.search(query, field, 10)
                self.assertEqual(result["total_matches"], 1)
                self.assertEqual(result["records"][0]["manifest_sha256"],
                                 manifest["manifest_sha256"])
                self.assertEqual(result["records"][0]["matched_fields"],
                                 [field])
        fetched = store.get(manifest["manifest_sha256"])
        self.assertEqual(fetched.manifest, manifest)
        with self.assertRaisesRegex(ProjectConfigError, "128"):
            store.search("x" * 129)

    def test_post_start_damage_is_removed_from_index_and_isolated(self):
        store = ProductionHistoryStore(self.root)
        store.save(self.batch.manifest)
        key = self.batch.manifest["manifest_sha256"]
        (self.root / f"{key}.json").write_text("{}\n", encoding="utf-8")
        self.assertEqual(store.search("", "all", 10)["total_matches"], 0)
        with self.assertRaisesRegex(ProjectConfigError, "未找到"):
            store.get(key)
        status = store.status()
        self.assertEqual(status["valid_record_count"], 0)
        self.assertEqual(status["status"], "attention")
        self.assertGreaterEqual(status["quarantined_count"], 1)


if __name__ == "__main__":
    unittest.main()

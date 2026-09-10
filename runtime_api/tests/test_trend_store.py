import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from runtime_api.dashboard import RuntimeDashboard
from runtime_api.provider import mock_snapshot
from runtime_api.trend_store import (
    TREND_STORE_FILENAME,
    RuntimeTrendStore,
    TrendStoreError,
)


class RuntimeTrendStoreTests(unittest.TestCase):
    def _store(self, root: Path, capacity: int = 3) -> RuntimeTrendStore:
        directory = root / "trends"
        return RuntimeTrendStore(directory, capacity=capacity)

    def test_restart_restores_bounded_samples_and_deduplicates_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dashboard = RuntimeDashboard(2, self._store(root, 2))
            snapshot = mock_snapshot()
            for sequence, depth in ((1, 4), (2, 9), (3, 2)):
                snapshot["snapshot_id"] = f"mock-{sequence}"
                snapshot["captured_at_ms"] = sequence
                snapshot["nodes"][0]["runtime"]["queue_depth"] = depth
                dashboard.observe(snapshot)

            restarted = RuntimeDashboard(2, self._store(root, 2))
            view = restarted.observe(snapshot)
            samples = view["nodes"][0]["trend"]["samples"]
            self.assertEqual([item["captured_at_ms"] for item in samples],
                             [2, 3])
            self.assertEqual(len(samples), 2)
            self.assertEqual(view["nodes"][0]["trend"]["peaks"][
                "queue_depth"], 9)

    def test_two_real_processes_restore_previous_trend(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "trends"
            first = subprocess.run(
                [sys.executable, "-m",
                 "runtime_api.tests.trend_process_helper", str(directory), "1"],
                check=True, capture_output=True, text=True, timeout=10)
            second = subprocess.run(
                [sys.executable, "-m",
                 "runtime_api.tests.trend_process_helper", str(directory), "2"],
                check=True, capture_output=True, text=True, timeout=10)
            self.assertEqual(len(json.loads(first.stdout)["samples"]), 1)
            restored = json.loads(second.stdout)
            self.assertEqual([item["captured_at_ms"]
                              for item in restored["samples"]], [1, 2])
            self.assertEqual(restored["peaks"]["queue_depth"], 20)

    def test_health_generation_and_sequence_are_deduplicated_after_restart(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            health = {"available": True, "snapshot": {
                "overall": "healthy", "producer_generation": 8,
                "sample_sequence": 11, "sample_time_ms": 123,
                "metrics": [{"name": "cpu_load_permille",
                             "availability": "available", "unit": "permille",
                             "value": 44}]}}
            RuntimeDashboard(3, self._store(root)).observe(
                mock_snapshot(), health)
            restarted = RuntimeDashboard(3, self._store(root))
            view = restarted.observe(mock_snapshot(), health)
            self.assertEqual(len(view["toolbusd_health"]["trend"]["samples"]), 1)

    def test_file_contains_only_whitelisted_trend_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            snapshot = mock_snapshot()
            snapshot["nodes"][0]["runtime"]["api_key"] = "绝不能落盘"
            snapshot["nodes"][0]["runtime"]["control"] = {"value": True}
            RuntimeDashboard(3, self._store(root)).observe(snapshot)
            encoded = (root / "trends" / TREND_STORE_FILENAME).read_text(
                encoding="utf-8")
            self.assertNotIn("绝不能落盘", encoded)
            self.assertNotIn("api_key", encoded)
            self.assertNotIn("control", encoded)

    def test_corrupt_file_is_isolated_and_does_not_poison_new_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = self._store(root)
            store.path.write_text("{broken", encoding="utf-8")
            if os.name == "posix":
                os.chmod(store.path, 0o600)
            self.assertEqual(store.load(), {"nodes": {}, "toolbusd_health": []})
            self.assertFalse(store.path.exists())
            self.assertEqual(len(list(store.directory.glob(
                "trend-v1.corrupt-*.json"))), 1)
            RuntimeDashboard(3, store).observe(mock_snapshot())
            self.assertTrue(store.path.exists())

    def test_oversize_and_unknown_fields_are_isolated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = RuntimeTrendStore(root / "trends", capacity=3,
                                      maximum_bytes=4096)
            store.path.write_bytes(b"x" * 4097)
            if os.name == "posix":
                os.chmod(store.path, 0o600)
            self.assertEqual(store.load()["nodes"], {})
            store.path.write_text(json.dumps({
                "schema_version": 1, "kind": "remotebsp-runtime-trends",
                "nodes": {}, "toolbusd_health": [], "api_key": "secret",
            }), encoding="utf-8")
            if os.name == "posix":
                os.chmod(store.path, 0o600)
            self.assertEqual(store.load()["nodes"], {})

    @unittest.skipUnless(os.name == "posix", "POSIX权限约束")
    def test_rejects_group_readable_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "trends"
            directory.mkdir(mode=0o750)
            os.chmod(directory, 0o750)
            with self.assertRaises(TrendStoreError):
                RuntimeTrendStore(directory, capacity=3)

    @unittest.skipUnless(os.name == "posix", "POSIX权限约束")
    def test_rejects_group_readable_or_hardlinked_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = self._store(root)
            RuntimeDashboard(3, store).observe(mock_snapshot())
            os.chmod(store.path, 0o640)
            with self.assertRaises(TrendStoreError):
                store.load()
            os.chmod(store.path, 0o600)
            os.link(store.path, root / "second-link.json")
            with self.assertRaises(TrendStoreError):
                store.load()

    @unittest.skipUnless(hasattr(os, "symlink"), "需要符号链接")
    def test_rejects_symlink_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = self._store(root)
            target = root / "target.json"
            target.write_text("{}", encoding="utf-8")
            try:
                store.path.symlink_to(target)
            except OSError:
                self.skipTest("当前环境不允许创建符号链接")
            with self.assertRaises(TrendStoreError):
                store.load()


if __name__ == "__main__":
    unittest.main()

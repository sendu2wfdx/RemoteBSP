#!/usr/bin/env python3
"""对照基准运行记录与原始证据完整性测试。"""

from __future__ import annotations

import copy
import hashlib
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "benchmarks"))

from validate_run_manifest import (  # noqa: E402
    MAXIMUM_MANIFEST_BYTES,
    RunManifestError,
    load_run_manifest,
    validate_run_manifest,
)


class BenchmarkRunManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(dir=REPO_ROOT)
        self.root = Path(self.temporary.name)
        self.raw_path = self.root / "capture.csv"
        self.raw_path.write_bytes(b"time_ns,step\n0,0\n1000,1\n")
        relative = self.raw_path.relative_to(REPO_ROOT).as_posix()
        digest = hashlib.sha256(self.raw_path.read_bytes()).hexdigest()
        system = {
            "revision": "a" * 40,
            "host_config_sha256": "b" * 64,
            "firmware_config_sha256s": ["c" * 64],
            "firmware_image_sha256s": ["f" * 64],
        }
        self.manifest = {
            "schema_version": 1,
            "format": "REMOTEBSP_KLIPPER_BENCHMARK_RUN_V1",
            "run_id": "paired_run_001",
            "plan_sha256": "d" * 64,
            "started_at": "2026-09-10T00:00:00Z",
            "completed_at": "2026-09-10T00:30:00Z",
            "operator_id": "operator_a",
            "environment_digest": "e" * 64,
            "systems": {"remotebsp": copy.deepcopy(system),
                        "klipper": copy.deepcopy(system)},
            "scenarios": [{
                "id": "single_node_step_timing",
                "status": "passed",
                "safety_failure": False,
                "metrics": [{
                    "id": "step_interval_error_p99_ns",
                    "unit": "ns",
                    "remotebsp": {"value": 12.5, "sample_count": 30},
                    "klipper": {"value": 13.5, "sample_count": 30},
                }],
                "raw_files": [{
                    "path": relative,
                    "sha256": digest,
                    "size_bytes": self.raw_path.stat().st_size,
                }],
            }],
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_valid_manifest_recomputes_raw_file_hash(self) -> None:
        paths = validate_run_manifest(
            self.manifest, REPO_ROOT, expected_plan_sha256="d" * 64)
        self.assertEqual(paths, {self.raw_path.relative_to(REPO_ROOT).as_posix()})

    def test_changed_raw_file_is_rejected(self) -> None:
        self.raw_path.write_bytes(b"tampered")
        with self.assertRaisesRegex(RunManifestError, "不匹配"):
            validate_run_manifest(self.manifest, REPO_ROOT)

    def test_plan_digest_mismatch_is_rejected(self) -> None:
        with self.assertRaisesRegex(RunManifestError, "当前计划"):
            validate_run_manifest(
                self.manifest, REPO_ROOT, expected_plan_sha256="f" * 64)

    def test_non_finite_metric_is_rejected(self) -> None:
        invalid = copy.deepcopy(self.manifest)
        invalid["scenarios"][0]["metrics"][0]["remotebsp"]["value"] = math.inf
        with self.assertRaisesRegex(RunManifestError, "有限数值"):
            validate_run_manifest(invalid, REPO_ROOT)

    def test_safety_failure_cannot_be_reported_passed(self) -> None:
        invalid = copy.deepcopy(self.manifest)
        invalid["scenarios"][0]["safety_failure"] = True
        with self.assertRaisesRegex(RunManifestError, "status必须为failed"):
            validate_run_manifest(invalid, REPO_ROOT)

    def test_symlink_raw_file_is_rejected(self) -> None:
        link = self.root / "capture-link.csv"
        try:
            link.symlink_to(self.raw_path.name)
        except OSError:
            self.skipTest("当前平台不允许创建符号链接")
        invalid = copy.deepcopy(self.manifest)
        invalid["scenarios"][0]["raw_files"][0]["path"] = \
            link.relative_to(REPO_ROOT).as_posix()
        with self.assertRaisesRegex(RunManifestError, "符号链接"):
            validate_run_manifest(invalid, REPO_ROOT)

    def test_duplicate_json_field_is_rejected(self) -> None:
        path = self.root / "duplicate.json"
        path.write_text('{"format":"x","format":"y"}', encoding="utf-8")
        with self.assertRaisesRegex(RunManifestError, "重复字段"):
            load_run_manifest(path)

    def test_oversized_and_too_deep_manifest_are_bounded(self) -> None:
        oversized = self.root / "oversized.json"
        oversized.write_bytes(b" " * (MAXIMUM_MANIFEST_BYTES + 1))
        with self.assertRaisesRegex(RunManifestError, "512 KiB"):
            load_run_manifest(oversized)
        deep = self.root / "deep.json"
        deep.write_text("[" * 2000 + "]" * 2000, encoding="utf-8")
        with self.assertRaisesRegex(RunManifestError, "嵌套超过64层"):
            load_run_manifest(deep)
        huge_integer = self.root / "huge-integer.json"
        huge_integer.write_text("9" * 5000, encoding="utf-8")
        with self.assertRaises(RunManifestError):
            load_run_manifest(huge_integer)


if __name__ == "__main__":
    unittest.main()

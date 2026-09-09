#!/usr/bin/env python3
"""RemoteBSP 成熟度基线的封闭契约与反夸大规则测试。"""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from maturity.validate_maturity import (
    MaturityValidationError,
    load_manifest,
    validate_manifest,
)


MANIFEST_PATH = REPO_ROOT / "maturity" / "remotebsp-maturity-v1.json"
SCHEMA_PATH = REPO_ROOT / "maturity" / "remotebsp-maturity-v1.schema.json"


class MaturityManifestTest(unittest.TestCase):
    def setUp(self):
        self.manifest = load_manifest(MANIFEST_PATH)

    def test_repository_baseline_is_valid_and_comparison_is_blocked(self):
        validate_manifest(self.manifest, REPO_ROOT)
        comparison = self.manifest["overall_comparison"]
        self.assertFalse(comparison["allowed"])
        self.assertEqual(comparison["status"], "blocked")
        self.assertEqual(comparison["benchmark_evidence"], [])
        self.assertTrue({
            "hardware_evidence_incomplete", "no_comparative_benchmark",
        }.issubset(comparison["blocker_ids"]))

    def test_schema_allows_future_ready_state_and_extra_dimensions(self):
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
        dimensions = schema["properties"]["dimensions"]
        self.assertNotIn("maxItems", dimensions)
        self.assertNotIn("minItems", schema["properties"]["blockers"])
        comparison = schema["properties"]["overall_comparison"]["properties"]
        self.assertEqual(comparison["allowed"]["type"], "boolean")
        self.assertIn("ready", comparison["status"]["enum"])

        extended = copy.deepcopy(self.manifest)
        extended["blockers"].append({
            "id": "future_gap",
            "dimension_id": "future_dimension",
            "summary": "未来维度尚无证据。",
            "requires_hardware": False,
            "closure_evidence": "补齐未来维度的实现和验证证据。",
        })
        absent = {
            "status": "absent", "evidence": [],
            "blocker_ids": ["future_gap"],
        }
        extended["dimensions"].append({
            "id": "future_dimension",
            "title": "未来扩展维度",
            "summary": "用于证明 v1 验证器不会把十项基线变成永久上限。",
            "implemented": copy.deepcopy(absent),
            "automated_test": copy.deepcopy(absent),
            "cross_compiled": {
                "status": "not_applicable", "evidence": [],
                "blocker_ids": [],
            },
            "hardware_verified": {
                "status": "not_applicable", "evidence": [],
                "blocker_ids": [],
            },
        })
        validate_manifest(extended, REPO_ROOT)

    def test_mock_evidence_cannot_be_used_as_hardware_verification(self):
        invalid = copy.deepcopy(self.manifest)
        hardware = next(
            item for item in invalid["dimensions"]
            if item["id"] == "hardware_evidence")
        evidence = hardware["hardware_verified"]["evidence"][0]
        evidence["kind"] = "mock_test"
        evidence["path"] = "tests/mock_node_tests.cpp"
        evidence["note"] = "Mock 测试不是实体实测。"
        with self.assertRaisesRegex(
                MaturityValidationError, "不能证明hardware_verified"):
            validate_manifest(invalid, REPO_ROOT)

    def test_overall_claim_needs_ready_levels_and_comparative_evidence(self):
        invalid = copy.deepcopy(self.manifest)
        comparison = invalid["overall_comparison"]
        comparison["allowed"] = True
        comparison["status"] = "ready"
        comparison["blocker_ids"] = []
        with self.assertRaisesRegex(
                MaturityValidationError, "任何blocker"):
            validate_manifest(invalid, REPO_ROOT)

        no_benchmark = copy.deepcopy(invalid)
        no_benchmark["blockers"] = []
        for dimension in no_benchmark["dimensions"]:
            for level_name in (
                    "implemented", "automated_test", "cross_compiled",
                    "hardware_verified"):
                level = dimension[level_name]
                level["blocker_ids"] = []
                if level["evidence"]:
                    level["status"] = "verified"
                else:
                    level["status"] = "not_applicable"
        with self.assertRaisesRegex(
                MaturityValidationError, "comparative_benchmark"):
            validate_manifest(no_benchmark, REPO_ROOT)

        partial_progress = copy.deepcopy(self.manifest)
        partial_progress["overall_comparison"]["blocker_ids"] = [
            "no_comparative_benchmark"]
        validate_manifest(partial_progress, REPO_ROOT)

    def test_missing_evidence_and_unknown_fields_fail_closed(self):
        missing = copy.deepcopy(self.manifest)
        missing["dimensions"][0]["implemented"]["evidence"][0][
            "path"] = "missing/not-a-real-evidence.txt"
        with self.assertRaisesRegex(MaturityValidationError, "不存在"):
            validate_manifest(missing, REPO_ROOT)

        extra = copy.deepcopy(self.manifest)
        extra["overall_comparison"]["score"] = 100
        with self.assertRaisesRegex(MaturityValidationError, "未知score"):
            validate_manifest(extra, REPO_ROOT)


if __name__ == "__main__":
    unittest.main()

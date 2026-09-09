#!/usr/bin/env python3
"""RemoteBSP / Klipper 对照基准计划契约测试。"""

from __future__ import annotations

import copy
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "benchmarks"))

from validate_comparison_plan import (  # noqa: E402
    MAXIMUM_BYTES,
    ComparisonPlanError,
    load_plan,
    validate_plan,
)


class ComparisonPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.path = REPO_ROOT / "benchmarks" / "comparison-plan-v1.json"
        cls.plan = load_plan(cls.path)

    def test_repository_draft_is_valid_and_not_executed(self) -> None:
        validate_plan(self.plan, REPO_ROOT)
        self.assertEqual(self.plan["status"], "draft")
        self.assertEqual(self.plan["artifacts"]["raw_data_paths"], [])

    def test_duplicate_json_field_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.json"
            path.write_text('{"schema_version":1,"schema_version":1}',
                            encoding="utf-8")
            with self.assertRaisesRegex(ComparisonPlanError, "重复字段"):
                load_plan(path)

    def test_oversized_and_too_deep_json_are_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            oversized = Path(directory) / "oversized.json"
            oversized.write_bytes(b" " * (MAXIMUM_BYTES + 1))
            with self.assertRaisesRegex(ComparisonPlanError, "256 KiB"):
                load_plan(oversized)
            deep = Path(directory) / "deep.json"
            deep.write_text("[" * 2000 + "]" * 2000, encoding="utf-8")
            with self.assertRaisesRegex(ComparisonPlanError, "嵌套超过64层"):
                load_plan(deep)
            huge_integer = Path(directory) / "huge-integer.json"
            huge_integer.write_text("9" * 5000, encoding="utf-8")
            with self.assertRaises(ComparisonPlanError):
                load_plan(huge_integer)

    def test_preregistered_requires_pins_environment_and_thresholds(self) -> None:
        invalid = copy.deepcopy(self.plan)
        invalid["status"] = "preregistered"
        with self.assertRaises(ComparisonPlanError):
            validate_plan(invalid, REPO_ROOT)

    def test_v1_refuses_executed_even_with_self_reported_evidence(self) -> None:
        invalid = copy.deepcopy(self.plan)
        invalid["status"] = "executed"
        with self.assertRaisesRegex(ComparisonPlanError, "status不受支持"):
            validate_plan(invalid, REPO_ROOT)

    def test_metric_ids_are_globally_unique(self) -> None:
        invalid = copy.deepcopy(self.plan)
        invalid["scenarios"][1]["metrics"][0]["id"] = \
            invalid["scenarios"][0]["metrics"][0]["id"]
        with self.assertRaisesRegex(ComparisonPlanError, "重复"):
            validate_plan(invalid, REPO_ROOT)

    def test_path_escape_is_rejected(self) -> None:
        invalid = copy.deepcopy(self.plan)
        invalid["artifacts"]["configuration_paths"] = ["../secret"]
        with self.assertRaisesRegex(ComparisonPlanError, "规范仓库相对路径"):
            validate_plan(invalid, REPO_ROOT)

    def test_minimum_measured_run_count_is_enforced(self) -> None:
        invalid = copy.deepcopy(self.plan)
        invalid["scenarios"][0]["measured_runs"] = 29
        with self.assertRaisesRegex(ComparisonPlanError, "30"):
            validate_plan(invalid, REPO_ROOT)

    def test_non_string_list_item_is_rejected_without_crash(self) -> None:
        invalid = copy.deepcopy(self.plan)
        invalid["environment"]["mcu_boards"] = [{"bad": "value"}]
        with self.assertRaises(ComparisonPlanError):
            validate_plan(invalid, REPO_ROOT)

if __name__ == "__main__":
    unittest.main()

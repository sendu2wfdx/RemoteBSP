import json
import os
import tempfile
import unittest
from pathlib import Path

from runtime_api.alert_rules import (
    ALERT_RULE_CONFIRMATION, AlertRuleError, AlertRuleEvaluator,
    AlertRuleManager, AlertRuleStore, DEFAULT_ALERT_RULES)


class AlertRuleTests(unittest.TestCase):
    def test_default_rules_and_debounce_hysteresis(self):
        manager = AlertRuleManager()
        rules = [dict(DEFAULT_ALERT_RULES[0])]
        rules[0]["debounce_samples"] = 2
        pending = manager.preflight(0, rules)
        manager.apply(pending["confirmation_token"], ALERT_RULE_CONFIRMATION)
        evaluator = AlertRuleEvaluator(manager)
        metric = [{"name": "cpu_load_permille", "unit": "permille",
                   "availability": "available", "value": 810}]
        self.assertEqual(evaluator.evaluate(metric), [])
        self.assertEqual(len(evaluator.evaluate(metric)), 1)
        metric[0]["value"] = 790
        self.assertEqual(len(evaluator.evaluate(metric)), 1)
        metric[0]["value"] = 750
        self.assertEqual(evaluator.evaluate(metric), [])

    def test_validation_is_closed_and_bounded(self):
        manager = AlertRuleManager()
        for mutation in (
                {"metric": "arbitrary_expression"},
                {"unit": "percent"}, {"comparison": "eval"},
                {"debounce_samples": 61}, {"recovery_threshold": 800}):
            rule = dict(DEFAULT_ALERT_RULES[0]); rule.update(mutation)
            with self.assertRaises(AlertRuleError):
                manager.preflight(0, [rule])
        with self.assertRaises(AlertRuleError):
            manager.preflight(0, [dict(DEFAULT_ALERT_RULES[0])] * 17)

    def test_atomic_persistence_revision_and_single_use_token(self):
        with tempfile.TemporaryDirectory() as directory:
            store = AlertRuleStore(Path(directory))
            manager = AlertRuleManager(store)
            rules = [dict(DEFAULT_ALERT_RULES[0])]
            rules[0]["trigger_threshold"] = 850
            pending = manager.preflight(0, rules)
            self.assertFalse(pending["applied"])
            applied = manager.apply(pending["confirmation_token"],
                                    ALERT_RULE_CONFIRMATION)
            self.assertEqual(applied["revision"], 1)
            self.assertEqual(AlertRuleManager(store).snapshot()["rules"][0]
                             ["trigger_threshold"], 850)
            with self.assertRaises(AlertRuleError):
                manager.apply(pending["confirmation_token"],
                              ALERT_RULE_CONFIRMATION)
            with self.assertRaises(AlertRuleError):
                manager.preflight(0, rules)
            if os.name == "posix":
                self.assertEqual(store.path.stat().st_mode & 0o777, 0o600)

    def test_corruption_is_quarantined_and_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            store = AlertRuleStore(Path(directory))
            store.path.write_text("{broken", encoding="utf-8")
            if os.name == "posix":
                store.path.chmod(0o600)
            with self.assertRaises(AlertRuleError):
                store.load()
            self.assertFalse(store.path.exists())
            self.assertEqual(len(list(Path(directory).glob("*corrupt-*.json"))), 1)

    def test_duplicate_json_fields_do_not_enter_model(self):
        # 存储加载后仍必须经过封闭字段校验；未知脚本字段不能被忽略。
        with tempfile.TemporaryDirectory() as directory:
            store = AlertRuleStore(Path(directory))
            document = {"schema_version": 1,
                        "kind": "remotebsp-runtime-alert-rules",
                        "revision": 1,
                        "rules": [{**dict(DEFAULT_ALERT_RULES[0]),
                                   "script": "do_anything()"}]}
            store.path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaises(AlertRuleError):
                store.load()


if __name__ == "__main__":
    unittest.main()

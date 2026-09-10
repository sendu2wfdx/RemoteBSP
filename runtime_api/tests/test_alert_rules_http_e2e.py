"""Runtime 告警规则 HTTP 边界与生效链路的端到端测试。"""

import json
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from runtime_api.alert_rules import (
    ALERT_RULE_CONFIRMATION,
    AlertRuleError,
    AlertRuleManager,
    AlertRuleStorageError,
    DEFAULT_ALERT_RULES,
)
from runtime_api.auth import (
    ALERT_RULE_WRITE_PERMISSION,
    RUNTIME_READ_PERMISSION,
    ApiKeyAuthenticator,
    ApiKeyCredential,
)
from runtime_api.provider import MockSnapshotProvider
from runtime_api.server import make_server


READ_KEY = "r" * 32
WRITE_KEY = "w" * 32


class _HealthProvider(MockSnapshotProvider):
    def health_snapshot(self) -> dict:
        return {
            "producer_generation": 1,
            "sample_sequence": 1,
            "sample_time_ms": 1,
            "overall": "healthy",
            "metrics": [{
                "name": "cpu_load_permille",
                "unit": "permille",
                "availability": "available",
                "value": 810,
            }],
        }


class _FailingStore:
    def load(self) -> dict:
        return {
            "schema_version": 1,
            "kind": "remotebsp-runtime-alert-rules",
            "revision": 0,
            "rules": [dict(item) for item in DEFAULT_ALERT_RULES],
        }

    def save(self, revision, rules):
        raise AlertRuleStorageError("测试存储拒绝写入：secret-material")


class AlertRulesHttpE2ETest(unittest.TestCase):
    def setUp(self):
        authenticator = ApiKeyAuthenticator([
            ApiKeyCredential("reader", READ_KEY,
                             frozenset({RUNTIME_READ_PERMISSION})),
            ApiKeyCredential("operator", WRITE_KEY, frozenset({
                RUNTIME_READ_PERMISSION, ALERT_RULE_WRITE_PERMISSION})),
        ])
        self.manager = AlertRuleManager()
        self.audit = []
        self.server = make_server(
            "127.0.0.1", 0, _HealthProvider(),
            authenticator=authenticator, alert_rules=self.manager,
            overview_stream_interval_seconds=0.1,
            audit_output=self.audit.append)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.server.audit_sink.close()

    def _request(self, method, path, *, key=None, body=None):
        headers = {}
        encoded = None
        if key is not None:
            headers["X-API-Key"] = key
        if body is not None:
            encoded = json.dumps(body, separators=(",", ":")).encode()
            headers["Content-Type"] = "application/json"
        request = Request(self.base + path, data=encoded, headers=headers,
                          method=method)
        with urlopen(request, timeout=2) as response:
            return response.status, json.loads(response.read())

    def _error(self, method, path, expected, *, key=None, body=None):
        with self.assertRaises(HTTPError) as caught:
            self._request(method, path, key=key, body=body)
        self.assertEqual(caught.exception.code, expected)
        return json.loads(caught.exception.read())

    @staticmethod
    def _rule(threshold=805):
        rule = dict(DEFAULT_ALERT_RULES[0])
        rule["trigger_threshold"] = threshold
        rule["recovery_threshold"] = threshold - 50
        return rule

    def _preflight(self, revision=0, threshold=805):
        return self._request("POST", "/api/v1/alert-rules/preflight",
                             key=WRITE_KEY, body={
                                 "expected_revision": revision,
                                 "rules": [self._rule(threshold)],
                             })[1]["data"]

    def test_auth_permissions_strict_fields_revision_and_cas(self):
        self._error("GET", "/api/v1/alert-rules", 401)
        status, result = self._request(
            "GET", "/api/v1/alert-rules", key=READ_KEY)
        self.assertEqual(status, 200)
        self.assertEqual(result["data"]["revision"], 0)
        self._error("POST", "/api/v1/alert-rules/preflight", 403,
                    key=READ_KEY, body={"expected_revision": 0,
                                        "rules": [self._rule()]})
        self._error("POST", "/api/v1/alert-rules/preflight", 409,
                    key=WRITE_KEY, body={"expected_revision": 0,
                                         "rules": [self._rule()],
                                         "script": "secret-material"})
        self._error("POST", "/api/v1/alert-rules/preflight", 409,
                    key=WRITE_KEY, body={"expected_revision": 1,
                                         "rules": [self._rule()]})

        first = self._preflight(threshold=805)
        stale = self._preflight(threshold=850)
        self._request("POST", "/api/v1/alert-rules/apply", key=WRITE_KEY,
                      body={"confirmation_token": first["confirmation_token"],
                            "confirmation": ALERT_RULE_CONFIRMATION})
        rejected = self._error(
            "POST", "/api/v1/alert-rules/apply", 409, key=WRITE_KEY,
            body={"confirmation_token": stale["confirmation_token"],
                  "confirmation": ALERT_RULE_CONFIRMATION})
        self.assertIn("revision", rejected["error"]["message"])
        self.assertEqual(self.manager.snapshot()["revision"], 1)

    def test_token_exact_confirmation_expiry_single_use_and_no_secret_audit(self):
        pending = self._preflight()
        token = pending["confirmation_token"]
        self._error("POST", "/api/v1/alert-rules/apply", 409,
                    key=WRITE_KEY,
                    body={"confirmation_token": token,
                          "confirmation": ALERT_RULE_CONFIRMATION + " "})
        self._request("POST", "/api/v1/alert-rules/apply", key=WRITE_KEY,
                      body={"confirmation_token": token,
                            "confirmation": ALERT_RULE_CONFIRMATION})
        self._error("POST", "/api/v1/alert-rules/apply", 409,
                    key=WRITE_KEY,
                    body={"confirmation_token": token,
                          "confirmation": ALERT_RULE_CONFIRMATION})

        expiring = AlertRuleManager(token_ttl=30)
        with patch("runtime_api.alert_rules.time.monotonic",
                   side_effect=[100.0, 131.0]):
            pending = expiring.preflight(0, [self._rule()])
            with self.assertRaisesRegex(AlertRuleError, "过期"):
                expiring.apply(pending["confirmation_token"],
                               ALERT_RULE_CONFIRMATION)

        self.server.audit_sink.close()
        serialized = json.dumps(self.audit)
        self.assertNotIn(token, serialized)
        self.assertNotIn("secret-material", serialized)
        self.assertNotIn(WRITE_KEY, serialized)
        self.assertTrue(all(set(item) == {
            "schema_version", "occurred_at_ms", "request_id", "key_id",
            "method_category", "path_category", "result"}
                            for item in self.audit))

    def test_persistence_failure_keeps_old_state_and_consumes_token(self):
        manager = AlertRuleManager(_FailingStore())
        self.server.alert_rules = manager
        self.server.runtime_dashboard = type(self.server.runtime_dashboard)(
            alert_rules=manager)
        pending = self._preflight()
        error = self._error(
            "POST", "/api/v1/alert-rules/apply", 503, key=WRITE_KEY,
            body={"confirmation_token": pending["confirmation_token"],
                  "confirmation": ALERT_RULE_CONFIRMATION})
        self.assertNotIn("secret-material", json.dumps(error))
        self.assertEqual(manager.snapshot()["revision"], 0)
        self._error("POST", "/api/v1/alert-rules/apply", 409,
                    key=WRITE_KEY,
                    body={"confirmation_token": pending["confirmation_token"],
                          "confirmation": ALERT_RULE_CONFIRMATION})

    def test_applied_rule_is_consistent_in_overview_dashboard_and_sse(self):
        pending = self._preflight(threshold=805)
        self._request("POST", "/api/v1/alert-rules/apply", key=WRITE_KEY,
                      body={"confirmation_token": pending["confirmation_token"],
                            "confirmation": ALERT_RULE_CONFIRMATION})
        overview = self._request("GET", "/api/v1/overview",
                                 key=READ_KEY)[1]["data"]
        alerts = overview["toolbusd_health"]["threshold_alerts"]
        self.assertEqual(alerts[0]["trigger_threshold"], 805)

        request = Request(self.base + "/dashboard",
                          headers={"X-API-Key": READ_KEY})
        with urlopen(request, timeout=2) as response:
            self.assertIn("RemoteBSP 运行状态",
                          response.read().decode("utf-8"))

        request = Request(self.base + "/api/v1/overview/stream",
                          headers={"X-API-Key": READ_KEY})
        with urlopen(request, timeout=2) as response:
            event = response.readline() + response.readline()
        payload = json.loads(event.split(b"data: ", 1)[1])
        stream_alerts = payload["data"]["toolbusd_health"][
            "threshold_alerts"]
        self.assertEqual(stream_alerts, alerts)


if __name__ == "__main__":
    unittest.main()

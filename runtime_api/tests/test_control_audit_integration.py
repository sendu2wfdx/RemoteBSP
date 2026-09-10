import json
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from runtime_api.control_leases import (
    ControlLeaseManager,
    DaemonBoundControlLeaseManager,
)
from runtime_api.control_audit_journal import ControlAuditJournal
from runtime_api.server import make_server
from runtime_api.tests.control_audit_support import FakeControlAuditJournal
from runtime_api.tests.test_gpio_control import FakeGpioProvider, _authenticator


class ControlAuditHttpIntegrationTest(unittest.TestCase):
    def _start(self, journal):
        self.identity = "1" * 32
        self.provider = FakeGpioProvider()
        manager = DaemonBoundControlLeaseManager(
            lambda: self.identity, manager=ControlLeaseManager(8))
        self.server = make_server(
            "127.0.0.1", 0, self.provider,
            authenticator=_authenticator(),
            control_lease_manager=manager,
            control_audit_journal=journal)
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        server = getattr(self, "server", None)
        if server is not None:
            server.shutdown()
            server.server_close()
            self.thread.join(timeout=2)

    def _request(self, method, path, body=None, *, key="a" * 32):
        data = None if body is None else json.dumps(body).encode("utf-8")
        headers = {"X-API-Key": key}
        if body is not None:
            headers["Content-Type"] = "application/json"
        return Request(self.base + path, data=data, headers=headers,
                       method=method)

    @staticmethod
    def _lease_body():
        return {
            "node_id": "mock-node-1",
            "resource_id": "gpio-0",
            "command_group": "gpio.write",
            "ttl_ms": 1000,
            "idempotency_key": "lease-audit-1",
        }

    def _error(self, request):
        with self.assertRaises(HTTPError) as caught:
            urlopen(request)
        return caught.exception.code, json.loads(caught.exception.read())

    def _acquire(self):
        with urlopen(self._request(
                "POST", "/api/v1/control-leases",
                self._lease_body())) as response:
            return json.loads(response.read())["data"]["lease"]["lease_id"]

    def test_without_journal_reads_work_and_mutation_stops_before_state_change(self):
        self._start(None)
        with urlopen(self._request("GET", "/api/v1")) as response:
            root = json.loads(response.read())["data"]["capabilities"]
        self.assertTrue(root["read_only"])
        self.assertFalse(root["control_audit"]["configured"])
        self.assertFalse(root["control_leases"]["mutation_available"])

        status, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", self._lease_body()))
        self.assertEqual(status, 503)
        self.assertEqual(payload["error"]["code"],
                         "control_audit_unavailable")
        self.assertEqual(payload["error"]["details"], {
            "possibly_committed": False,
            "safe_to_retry": True,
        })
        self.assertEqual(self.server.control_leases.active_count(), 0)
        self.assertEqual(self.provider.acquires, [])

    def test_acquire_intent_is_durable_before_lease_and_downstream_mutation(self):
        journal = FakeControlAuditJournal()
        self._start(journal)

        def observe_acquire(_arguments):
            journal.events.append(("downstream_acquire",))

        self.provider.acquire_hook = observe_acquire
        lease_id = self._acquire()
        kinds = [event[0] for event in journal.events]
        self.assertLess(kinds.index("intent"), kinds.index("downstream_acquire"))
        self.assertLess(kinds.index("downstream_acquire"),
                        kinds.index("terminal"))
        intent = next(event[1] for event in journal.events
                      if event[0] == "intent")
        self.assertEqual(intent["action"], "acquire")
        self.assertIsNone(intent["lease_id"])
        self.assertNotIn("api_key", json.dumps(intent))
        self.assertEqual(len(lease_id), 32)

    def test_gpio_terminal_failure_returns_no_success_but_operation_is_queryable(self):
        journal = FakeControlAuditJournal()
        self._start(journal)
        lease_id = self._acquire()
        journal.fail_terminal = True
        write = {
            "lease_id": lease_id,
            "node_id": "mock-node-1",
            "resource_id": "gpio-0",
            "idempotency_key": "write-audit-1",
            "value": True,
        }
        status, payload = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", write))
        self.assertEqual(status, 503)
        self.assertEqual(payload["error"]["code"],
                         "control_audit_persistence_failed")
        details = payload["error"]["details"]
        self.assertTrue(details["possibly_committed"])
        self.assertFalse(details["safe_to_retry"])
        operation = details["operation"]
        self.assertEqual(operation["state"], "committed")
        operation_id = operation["operation_id"]
        self.assertEqual(len(self.provider.writes), 1)

        with urlopen(self._request(
                "GET", "/api/v1/control/operations/" + operation_id)) \
                as response:
            queried = json.loads(response.read())["data"]["operation"]
        self.assertEqual(queried["state"], "committed")
        self.assertEqual(len(self.provider.writes), 1)

        with urlopen(self._request("GET", "/api/v1/health")) as response:
            health = json.loads(response.read())["data"]
        self.assertFalse(health["runtime_control_audit"]["operational"])
        with urlopen(self._request("GET", "/api/v1")) as response:
            capabilities = json.loads(response.read())["data"]["capabilities"]
        self.assertFalse(capabilities["gpio_write"]["operational"])
        self.assertTrue(capabilities["operation_ledger"]["operational"])

    def test_acquire_terminal_failure_keeps_lease_and_closes_new_mutation(self):
        journal = FakeControlAuditJournal()
        self._start(journal)
        journal.fail_terminal = True

        status, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", self._lease_body()))
        self.assertEqual(status, 503)
        self.assertEqual(payload["error"]["code"],
                         "control_audit_persistence_failed")
        self.assertTrue(payload["error"]["details"]["possibly_committed"])
        self.assertFalse(payload["error"]["details"]["safe_to_retry"])
        self.assertEqual(self.server.control_leases.active_count(), 1)
        self.assertEqual(len(self.provider.acquires), 1)

        second = dict(self._lease_body(), resource_id="gpio-other",
                      idempotency_key="lease-audit-2")
        status, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", second))
        self.assertEqual(status, 503)
        self.assertEqual(payload["error"]["code"],
                         "control_audit_unavailable")
        self.assertEqual(len(self.provider.acquires), 1)

    def test_admin_release_terminal_failure_preserves_private_recovery(self):
        journal = FakeControlAuditJournal()
        self._start(journal)
        lease_id = self._acquire()
        journal.fail_terminal = True

        status, payload = self._error(self._request(
            "DELETE", "/api/v1/control-leases/" + lease_id,
            key="c" * 32))
        self.assertEqual(status, 503)
        self.assertEqual(payload["error"]["code"],
                         "control_audit_persistence_failed")
        details = payload["error"]["details"]
        self.assertTrue(details["possibly_committed"])
        self.assertFalse(details["safe_to_retry"])
        operation_id = details["operation"]["operation_id"]
        self.assertNotIn("operator", json.dumps(payload))
        self.assertNotIn("owner", json.dumps(payload))
        self.assertEqual(self.server.control_leases.active_count(), 1)

        with urlopen(self._request(
                "GET", "/api/v1/control/operations/" + operation_id,
                key="c" * 32)) as response:
            operation = json.loads(response.read())["data"]["operation"]
        self.assertEqual(operation["state"], "committed")
        self.assertEqual(operation["operation_kind"], "control_release")
        self.assertNotIn("owner", json.dumps(operation))
        self.assertEqual(len(self.provider.releases), 1)

    def test_real_journal_survives_full_mutation_sequence_and_restart(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            key = root / "audit.key"
            key.write_bytes(bytes(range(32)))
            key.chmod(0o600)
            directory = root / "journal"
            self._start(ControlAuditJournal(directory, key))

            lease_id = self._acquire()
            write = {
                "lease_id": lease_id,
                "node_id": "mock-node-1",
                "resource_id": "gpio-0",
                "idempotency_key": "write-audit-real-1",
                "value": True,
            }
            with urlopen(self._request(
                    "POST", "/api/v1/control/gpio/write", write)) as response:
                self.assertEqual(response.status, 200)
            with urlopen(self._request(
                    "DELETE", f"/api/v1/control-leases/{lease_id}")) as response:
                self.assertEqual(response.status, 200)

            self.server.shutdown()
            self.server.server_close()
            self.thread.join(timeout=2)
            self.server = None

            persisted = b"".join(
                path.read_bytes() for path in directory.iterdir())
            self.assertNotIn(b"lease-audit-1", persisted)
            self.assertNotIn(b"write-audit-real-1", persisted)
            self.assertNotIn(b"mock-node-1", persisted)
            self.assertNotIn(b"gpio-0", persisted)

            with ControlAuditJournal(directory, key) as reopened:
                records = reopened.snapshot()
                self.assertEqual(
                    [record.state for record in records],
                    ["intent", "terminal"] * 3)
                self.assertEqual(
                    [record.action for record in records if
                     record.state == "intent"],
                    ["acquire", "gpio_write", "release"])
                terminals = [record for record in records
                             if record.state == "terminal"]
                self.assertEqual(
                    [record.result for record in terminals],
                    ["committed", "committed", "released"])
                self.assertIsNone(terminals[0].operation_id)
                self.assertTrue(all(record.operation_id is not None
                                    for record in terminals[1:]))


if __name__ == "__main__":
    unittest.main()

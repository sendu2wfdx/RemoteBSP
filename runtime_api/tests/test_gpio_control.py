import json
import subprocess
import threading
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from runtime_api.auth import (
    CONTROL_OPERATION_READ_PERMISSION,
    CONTROL_LEASE_ACQUIRE_PERMISSION,
    CONTROL_LEASE_RELEASE_PERMISSION,
    CONTROL_LEASE_REVOKE_PERMISSION,
    GPIO_WRITE_PERMISSION,
    PWM_WRITE_PERMISSION,
    RUNTIME_READ_PERMISSION,
    ApiKeyAuthenticator,
    ApiKeyCredential,
)
from runtime_api.control_leases import (
    ControlLeaseManager,
    DaemonBoundControlLeaseManager,
)
from runtime_api.provider import (
    MockSnapshotProvider,
    RuntimeProviderError,
    RuntimeProviderOperationError,
)
from runtime_api.server import _ForeignOperationRecoveryIndex, make_server
from runtime_api.tests.control_audit_support import FakeControlAuditJournal
from runtime_api.toolbusd_provider import (
    RemoteCliIpcClient,
    ToolbusIpcOperationError,
    ToolbusIpcProtocolError,
    ToolbusdSnapshotProvider,
)


class FakeGpioProvider(MockSnapshotProvider):
    gpio_control_available = True
    pwm_control_available = True

    def __init__(self):
        super().__init__()
        self.acquires = []
        self.writes = []
        self.releases = []
        self._completed = {}
        self._operations = {}
        self.lookup_calls = []
        self.lookup_owners = []
        self.lookup_failures = 0
        self._operation_owners = {}
        self.status_daemons = []
        self.after_status = None
        self.fail_resource = None
        self.fail_writes = False
        self.fail_releases = False
        self.after_acquire = None
        self.acquire_hook = None
        self.write_hook = None
        self.after_release = None

    def gpio_control_acquire(self, *arguments):
        arguments = (*arguments[:-1], arguments[-1]())
        self.acquires.append(arguments)
        if arguments[4] == self.fail_resource:
            raise RuntimeProviderOperationError(
                "target_rejected", category="target", retryable=False,
                possibly_committed=False,
                detail="不应回显的 /secret/socket 路径")
        if self.acquire_hook is not None:
            self.acquire_hook(arguments)
        if self.after_acquire is not None:
            self.after_acquire()

    def gpio_control_write(self, daemon_id, lease_id, owner, node_id,
                           resource_id, idempotency_key, value):
        if self.fail_writes:
            raise RuntimeProviderError("toolbusd拒绝了目标GPIO合同")
        arguments = (daemon_id, lease_id, owner, node_id, resource_id,
                     idempotency_key, value)
        if self.write_hook is not None:
            self.write_hook(arguments)
        replayed = arguments in self._completed
        self._completed[arguments] = True
        self.writes.append(arguments)
        operation_id = ("1" if value else "2") * 64
        outcome = {
            "operation_id": operation_id, "lease_id": lease_id,
            "expected_node_uuid": "a" * 32, "resource_id": 0x01000001,
            "kind": "gpio_write",
            "state": "committed", "replayed": replayed,
            "recovery": "none", "object_id": 73, "value": value,
            "error_code": None,
        }
        self._operations[("gpio_write", lease_id, idempotency_key)] = outcome
        self._operations[operation_id] = outcome
        return outcome

    def gpio_control_release(self, *arguments):
        if self.fail_releases:
            raise RuntimeProviderError("toolbusd释放路径不可用")
        self.releases.append(arguments)
        outcome = {
            "operation_id": "3" * 64, "lease_id": arguments[1],
            "expected_node_uuid": "a" * 32, "resource_id": 0x01000001,
            "kind": "control_release",
            "state": "committed", "replayed": False,
            "recovery": "safe_closed", "object_id": None,
            "value": None, "error_code": None,
        }
        self._operations[("control_release", arguments[1], "release:v1")] = \
            outcome
        self._operation_owners[(
            "control_release", arguments[1], "release:v1")] = arguments[2]
        self._operation_owners[outcome["operation_id"]] = arguments[2]
        self._operations[outcome["operation_id"]] = outcome
        if self.after_release is not None:
            self.after_release()
        return outcome

    pwm_control_acquire = gpio_control_acquire

    def pwm_control_configure(self, _daemon, lease, _owner, _node, _resource,
                              idem, frequency, duty, active_low):
        outcome = {"operation_id": "8" * 64, "lease_id": lease,
            "expected_node_uuid": "a" * 32, "resource_id": 0x06000000,
            "kind": "pwm_configure", "state": "committed", "replayed": False,
            "recovery": "none", "object_id": 81, "value": None,
            "frequency_hz": frequency, "duty": duty, "active_low": active_low,
            "error_code": None}
        self._operations[("pwm_configure", lease, idem)] = outcome
        return outcome

    def pwm_control_stop(self, _daemon, lease, _owner, _node, _resource, idem):
        outcome = {"operation_id": "9" * 64, "lease_id": lease,
            "expected_node_uuid": "a" * 32, "resource_id": 0x06000000,
            "kind": "pwm_stop", "state": "committed", "replayed": False,
            "recovery": "safe_closed", "object_id": 81, "value": None,
            "frequency_hz": None, "duty": None, "active_low": None,
            "error_code": None}
        self._operations[("pwm_stop", lease, idem)] = outcome
        return outcome

    def operation_status(self, daemon_id, owner, operation_id):
        self.status_daemons.append(daemon_id)
        expected_owner = self._operation_owners.get(operation_id)
        outcome = None if expected_owner is not None and \
            owner != expected_owner else self._operations.get(operation_id)
        outcome = outcome or {
            "operation_id": operation_id, "lease_id": None,
            "expected_node_uuid": None, "resource_id": None, "kind": None,
            "state": "expired_unknown", "replayed": True,
            "recovery": "none", "object_id": None, "value": None,
            "error_code": "history_expired",
        }
        if self.after_status is not None:
            self.after_status()
        return outcome

    def operation_lookup(self, _daemon_id, owner, kind, lease_id,
                         idempotency_key):
        selector = (kind, lease_id, idempotency_key)
        self.lookup_calls.append(selector)
        self.lookup_owners.append(owner)
        if self.lookup_failures > 0:
            self.lookup_failures -= 1
            raise RuntimeProviderError("模拟即时账本定位暂不可用")
        expected_owner = self._operation_owners.get(selector)
        outcome = self._operations.get(selector)
        if expected_owner is not None and owner != expected_owner:
            outcome = None
        if outcome is not None:
            return {**outcome, "replayed": True}
        return {
            "operation_id": "4" * 64, "lease_id": None,
            "expected_node_uuid": None, "resource_id": None, "kind": kind,
            "state": "expired_unknown", "replayed": True,
            "recovery": "none", "object_id": None, "value": None,
            "error_code": "history_expired",
        }


class RemoteCliGpioControlTest(unittest.TestCase):
    @staticmethod
    def _error_document(command="runtime-gpio-write", *, code=103,
                        category=4, retryable=False,
                        possibly_committed=False):
        return json.dumps({
            "schema_version": 1,
            "command": command,
            "error": {
                "ipc_error_version": 1,
                "code": code,
                "category": category,
                "retryable": retryable,
                "possibly_committed": possibly_committed,
                "message": "Runtime 控制请求失败",
            },
        }).encode("utf-8")

    def test_real_runner_parses_nonzero_stdout_error_document(self):
        completed = subprocess.CompletedProcess(
            args=[], returncode=1,
            stdout=self._error_document(), stderr=b"")
        with patch(
                "runtime_api.toolbusd_provider.subprocess.run",
                return_value=completed):
            client = RemoteCliIpcClient("/tmp/toolbusd-test.sock")
            with self.assertRaises(ToolbusIpcOperationError) as caught:
                client.runtime_gpio_write(
                    "1" * 32, "2" * 32, "3" * 32, "operator", 3,
                    0x01000005, "write-1", True)
        self.assertEqual(caught.exception.code, 103)
        self.assertEqual(caught.exception.category, 4)
        self.assertFalse(caught.exception.retryable)
        self.assertFalse(caught.exception.possibly_committed)

    def test_three_commands_use_strict_structured_contract(self):
        calls = []

        def runner(command, _timeout, _maximum_output):
            calls.append(command)
            operation = next(name for name in (
                "runtime-control-acquire", "runtime-gpio-write",
                "runtime-control-release") if name in command)
            data = ({"object_id": 9, "value": True, "replayed": False}
                    if operation == "runtime-gpio-write" else {})
            return json.dumps({
                "schema_version": 1, "command": operation, "data": data,
            })

        client = RemoteCliIpcClient("/tmp/toolbusd-test.sock", runner=runner)
        client.runtime_control_acquire(
            "1" * 32, "2" * 32, "3" * 32, "operator", 3,
            0x01000005, 1000)
        result = client.runtime_gpio_write(
            "1" * 32, "2" * 32, "3" * 32, "operator", 3, 0x01000005,
            "write-1", True)
        client.runtime_control_release("1" * 32, "2" * 32, "operator")
        self.assertEqual(result, {
            "object_id": 9, "value": True, "replayed": False})
        self.assertEqual(len(calls), 3)
        self.assertIn("--node", calls[0])
        self.assertNotIn("--node", calls[2])

    def test_write_rejects_mismatched_or_unknown_result(self):
        for data in (
                {"object_id": 9, "value": False, "replayed": False},
                {"object_id": 9, "value": True, "replayed": False,
                 "extra": 1}):
            with self.subTest(data=data), self.assertRaises(
                    ToolbusIpcProtocolError):
                client = RemoteCliIpcClient(
                    "/tmp/toolbusd-test.sock",
                    runner=lambda command, *_: json.dumps({
                        "schema_version": 1,
                        "command": "runtime-gpio-write", "data": data,
                    }))
                client.runtime_gpio_write(
                    "1" * 32, "2" * 32, "3" * 32, "operator", 3,
                    0x01000005, "write-1", True)

    @staticmethod
    def _operation_document(command, **changes):
        data = {
            "operation_id": "a" * 64, "lease_id": "2" * 32,
            "expected_node_uuid": "3" * 32,
            "resource_id": 0x01000005, "kind": "gpio_write",
            "state": "committed", "replayed": False,
            "recovery": "none", "object_id": 9, "value": True,
            "error_code": None,
        }
        data.update(changes)
        return json.dumps({
            "schema_version": 1, "command": command, "data": data,
        })

    def test_operation_commands_and_strict_88_byte_contract(self):
        def runner(command, *_arguments):
            operation = next(name for name in (
                "runtime-gpio-write-operation",
                "runtime-control-release-operation",
                "runtime-operation-status", "runtime-operation-lookup")
                if name in command)
            if operation == "runtime-control-release-operation":
                return self._operation_document(
                    operation, kind="control_release", state="committed",
                    recovery="safe_closed", object_id=None, value=None)
            if operation == "runtime-operation-status":
                return self._operation_document(operation, replayed=True)
            if operation == "runtime-operation-lookup":
                return self._operation_document(operation, replayed=True)
            return self._operation_document(operation)

        client = RemoteCliIpcClient("/tmp/toolbusd-test.sock", runner=runner)
        write = client.runtime_gpio_write_operation(
            "1" * 32, "2" * 32, "3" * 32, "operator", 3,
            0x01000005, "write-1", True)
        released = client.runtime_control_release_operation(
            "1" * 32, "2" * 32, "operator")
        status = client.runtime_operation_status(
            "1" * 32, "operator", "a" * 64)
        located = client.runtime_operation_lookup(
            "1" * 32, "operator", "gpio_write", "2" * 32, "write-1")
        self.assertEqual(write["object_id"], 9)
        self.assertEqual(released["recovery"], "safe_closed")
        self.assertTrue(status["replayed"])
        self.assertEqual(located["expected_node_uuid"], "3" * 32)

    def test_operation_contract_rejects_invalid_scope_and_state_combinations(
            self):
        invalid = (
            {"operation_id": "A" * 64},
            {"lease_id": None},
            {"expected_node_uuid": "0" * 32},
            {"resource_id": 0},
            {"state": "pending", "object_id": None, "value": None,
             "error_code": "backend"},
            {"state": "unknown", "recovery": "none", "object_id": None,
             "value": None, "error_code": "backend"},
            {"state": "expired_unknown", "lease_id": None,
             "expected_node_uuid": None, "resource_id": None, "kind": None,
             "recovery": "none", "object_id": None, "value": None,
             "error_code": "history_expired"},
        )
        for changes in invalid:
            with self.subTest(changes=changes), self.assertRaises(
                    ToolbusIpcProtocolError):
                client = RemoteCliIpcClient(
                    "/tmp/toolbusd-test.sock",
                    runner=lambda *_: self._operation_document(
                        "runtime-operation-lookup", replayed=True, **changes))
                client.runtime_operation_lookup(
                    "1" * 32, "operator", "gpio_write", "2" * 32,
                    "write-1")

        client = RemoteCliIpcClient(
            "/tmp/toolbusd-test.sock",
            runner=lambda *_: self._operation_document(
                "runtime-operation-status", replayed=True,
                state="expired_unknown", lease_id=None,
                expected_node_uuid=None, resource_id=None, kind=None,
                recovery="none", object_id=None, value=None,
                error_code="history_expired"))
        expired = client.runtime_operation_status(
            "1" * 32, "operator", "a" * 64)
        self.assertIsNone(expired["kind"])
        self.assertEqual(expired["state"], "expired_unknown")


def _authenticator():
    return ApiKeyAuthenticator([
        ApiKeyCredential(
            "operator", "a" * 32,
            frozenset({RUNTIME_READ_PERMISSION,
                       CONTROL_OPERATION_READ_PERMISSION,
                       CONTROL_LEASE_ACQUIRE_PERMISSION,
                       CONTROL_LEASE_RELEASE_PERMISSION,
                       GPIO_WRITE_PERMISSION, PWM_WRITE_PERMISSION})),
        ApiKeyCredential(
            "other", "b" * 32,
            frozenset({CONTROL_LEASE_ACQUIRE_PERMISSION,
                       CONTROL_LEASE_RELEASE_PERMISSION,
                       GPIO_WRITE_PERMISSION})),
        ApiKeyCredential(
            "supervisor", "c" * 32,
            frozenset({CONTROL_LEASE_REVOKE_PERMISSION,
                       CONTROL_OPERATION_READ_PERMISSION})),
        ApiKeyCredential(
            "lease-only", "d" * 32,
            frozenset({CONTROL_LEASE_ACQUIRE_PERMISSION})),
    ])


class GpioControlHttpTest(unittest.TestCase):
    def setUp(self):
        self.identity = "1" * 32
        self.now_ns = [1_000_000_000]
        self.provider = FakeGpioProvider()
        inner = ControlLeaseManager(
            8, monotonic_ns=lambda: self.now_ns[0],
            wall_time_ms=lambda: self.now_ns[0] // 1_000_000)
        manager = DaemonBoundControlLeaseManager(
            lambda: self.identity, manager=inner)
        self.server = make_server(
            "127.0.0.1", 0, self.provider,
            authenticator=_authenticator(), control_lease_manager=manager,
            control_audit_journal=FakeControlAuditJournal())
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def _request(self, method, path, key, body=None):
        data = None if body is None else json.dumps(body).encode("utf-8")
        headers = {"X-API-Key": key}
        if body is not None:
            headers["Content-Type"] = "application/json"
        return Request(self.base + path, data=data, headers=headers,
                       method=method)

    def _error(self, request):
        with self.assertRaises(HTTPError) as caught:
            urlopen(request)
        return caught.exception.code, json.loads(caught.exception.read())

    @staticmethod
    def _lease_body():
        return {
            "node_id": "mock-node-1", "resource_id": "gpio-0",
            "command_group": "gpio.write", "ttl_ms": 1000,
            "idempotency_key": "lease-request-1",
        }

    def _acquire(self):
        with urlopen(self._request(
                "POST", "/api/v1/control-leases", "a" * 32,
                self._lease_body())) as response:
            self.assertEqual(response.status, 201)
            return json.loads(response.read())["data"]["lease"]["lease_id"]

    def test_pwm_http_contract_permissions_validation_scope_and_commits(self):
        lease_body = {"node_id": "mock-node-1", "resource_id": "pwm-0",
            "command_group": "pwm.write", "ttl_ms": 1000,
            "idempotency_key": "pwm-lease-1"}
        code, _ = self._error(self._request(
            "POST", "/api/v1/control-leases", "d" * 32, lease_body))
        self.assertEqual(code, 403)
        with urlopen(self._request("POST", "/api/v1/control-leases",
                                  "a" * 32, lease_body)) as response:
            lease_id = json.loads(response.read())["data"]["lease"]["lease_id"]
        base = {"lease_id": lease_id, "node_id": "mock-node-1",
                "resource_id": "pwm-0", "idempotency_key": "pwm-config-1",
                "frequency_hz": 20000, "duty": 4200, "active_low": False}
        code, _ = self._error(self._request(
            "POST", "/api/v1/control/pwm/configure", "a" * 32,
            {**base, "duty": 10001}))
        self.assertEqual(code, 400)
        code, _ = self._error(self._request(
            "POST", "/api/v1/control/pwm/configure", "a" * 32,
            {**base, "resource_id": "pwm-other"}))
        self.assertEqual(code, 409)
        with urlopen(self._request("POST", "/api/v1/control/pwm/configure",
                                  "a" * 32, base)) as response:
            configured = json.loads(response.read())["data"]["operation"]
        self.assertEqual(configured["result"], {"object_id": 81,
            "frequency_hz": 20000, "duty": 4200, "active_low": False})
        stop = {"lease_id": lease_id, "node_id": "mock-node-1",
                "resource_id": "pwm-0", "idempotency_key": "pwm-stop-1"}
        with urlopen(self._request("POST", "/api/v1/control/pwm/stop",
                                  "a" * 32, stop)) as response:
            stopped = json.loads(response.read())["data"]["operation"]
        self.assertEqual(stopped["result"], {"object_id": 81, "stopped": True})

        def uncertain(_daemon, lease, _owner, _node, _resource, idem,
                      frequency, duty, active_low):
            outcome = {"operation_id": "7" * 64, "lease_id": lease,
                "expected_node_uuid": "a" * 32, "resource_id": 0x06000000,
                "kind": "pwm_configure", "state": "committed", "replayed": False,
                "recovery": "none", "object_id": 82, "value": None,
                "frequency_hz": frequency, "duty": duty,
                "active_low": active_low, "error_code": None}
            self.provider._operations[("pwm_configure", lease, idem)] = outcome
            raise RuntimeProviderOperationError(
                "backend_unavailable", category="transport",
                retryable=False, possibly_committed=True)
        self.provider.pwm_control_configure = uncertain
        recovered_request = {**base, "idempotency_key": "pwm-recover-1"}
        with urlopen(self._request("POST", "/api/v1/control/pwm/configure",
                                  "a" * 32, recovered_request)) as response:
            recovered = json.loads(response.read())["data"]["operation"]
        self.assertEqual(recovered["operation_id"], "7" * 64)
        self.assertTrue(recovered["replayed"])
        self.assertIn(("pwm_configure", lease_id, "pwm-recover-1"),
                      self.provider.lookup_calls)

    def test_request_deadline_returns_sanitized_gateway_timeout(self):
        self.server.request_io_timeout_seconds = 0.1  # type: ignore[attr-defined]

        def expire_after_downstream_started(_arguments):
            time.sleep(0.11)
            raise RuntimeProviderError(
                "remote-cli /secret/socket token=not-for-http")

        self.provider.acquire_hook = expire_after_downstream_started
        status, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", "a" * 32,
            self._lease_body()))
        self.assertEqual(status, 504)
        self.assertEqual(payload["error"]["code"],
                         "control_deadline_exceeded")
        encoded = json.dumps(payload)
        self.assertNotIn("secret", encoded)
        self.assertNotIn("token", encoded)
        # 调用已进入下游且结果迟到，提交状态未知；本地租约必须保留至TTL，
        # 不能盲目回滚后允许另一个所有者进入同一scope。
        self.assertEqual(self.server.control_leases.active_count(), 1)  # type: ignore[attr-defined]

    def test_acquire_write_replay_and_admin_release_are_server_derived(self):
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        capabilities = root["data"]["capabilities"]
        self.assertFalse(capabilities["write_commands"])
        self.assertEqual(capabilities["gpio_write"], {
            "configured": True, "operational": False})

        lease_id = self._acquire()
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])
        self.assertTrue(root["data"]["capabilities"][
            "control_leases"]["downstream_commands"])
        self.assertEqual(self.provider.acquires[0][0], "1" * 32)
        self.assertGreaterEqual(self.provider.acquires[0][-1], 1)
        self.assertLessEqual(self.provider.acquires[0][-1], 1000)
        body = {
            "lease_id": lease_id, "node_id": "mock-node-1",
            "resource_id": "gpio-0", "idempotency_key": "write-1",
            "value": True,
        }
        for expected_replay in (False, True):
            with urlopen(self._request(
                    "POST", "/api/v1/control/gpio/write", "a" * 32,
                    body)) as response:
                payload = json.loads(response.read())
            operation = payload["data"]["operation"]
            self.assertEqual(operation["replayed"], expected_replay)
            self.assertEqual(operation["state"], "committed")
            self.assertEqual(operation["result"]["object_id"], 73)

        code, payload = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "b" * 32, body))
        self.assertEqual(code, 403)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_not_owner")

        with urlopen(self._request(
                "DELETE", f"/api/v1/control-leases/{lease_id}",
                "c" * 32)) as response:
            self.assertEqual(response.status, 200)
            released = json.loads(response.read())["data"]["operation"]
            self.assertEqual(released["operation_kind"], "control_release")
            self.assertEqual(released["result"], {"released": True})
        self.assertEqual(self.provider.releases,
                         [("1" * 32, lease_id, "operator")])
        records = self.server.audit_sink.snapshot()  # type: ignore[attr-defined]
        self.assertIn("gpio_write_committed",
                      {record.result for record in records})
        self.assertEqual(
            [record.path_category for record in records
             if record.result == "gpio_write_committed"],
            ["gpio_control", "gpio_control"])

    def test_admin_release_recovers_with_original_lease_owner(self):
        lease_id = self._acquire()

        def lose_response_after_commit():
            raise RuntimeProviderOperationError(
                "backend_unavailable", category="transport",
                retryable=False, possibly_committed=True)

        self.provider.after_release = lose_response_after_commit
        with urlopen(self._request(
                "DELETE", f"/api/v1/control-leases/{lease_id}",
                "c" * 32)) as response:
            self.assertEqual(response.status, 200)
            operation = json.loads(response.read())["data"]["operation"]
        self.assertEqual(operation["operation_id"], "3" * 64)
        self.assertEqual(operation["operation_kind"], "control_release")
        self.assertEqual(operation["result"], {"released": True})
        self.assertEqual(self.provider.lookup_owners, ["operator"])
        self.assertEqual(self.provider.lookup_calls, [
            ("control_release", lease_id, "release:v1")])
        self.assertEqual(
            self.server.control_leases.active_count(), 0)  # type: ignore[attr-defined]

    def test_admin_release_can_recover_later_without_client_owner(self):
        lease_id = self._acquire()

        def lose_response_after_commit():
            raise RuntimeProviderOperationError(
                "backend_unavailable", category="transport",
                retryable=False, possibly_committed=True)

        self.provider.after_release = lose_response_after_commit
        self.provider.lookup_failures = 2
        failed = None
        for _ in range(2):
            code, failed = self._error(self._request(
                "DELETE", f"/api/v1/control-leases/{lease_id}", "c" * 32))
            self.assertEqual(code, 409)
            self.assertEqual(failed["error"]["code"],
                             "control_operation_lookup_required")
            entries = self.server.foreign_operation_recovery._entries  # type: ignore[attr-defined]
            self.assertEqual(len(entries), 1)
            self.assertEqual(len(next(iter(entries.values())).reservations), 0)
        self.assertIsNotNone(failed)
        lookup = failed["error"]["details"]["lookup"]
        self.assertEqual(set(lookup["body"]), {
            "operation_kind", "lease_id", "idempotency_key"})
        self.assertNotIn("owner", json.dumps(lookup, sort_keys=True))
        self.assertEqual(
            self.server.control_leases.active_count(), 1)  # type: ignore[attr-defined]

        forged = {**lookup["body"], "owner_key_id": "operator"}
        forged_code, _ = self._error(self._request(
            "POST", "/api/v1/control/operation-lookups", "c" * 32,
            forged))
        self.assertEqual(forged_code, 400)

        with urlopen(self._request(
                lookup["method"], lookup["path"], "c" * 32,
                lookup["body"])) as response:
            self.assertEqual(response.status, 200)
            payload = json.loads(response.read())
            location = response.headers["Location"]
        operation = payload["data"]["operation"]
        self.assertEqual(operation["operation_kind"], "control_release")
        self.assertEqual(operation["result"], {"released": True})
        self.assertNotIn("owner", json.dumps(payload, sort_keys=True))
        self.assertEqual(self.provider.lookup_owners,
                         ["operator", "operator", "operator"])
        self.assertEqual(
            self.server.control_leases.active_count(), 0)  # type: ignore[attr-defined]

        # 定位成功后，服务端把operation ID绑定到同一有界恢复记录；管理员
        # 跟随Location时仍使用原owner查询，HTTP中不暴露owner字段。
        with urlopen(self._request("GET", location, "c" * 32)) as response:
            queried = json.loads(response.read())
        self.assertEqual(queried["data"]["operation"]["operation_id"],
                         operation["operation_id"])

    def test_admin_recovery_reservation_fails_before_downstream(self):
        lease_id = self._acquire()
        recovery = self.server.foreign_operation_recovery  # type: ignore[attr-defined]
        recovery.capacity = 1
        token, error = recovery.reserve(
            "another-supervisor", "operator", kind="control_release",
            lease_id="f" * 32, idempotency_key="release:v1")
        self.assertIsNotNone(token)
        self.assertIsNone(error)

        code, payload = self._error(self._request(
            "DELETE", f"/api/v1/control-leases/{lease_id}", "c" * 32))
        self.assertEqual(code, 503)
        self.assertEqual(payload["error"]["code"],
                         "control_recovery_locator_capacity_exceeded")
        self.assertEqual(self.provider.releases, [])

    def test_admin_recovery_owner_conflict_fails_before_downstream(self):
        lease_id = self._acquire()
        recovery = self.server.foreign_operation_recovery  # type: ignore[attr-defined]
        token, error = recovery.reserve(
            "supervisor", "stale-owner", kind="control_release",
            lease_id=lease_id, idempotency_key="release:v1")
        self.assertIsNotNone(token)
        self.assertIsNone(error)

        code, payload = self._error(self._request(
            "DELETE", f"/api/v1/control-leases/{lease_id}", "c" * 32))
        self.assertEqual(code, 409)
        self.assertEqual(payload["error"]["code"],
                         "control_recovery_locator_conflict")
        self.assertEqual(self.provider.releases, [])

    def test_admin_recovery_binding_expires_without_owner_fallback(self):
        lease_id = self._acquire()
        now_ns = [1_000_000_000]
        self.server.foreign_operation_recovery = _ForeignOperationRecoveryIndex(  # type: ignore[attr-defined]
            monotonic_ns=lambda: now_ns[0])

        def lose_response_after_commit():
            raise RuntimeProviderOperationError(
                "backend_unavailable", category="transport",
                retryable=False, possibly_committed=True)

        self.provider.after_release = lose_response_after_commit
        self.provider.lookup_failures = 1
        code, failed = self._error(self._request(
            "DELETE", f"/api/v1/control-leases/{lease_id}", "c" * 32))
        self.assertEqual(code, 409)
        now_ns[0] += 24 * 60 * 60 * 1_000_000_000 + 1

        lookup = failed["error"]["details"]["lookup"]
        code, payload = self._error(self._request(
            lookup["method"], lookup["path"], "c" * 32,
            lookup["body"]))
        self.assertEqual(code, 409)
        self.assertEqual(payload["error"]["code"],
                         "control_operation_expired_unknown")
        self.assertEqual(self.provider.lookup_owners,
                         ["operator", "supervisor"])

    def test_admin_recovery_reservations_are_transaction_isolated(self):
        now_ns = [1_000_000_000]
        recovery = _ForeignOperationRecoveryIndex(
            capacity=1, monotonic_ns=lambda: now_ns[0])
        selector = {
            "kind": "control_release", "lease_id": "1" * 32,
            "idempotency_key": "release:v1",
        }

        first, error = recovery.reserve(
            "supervisor", "operator", **selector)
        self.assertIsNotNone(first)
        self.assertIsNone(error)
        second, error = recovery.reserve(
            "supervisor", "operator", **selector)
        self.assertIsNotNone(second)
        self.assertIsNone(error)
        self.assertNotEqual(first, second)

        # 第一个请求的确定失败只能撤销自己的token，第二个未决请求仍能
        # 使用服务端保存的真实owner做延后定位。
        recovery.forget(
            "supervisor", reservation_token=first, **selector)
        self.assertEqual(recovery.owner_for_selector(
            "supervisor", **selector), "operator")

        self.assertTrue(recovery.retain_for_lookup(
            "supervisor", reservation_token=second, **selector))
        entry = next(iter(recovery._entries.values()))
        self.assertEqual(len(entry.reservations), 0)

        # 同一selector重复进入uncertain只刷新一个retained占位，每次请求的
        # 临时token都会被消费，不能借此绕过entry容量造成集合增长。
        third, error = recovery.reserve(
            "supervisor", "operator", **selector)
        self.assertIsNotNone(third)
        self.assertIsNone(error)
        self.assertTrue(recovery.retain_for_lookup(
            "supervisor", reservation_token=third, **selector))
        self.assertEqual(len(entry.reservations), 0)

        fourth, error = recovery.reserve(
            "supervisor", "operator", **selector)
        self.assertIsNotNone(fourth)
        self.assertIsNone(error)
        fifth, error = recovery.reserve(
            "supervisor", "operator", **selector)
        self.assertIsNotNone(fifth)
        self.assertIsNone(error)
        operation_id = "2" * 64
        self.assertTrue(recovery.bind_operation(
            "supervisor", operation_id,
            reservation_token=fourth, **selector))
        self.assertEqual(len(entry.reservations), 0)
        recovery.forget(
            "supervisor", reservation_token=fifth, **selector)
        self.assertEqual(recovery.owner_for_operation(
            "supervisor", operation_id), "operator")

        # 猜测token和不同owner都不能覆盖或删除已经绑定的记录。
        recovery.forget(
            "supervisor", reservation_token="0" * 32, **selector)
        conflict_token, conflict = recovery.reserve(
            "supervisor", "other-owner", **selector)
        self.assertIsNone(conflict_token)
        self.assertEqual(conflict, "conflict")
        self.assertEqual(recovery.owner_for_operation(
            "supervisor", operation_id), "operator")

        # 同一个key只占一个容量槽；其他key在24小时内仍被容量上限拒绝。
        other_token, capacity = recovery.reserve(
            "supervisor", "operator", kind="control_release",
            lease_id="3" * 32, idempotency_key="release:v1")
        self.assertIsNone(other_token)
        self.assertEqual(capacity, "capacity")
        now_ns[0] += 24 * 60 * 60 * 1_000_000_000 + 1
        self.assertIsNone(recovery.owner_for_operation(
            "supervisor", operation_id))

    def test_permission_scope_restart_and_unavailable_fail_closed(self):
        code, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", "d" * 32,
            self._lease_body()))
        self.assertEqual(code, 403)
        self.assertEqual(payload["error"]["code"], "permission_denied")
        self.assertEqual(self.provider.acquires, [])

        lease_id = self._acquire()
        wrong_scope = {
            "lease_id": lease_id, "node_id": "mock-node-1",
            "resource_id": "gpio-other", "idempotency_key": "write-2",
            "value": False,
        }
        code, payload = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "a" * 32,
            wrong_scope))
        self.assertEqual(code, 409)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_conflict")
        self.assertEqual(self.provider.writes, [])

        code, payload = self._error(self._request(
            "DELETE", f"/api/v1/control-leases/{'f' * 32}",
            "a" * 32))
        self.assertEqual(code, 404)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_not_found")
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

        self.identity = "2" * 32
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertFalse(root["data"]["capabilities"]["write_commands"])
        self.assertEqual(root["data"]["capabilities"]["gpio_write"], {
            "configured": True, "operational": False})
        valid = dict(wrong_scope, resource_id="gpio-0")
        code, payload = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "a" * 32, valid))
        self.assertEqual(code, 404)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_not_found")
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertFalse(root["data"]["capabilities"]["write_commands"])

        unavailable = make_server(
            "127.0.0.1", 0, MockSnapshotProvider(),
            authenticator=_authenticator())
        thread = threading.Thread(target=unavailable.serve_forever,
                                  daemon=True)
        thread.start()
        try:
            base = f"http://127.0.0.1:{unavailable.server_port}"
            root = json.loads(urlopen(Request(
                base + "/api/v1", headers={"X-API-Key": "a" * 32})).read())
            self.assertFalse(root["data"]["capabilities"]["write_commands"])
            self.assertEqual(root["data"]["capabilities"]["gpio_write"], {
                "configured": False, "operational": False})
            request = Request(
                base + "/api/v1/control/gpio/write",
                data=json.dumps(valid).encode("utf-8"),
                headers={"X-API-Key": "a" * 32,
                         "Content-Type": "application/json"}, method="POST")
            with self.assertRaises(HTTPError) as caught:
                urlopen(request)
            self.assertEqual(caught.exception.code, 503)
        finally:
            unavailable.shutdown()
            unavailable.server_close()
            thread.join(timeout=2)

    def test_unknown_and_expired_write_keep_same_daemon_operational(self):
        lease_id = self._acquire()
        template = {
            "node_id": "mock-node-1", "resource_id": "gpio-0",
            "idempotency_key": "write-client-error", "value": True,
        }

        unknown = dict(template, lease_id="f" * 32)
        code, payload = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "a" * 32, unknown))
        self.assertEqual(code, 404)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_not_found")
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

        self.now_ns[0] += 1_001_000_000
        expired = dict(template, lease_id=lease_id,
                       idempotency_key="write-expired")
        code, payload = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "a" * 32, expired))
        self.assertEqual(code, 404)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_not_found")
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

    def test_valid_lease_downstream_write_and_release_failures_revoke_proof(
            self):
        lease_id = self._acquire()
        write = {
            "lease_id": lease_id, "node_id": "mock-node-1",
            "resource_id": "gpio-0",
            "idempotency_key": "write-backend-failure", "value": True,
        }
        self.provider.fail_writes = True
        code, payload = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "a" * 32, write))
        self.assertEqual(code, 409)
        self.assertEqual(payload["error"]["code"],
                         "control_operation_expired_unknown")
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

        # 重新幂等 acquire 必须再次抵达下游，才能建立新证明。
        self.provider.fail_writes = False
        replayed = self._lease_body()
        with urlopen(self._request(
                "POST", "/api/v1/control-leases", "a" * 32,
                replayed)) as response:
            self.assertEqual(response.status, 200)
        self.assertEqual(len(self.provider.acquires), 2)
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

        self.provider.fail_releases = True
        code, payload = self._error(self._request(
            "DELETE", f"/api/v1/control-leases/{lease_id}", "a" * 32))
        self.assertEqual(code, 409)
        self.assertEqual(payload["error"]["code"],
                         "control_operation_expired_unknown")
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

    def test_unreadable_daemon_identity_get_fails_closed_and_revokes_lease(
            self):
        lease_id = self._acquire()
        self.identity = None
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertFalse(root["data"]["capabilities"]["write_commands"])
        self.assertEqual(root["data"]["capabilities"]["gpio_write"], {
            "configured": True, "operational": False})

        # 身份恢复也不能复活旧租约或旧 operational 证明；必须重新准入。
        self.identity = "1" * 32
        write = {
            "lease_id": lease_id, "node_id": "mock-node-1",
            "resource_id": "gpio-0", "idempotency_key": "old-write",
            "value": True,
        }
        code, payload = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "a" * 32, write))
        self.assertEqual(code, 404)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_not_found")
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertFalse(root["data"]["capabilities"]["write_commands"])

    def test_failed_second_acquire_rolls_back_only_itself_and_redacts_detail(self):
        first_lease = self._acquire()
        self.provider.fail_resource = "gpio-other"
        second = dict(self._lease_body(), resource_id="gpio-other",
                      idempotency_key="lease-request-2")
        code, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", "a" * 32, second))
        self.assertEqual(code, 409)
        self.assertEqual(payload["error"]["code"],
                         "control_target_rejected")
        self.assertEqual(payload["error"]["details"], {
            "category": "target", "retryable": False,
            "possibly_committed": False,
        })
        self.assertNotIn("secret", json.dumps(payload))
        self.assertEqual(
            self.server.control_leases.active_count(), 1)  # type: ignore[attr-defined]
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertEqual(root["data"]["capabilities"]["gpio_write"], {
            "configured": True, "operational": True})

        write = {
            "lease_id": first_lease, "node_id": "mock-node-1",
            "resource_id": "gpio-0", "idempotency_key": "write-still-ok",
            "value": True,
        }
        with urlopen(self._request(
                "POST", "/api/v1/control/gpio/write", "a" * 32,
                write)) as response:
            self.assertEqual(response.status, 200)

    def test_snapshot_preflight_failure_rolls_back_before_remote_acquire(self):
        class CountingClient:
            structured_output = True

            def __init__(self):
                self.acquire_calls = 0

            def runtime_control_acquire(self, *_arguments, **_keywords):
                self.acquire_calls += 1

            def runtime_gpio_write(self, *_arguments, **_keywords):
                raise AssertionError("本用例不应执行GPIO写")

            def runtime_control_release(self, *_arguments, **_keywords):
                raise AssertionError("本用例不应执行租约释放")

            def runtime_gpio_write_operation(self, *_arguments, **_keywords):
                raise AssertionError("本用例不应执行GPIO写")

            def runtime_control_release_operation(
                    self, *_arguments, **_keywords):
                raise AssertionError("本用例不应执行租约释放")

            def runtime_operation_status(self, *_arguments, **_keywords):
                raise AssertionError("快照失败后不应探测账本")

            def runtime_operation_lookup(self, *_arguments, **_keywords):
                raise AssertionError("本用例不应定位操作")

        class FailingSnapshotProvider(ToolbusdSnapshotProvider):
            def read_snapshot(self, *, deadline=None):
                raise RuntimeProviderError("快照后端暂时不可用")

        client = CountingClient()
        self.server.provider = FailingSnapshotProvider(client)
        code, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", "a" * 32,
            self._lease_body()))
        self.assertEqual(code, 503)
        self.assertEqual(payload["error"]["code"],
                         "control_backend_unavailable")
        self.assertEqual(payload["error"]["details"], {
            "category": "transport", "retryable": True,
            "possibly_committed": False,
        })
        self.assertEqual(client.acquire_calls, 0)
        self.assertEqual(
            self.server.control_leases.active_count(), 0)  # type: ignore[attr-defined]

    def test_operation_query_and_lookup_survive_local_lease_ttl(self):
        lease_id = self._acquire()
        body = {
            "lease_id": lease_id, "node_id": "mock-node-1",
            "resource_id": "gpio-0", "idempotency_key": "durable-write",
            "value": True,
        }
        with urlopen(self._request(
                "POST", "/api/v1/control/gpio/write", "a" * 32,
                body)) as response:
            operation = json.loads(response.read())["data"]["operation"]
        self.now_ns[0] += 1_001_000_000

        with urlopen(self._request(
                "GET", "/api/v1/control/operations/" +
                operation["operation_id"], "a" * 32)) as response:
            queried = json.loads(response.read())["data"]["operation"]
        self.assertEqual(queried["state"], "committed")
        self.assertEqual(queried["lease_id"], lease_id)
        self.assertEqual(queried["error"], None)
        self.assertEqual(set(queried), {
            "operation_schema_version", "operation_id", "lease_id", "scope",
            "operation_kind", "state", "recovery", "scope_blocked",
            "safe_to_retry", "replayed", "result", "error",
        })

        lookup = {
            "operation_kind": "gpio_write", "lease_id": lease_id,
            "idempotency_key": "durable-write",
        }
        with urlopen(self._request(
                "POST", "/api/v1/control/operation-lookups", "a" * 32,
                lookup)) as response:
            located = json.loads(response.read())["data"]["operation"]
        self.assertEqual(located["operation_id"], operation["operation_id"])

        code, _ = self._error(self._request(
            "GET", "/api/v1/control/operations/" + "A" * 64,
            "a" * 32))
        self.assertEqual(code, 400)

    def test_unknown_scope_is_local_only_and_safe_recovery_unblocks(self):
        operation_id = "5" * 64
        unknown = {
            "operation_id": operation_id, "lease_id": "6" * 32,
            "expected_node_uuid": "a" * 32,
            "resource_id": 0x01000001, "kind": "gpio_write",
            "state": "unknown", "replayed": True,
            "recovery": "scope_blocked", "object_id": None,
            "value": None, "error_code": "backend",
        }
        self.provider._operations[operation_id] = unknown
        code, payload = self._error(self._request(
            "GET", "/api/v1/control/operations/" + operation_id,
            "a" * 32))
        self.assertEqual(code, 409)
        operation = payload["error"]["details"]["operation"]
        self.assertEqual(operation["error"], {
            "code": "backend", "category": "backend",
            "retryable": False, "possibly_committed": True,
        })
        self.assertTrue(operation["scope_blocked"])

        blocked = dict(
            self._lease_body(), node_id="node-" + "a" * 32,
            resource_id="resource-01000001", idempotency_key="blocked-lease")
        code, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", "a" * 32, blocked))
        self.assertEqual(code, 409)
        self.assertEqual(payload["error"]["code"], "control_scope_blocked")

        self.provider._operations[operation_id] = {
            **unknown, "recovery": "safe_closed"}
        code, payload = self._error(self._request(
            "GET", "/api/v1/control/operations/" + operation_id,
            "a" * 32))
        self.assertEqual(code, 409)
        self.assertFalse(payload["error"]["details"]["operation"][
            "scope_blocked"])
        with urlopen(self._request(
                "POST", "/api/v1/control-leases", "a" * 32,
                blocked)) as response:
            self.assertEqual(response.status, 201)

    def test_uncertain_write_auto_lookup_returns_pending(self):
        lease_id = self._acquire()
        idempotency_key = "recover-write"
        pending = {
            "operation_id": "7" * 64, "lease_id": lease_id,
            "expected_node_uuid": "a" * 32,
            "resource_id": 0x01000001, "kind": "gpio_write",
            "state": "pending", "replayed": True, "recovery": "none",
            "object_id": None, "value": None, "error_code": None,
        }
        self.provider._operations[(
            "gpio_write", lease_id, idempotency_key)] = pending
        self.provider.fail_writes = True
        body = {
            "lease_id": lease_id, "node_id": "mock-node-1",
            "resource_id": "gpio-0", "idempotency_key": idempotency_key,
            "value": True,
        }
        with urlopen(self._request(
                "POST", "/api/v1/control/gpio/write", "a" * 32,
                body)) as response:
            self.assertEqual(response.status, 202)
            operation = json.loads(response.read())["data"]["operation"]
        self.assertEqual(operation["state"], "pending")
        self.assertEqual(self.provider.lookup_calls,
                         [("gpio_write", lease_id, idempotency_key)])
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

    def test_operation_query_retries_once_after_daemon_change(self):
        operation_id = "8" * 64
        self.provider._operations[operation_id] = {
            "operation_id": operation_id, "lease_id": "9" * 32,
            "expected_node_uuid": "a" * 32,
            "resource_id": 0x01000001, "kind": "gpio_write",
            "state": "committed", "replayed": True, "recovery": "none",
            "object_id": 4, "value": False, "error_code": None,
        }
        changed = [False]

        def change_once():
            if not changed[0]:
                changed[0] = True
                self.identity = "2" * 32

        self.provider.after_status = change_once
        with urlopen(self._request(
                "GET", "/api/v1/control/operations/" + operation_id,
                "a" * 32)) as response:
            self.assertEqual(response.status, 200)
        self.assertEqual(self.provider.status_daemons[-2:],
                         ["1" * 32, "2" * 32])

    def test_definite_local_write_and_release_errors_keep_global_operational(self):
        lease_id = self._acquire()

        def reject_write(*_arguments):
            raise RuntimeProviderOperationError(
                "target_rejected", category="target", retryable=False,
                possibly_committed=False)

        self.provider.gpio_control_write = reject_write
        write = {
            "lease_id": lease_id, "node_id": "mock-node-1",
            "resource_id": "gpio-0", "idempotency_key": "local-reject",
            "value": True,
        }
        status, _ = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "a" * 32, write))
        self.assertEqual(status, 409)
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

        def reject_release(*_arguments):
            raise RuntimeProviderOperationError(
                "target_rejected", category="target", retryable=False,
                possibly_committed=False)

        self.provider.gpio_control_release = reject_release
        status, _ = self._error(self._request(
            "DELETE", f"/api/v1/control-leases/{lease_id}", "a" * 32))
        self.assertEqual(status, 409)
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

    def test_identity_failure_cannot_be_undone_by_old_inflight_write(self):
        lease_id = self._acquire()
        write_started = threading.Event()
        release_write = threading.Event()

        def block_write(_arguments):
            write_started.set()
            release_write.wait(timeout=3)

        self.provider.write_hook = block_write
        write = {
            "lease_id": lease_id, "node_id": "mock-node-1",
            "resource_id": "gpio-0", "idempotency_key": "inflight-write",
            "value": True,
        }
        outcomes = []

        def request_write():
            try:
                with urlopen(self._request(
                        "POST", "/api/v1/control/gpio/write", "a" * 32,
                        write)) as response:
                    outcomes.append(response.status)
            except Exception as error:
                outcomes.append(error)

        worker = threading.Thread(target=request_write)
        worker.start()
        self.assertTrue(write_started.wait(timeout=2))
        revision_before = self.server.gpio_control_state_revision  # type: ignore[attr-defined]
        self.identity = None
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertFalse(root["data"]["capabilities"]["write_commands"])
        cleared_revision = self.server.gpio_control_state_revision  # type: ignore[attr-defined]
        self.assertGreater(cleared_revision, revision_before)

        self.identity = "1" * 32
        release_write.set()
        worker.join(timeout=3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(outcomes, [200])
        self.assertEqual(
            self.server.gpio_control_state_revision,  # type: ignore[attr-defined]
            cleared_revision)
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertFalse(root["data"]["capabilities"]["write_commands"])

    def test_old_inflight_acquire_cannot_overwrite_newer_revision(self):
        first_lease = self._acquire()
        acquire_started = threading.Event()
        release_acquire = threading.Event()

        def block_old_acquire(arguments):
            if arguments[4] == "gpio-old":
                acquire_started.set()
                release_acquire.wait(timeout=3)

        self.provider.acquire_hook = block_old_acquire
        old_body = dict(
            self._lease_body(), resource_id="gpio-old",
            idempotency_key="old-acquire")
        outcomes = []

        def request_old_acquire():
            try:
                with urlopen(self._request(
                        "POST", "/api/v1/control-leases", "a" * 32,
                        old_body)) as response:
                    outcomes.append(response.status)
            except Exception as error:
                outcomes.append(error)

        worker = threading.Thread(target=request_old_acquire)
        worker.start()
        self.assertTrue(acquire_started.wait(timeout=2))

        self.provider.fail_writes = True
        failed_write = {
            "lease_id": first_lease, "node_id": "mock-node-1",
            "resource_id": "gpio-0", "idempotency_key": "force-clear",
            "value": True,
        }
        code, _ = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "a" * 32,
            failed_write))
        self.assertEqual(code, 409)
        self.provider.fail_writes = False

        new_body = dict(
            self._lease_body(), resource_id="gpio-new",
            idempotency_key="new-acquire")
        with urlopen(self._request(
                "POST", "/api/v1/control-leases", "a" * 32,
                new_body)) as response:
            self.assertEqual(response.status, 201)
        new_revision = self.server.gpio_control_state_revision  # type: ignore[attr-defined]

        release_acquire.set()
        worker.join(timeout=3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(outcomes, [201])
        self.assertEqual(
            self.server.gpio_control_state_revision,  # type: ignore[attr-defined]
            new_revision)
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertTrue(root["data"]["capabilities"]["write_commands"])

    def test_expired_after_downstream_registration_is_released_best_effort(self):
        now = [1_000_000_000]
        inner = ControlLeaseManager(
            4, monotonic_ns=lambda: now[0], wall_time_ms=lambda: 1000,
            lease_id_factory=lambda: "e" * 32)
        manager = DaemonBoundControlLeaseManager(
            lambda: "1" * 32, manager=inner)
        provider = FakeGpioProvider()
        provider.after_acquire = lambda: now.__setitem__(
            0, now[0] + 101_000_000)
        server = make_server(
            "127.0.0.1", 0, provider, authenticator=_authenticator(),
            control_lease_manager=manager,
            control_audit_journal=FakeControlAuditJournal())
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            body = dict(self._lease_body(), ttl_ms=100)
            request = Request(
                f"http://127.0.0.1:{server.server_port}"
                "/api/v1/control-leases",
                data=json.dumps(body).encode("utf-8"),
                headers={"X-API-Key": "a" * 32,
                         "Content-Type": "application/json"}, method="POST")
            with self.assertRaises(HTTPError) as caught:
                urlopen(request)
            payload = json.loads(caught.exception.read())
            self.assertEqual(caught.exception.code, 503)
            self.assertNotIn("secret", json.dumps(payload))
            self.assertEqual(provider.releases,
                             [("1" * 32, "e" * 32, "operator")])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()

import json
import subprocess
import threading
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from runtime_api.auth import (
    CONTROL_LEASE_ACQUIRE_PERMISSION,
    CONTROL_LEASE_RELEASE_PERMISSION,
    CONTROL_LEASE_REVOKE_PERMISSION,
    GPIO_WRITE_PERMISSION,
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
from runtime_api.server import make_server
from runtime_api.toolbusd_provider import (
    RemoteCliIpcClient,
    ToolbusIpcOperationError,
    ToolbusIpcProtocolError,
    ToolbusdSnapshotProvider,
)


class FakeGpioProvider(MockSnapshotProvider):
    gpio_control_available = True

    def __init__(self):
        super().__init__()
        self.acquires = []
        self.writes = []
        self.releases = []
        self._completed = {}
        self.fail_resource = None
        self.fail_writes = False
        self.fail_releases = False
        self.after_acquire = None
        self.acquire_hook = None
        self.write_hook = None

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
        return {"object_id": 73, "value": value, "replayed": replayed}

    def gpio_control_release(self, *arguments):
        if self.fail_releases:
            raise RuntimeProviderError("toolbusd释放路径不可用")
        self.releases.append(arguments)


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


def _authenticator():
    return ApiKeyAuthenticator([
        ApiKeyCredential(
            "operator", "a" * 32,
            frozenset({RUNTIME_READ_PERMISSION,
                       CONTROL_LEASE_ACQUIRE_PERMISSION,
                       CONTROL_LEASE_RELEASE_PERMISSION,
                       GPIO_WRITE_PERMISSION})),
        ApiKeyCredential(
            "other", "b" * 32,
            frozenset({CONTROL_LEASE_ACQUIRE_PERMISSION,
                       CONTROL_LEASE_RELEASE_PERMISSION,
                       GPIO_WRITE_PERMISSION})),
        ApiKeyCredential(
            "supervisor", "c" * 32,
            frozenset({CONTROL_LEASE_REVOKE_PERMISSION})),
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
            authenticator=_authenticator(), control_lease_manager=manager)
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
            self.assertEqual(payload["data"]["replayed"], expected_replay)
            self.assertEqual(payload["data"]["object_id"], 73)

        code, payload = self._error(self._request(
            "POST", "/api/v1/control/gpio/write", "b" * 32, body))
        self.assertEqual(code, 403)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_not_owner")

        with urlopen(self._request(
                "DELETE", f"/api/v1/control-leases/{lease_id}",
                "c" * 32)) as response:
            self.assertEqual(response.status, 204)
        self.assertEqual(self.provider.releases,
                         [("1" * 32, lease_id, "operator")])
        records = self.server.audit_sink.snapshot()  # type: ignore[attr-defined]
        self.assertIn("gpio_write_completed",
                      {record.result for record in records})
        self.assertEqual(
            [record.path_category for record in records
             if record.result == "gpio_write_completed"], ["gpio_control"])

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
        self.assertEqual(code, 503)
        self.assertEqual(payload["error"]["code"],
                         "gpio_control_unavailable")
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertFalse(root["data"]["capabilities"]["write_commands"])

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
        self.assertEqual(code, 503)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_unavailable")
        root = json.loads(urlopen(self._request(
            "GET", "/api/v1", "a" * 32)).read())
        self.assertFalse(root["data"]["capabilities"]["write_commands"])

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
        self.assertEqual(code, 503)
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
            control_lease_manager=manager)
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

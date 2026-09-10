import json
import socket
import threading
import unittest
from http.client import HTTPConnection
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from runtime_api.auth import (
    CONTROL_LEASE_ACQUIRE_PERMISSION,
    CONTROL_LEASE_RELEASE_PERMISSION,
    CONTROL_LEASE_REVOKE_PERMISSION,
    RUNTIME_READ_PERMISSION,
    ApiKeyAuthenticator,
    ApiKeyCredential,
)
from runtime_api.control_leases import (
    ControlLeaseError,
    ControlLeaseCapacityExceeded,
    ControlLeaseConflict,
    ControlLeaseManager,
    ControlLeaseNotFound,
    ControlLeaseOwnershipError,
    DaemonBoundControlLeaseManager,
)
from runtime_api.provider import MockSnapshotProvider
from runtime_api.server import make_server
from runtime_api.tests.control_audit_support import FakeControlAuditJournal


class FakeClock:
    def __init__(self):
        self.monotonic = 1_000_000_000
        self.wall = 2_000_000

    def monotonic_ns(self):
        return self.monotonic

    def wall_time_ms(self):
        return self.wall

    def advance_ms(self, milliseconds):
        self.monotonic += milliseconds * 1_000_000
        self.wall += milliseconds


class ControlLeaseManagerTest(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        identifiers = iter(f"{index:032x}" for index in range(1, 20))
        self.manager = ControlLeaseManager(
            4, monotonic_ns=self.clock.monotonic_ns,
            wall_time_ms=self.clock.wall_time_ms,
            lease_id_factory=lambda: next(identifiers))

    def _acquire(self, owner="operator-a", idempotency="request-1"):
        return self.manager.acquire(
            owner_key_id=owner, node_id="node-1", resource_id="gpio-0",
            command_group="write", ttl_ms=1000,
            idempotency_key=idempotency)

    def test_acquire_replay_conflict_release_and_expiry(self):
        lease, replayed = self._acquire()
        self.assertFalse(replayed)
        self.assertEqual(lease.expires_at_ms, 2_001_000)
        replay, replayed = self._acquire()
        self.assertTrue(replayed)
        self.assertEqual(replay.lease_id, lease.lease_id)
        with self.assertRaises(ControlLeaseConflict):
            self._acquire(owner="operator-b", idempotency="request-2")
        with self.assertRaises(ControlLeaseConflict):
            self.manager.acquire(
                owner_key_id="operator-b", node_id="node-1",
                resource_id="gpio-0", command_group="configure",
                ttl_ms=1000, idempotency_key="request-other-group")
        with self.assertRaises(ControlLeaseConflict):
            self.manager.acquire(
                owner_key_id="operator-a", node_id="node-2",
                resource_id="gpio-0", command_group="write", ttl_ms=1000,
                idempotency_key="request-1")
        with self.assertRaises(ControlLeaseOwnershipError):
            self.manager.release(lease.lease_id,
                                 requester_key_id="operator-b")
        self.assertEqual(self.manager.release(
            lease.lease_id, requester_key_id="operator-b",
            allow_foreign=True).owner_key_id, "operator-a")

        expired, _ = self._acquire(idempotency="request-3")
        self.clock.advance_ms(1000)
        self.assertEqual(self.manager.active_count(), 0)
        with self.assertRaises(ControlLeaseNotFound):
            self.manager.release(expired.lease_id,
                                 requester_key_id="operator-a")
        replacement, _ = self._acquire(
            owner="operator-b", idempotency="request-4")
        self.assertNotEqual(replacement.lease_id, expired.lease_id)

    def test_concurrent_same_scope_has_exactly_one_winner(self):
        barrier = threading.Barrier(8)
        successes = []
        conflicts = []
        lock = threading.Lock()

        def contender(index):
            barrier.wait()
            try:
                lease, _ = self._acquire(
                    owner=f"operator-{index}",
                    idempotency=f"request-{index}")
                with lock:
                    successes.append(lease)
            except ControlLeaseConflict as error:
                with lock:
                    conflicts.append(error)

        threads = [threading.Thread(target=contender, args=(index,))
                   for index in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=2)
        self.assertEqual(len(successes), 1)
        self.assertEqual(len(conflicts), 7)
        self.assertEqual(self.manager.active_count(), 1)


    def test_validation_rejects_bool_ttl_and_unbounded_identifiers(self):
        for changes in (
                {"ttl_ms": True}, {"ttl_ms": 99}, {"ttl_ms": 30001},
                {"node_id": "bad/path"}, {"resource_id": "x" * 129},
                {"idempotency_key": "bad key"}):
            arguments = {
                "owner_key_id": "operator-a", "node_id": "node-1",
                "resource_id": "gpio-0", "command_group": "write",
                "ttl_ms": 1000, "idempotency_key": "request-1",
            }
            arguments.update(changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.manager.acquire(**arguments)

    def test_capacity_is_fail_closed_until_expiry_or_release(self):
        manager = ControlLeaseManager(
            1, monotonic_ns=self.clock.monotonic_ns,
            wall_time_ms=self.clock.wall_time_ms,
            lease_id_factory=lambda: "f" * 32)
        manager.acquire(
            owner_key_id="operator-a", node_id="node-1",
            resource_id="gpio-0", command_group="write", ttl_ms=1000,
            idempotency_key="request-1")
        with self.assertRaises(ControlLeaseCapacityExceeded):
            manager.acquire(
                owner_key_id="operator-b", node_id="node-2",
                resource_id="gpio-0", command_group="write", ttl_ms=1000,
                idempotency_key="request-2")

    def test_finished_idempotency_key_cannot_resurrect_lease(self):
        lease, _ = self._acquire()
        self.manager.release(lease.lease_id, requester_key_id="operator-a")
        with self.assertRaisesRegex(ControlLeaseConflict, "已经结束"):
            self._acquire()

        expired, _ = self._acquire(idempotency="request-expired")
        self.clock.advance_ms(1000)
        with self.assertRaisesRegex(ControlLeaseConflict, "已经结束"):
            self._acquire(idempotency="request-expired")
        self.clock.advance_ms(30_000)
        replacement, replayed = self._acquire(idempotency="request-expired")
        self.assertFalse(replayed)
        self.assertNotEqual(replacement.lease_id, expired.lease_id)

    def test_lock_wait_does_not_return_already_expired_lease(self):
        class AdvancingLock:
            def __init__(self, clock):
                self._lock = threading.Lock()
                self._clock = clock
                self._advance_once = True

            def __enter__(self):
                self._lock.acquire()
                if self._advance_once:
                    self._advance_once = False
                    self._clock.advance_ms(1000)
                return self

            def __exit__(self, *args):
                self._lock.release()

        self.manager._lock = AdvancingLock(self.clock)
        self._acquire()
        self.assertEqual(self.manager.active_count(), 1)


class DaemonBoundControlLeaseManagerTest(unittest.TestCase):
    def setUp(self):
        self.identity = "1" * 32
        identifiers = iter(("a" * 32, "b" * 32, "c" * 32))
        inner = ControlLeaseManager(
            4, lease_id_factory=lambda: next(identifiers))
        self.manager = DaemonBoundControlLeaseManager(
            lambda: self.identity, manager=inner)

    def _acquire(self, idempotency="request-1"):
        return self.manager.acquire(
            owner_key_id="operator-a", node_id="node-1",
            resource_id="gpio-0", command_group="write", ttl_ms=1000,
            idempotency_key=idempotency)

    def test_daemon_restart_atomically_invalidates_old_generation(self):
        old, replayed = self._acquire()
        self.assertFalse(replayed)
        self.assertEqual(self.manager.daemon_instance_id, "1" * 32)

        self.identity = "2" * 32
        with self.assertRaises(ControlLeaseNotFound):
            self.manager.release(
                old.lease_id, requester_key_id="operator-a")
        replacement, replayed = self._acquire()
        self.assertFalse(replayed)
        self.assertNotEqual(replacement.lease_id, old.lease_id)
        self.assertEqual(self.manager.daemon_instance_id, "2" * 32)

    def test_unavailable_or_invalid_identity_fails_closed(self):
        self._acquire()

        def unavailable():
            raise RuntimeError("socket unavailable")

        self.manager._identity_reader = unavailable
        with self.assertRaisesRegex(ControlLeaseError, "无法确认"):
            self.manager.active_count()
        self.assertIsNone(self.manager.daemon_instance_id)

        self.manager._identity_reader = lambda: "0" * 32
        with self.assertRaisesRegex(ControlLeaseError, "无效"):
            self._acquire("request-2")

    def test_concurrent_identity_reads_are_single_flight(self):
        first_started = threading.Event()
        release_first = threading.Event()
        reader_lock = threading.Lock()
        calls = 0

        def reader():
            nonlocal calls
            with reader_lock:
                calls += 1
            first_started.set()
            self.assertTrue(release_first.wait(timeout=2))
            return "1" * 32

        identifiers = iter(("d" * 32, "e" * 32))
        inner = ControlLeaseManager(
            4, lease_id_factory=lambda: next(identifiers))
        manager = DaemonBoundControlLeaseManager(reader, manager=inner)
        outcomes = []

        def request(resource_id, idempotency_key):
            try:
                outcomes.append(manager.acquire(
                    owner_key_id="operator-a", node_id="node-1",
                    resource_id=resource_id, command_group="write",
                    ttl_ms=1000, idempotency_key=idempotency_key))
            except Exception as error:  # 测试线程必须把异常带回主线程。
                outcomes.append(error)

        first = threading.Thread(
            target=request, args=("gpio-0", "request-1"))
        second = threading.Thread(
            target=request, args=("gpio-1", "request-2"))
        first.start()
        self.assertTrue(first_started.wait(timeout=2))
        second.start()
        threading.Event().wait(0.05)
        with reader_lock:
            self.assertEqual(calls, 1)
        release_first.set()
        first.join(timeout=2)
        second.join(timeout=2)
        self.assertFalse(first.is_alive())
        self.assertFalse(second.is_alive())
        self.assertEqual(len(outcomes), 2)
        self.assertTrue(all(isinstance(value, tuple) for value in outcomes))
        self.assertEqual(manager.daemon_instance_id, "1" * 32)
        self.assertEqual(inner.active_count(), 2)

    def test_inflight_identity_result_cannot_revive_explicit_invalidation(self):
        started = threading.Event()
        release = threading.Event()

        def reader():
            started.set()
            self.assertTrue(release.wait(timeout=2))
            return "1" * 32

        manager = DaemonBoundControlLeaseManager(reader, capacity=4)
        outcome = []

        def request():
            try:
                manager.acquire(
                    owner_key_id="operator-a", node_id="node-1",
                    resource_id="gpio-0", command_group="write",
                    ttl_ms=1000, idempotency_key="request-1")
            except Exception as error:  # 测试线程必须把异常带回主线程。
                outcome.append(error)

        worker = threading.Thread(target=request)
        worker.start()
        self.assertTrue(started.wait(timeout=2))
        manager.invalidate_all()
        release.set()
        worker.join(timeout=2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(len(outcome), 1)
        self.assertIsInstance(outcome[0], ControlLeaseError)
        self.assertRegex(str(outcome[0]), "显式作废")
        self.assertIsNone(manager.daemon_instance_id)


def authenticator() -> ApiKeyAuthenticator:
    return ApiKeyAuthenticator([
        ApiKeyCredential(
            "operator-a", "a" * 32,
            frozenset({RUNTIME_READ_PERMISSION,
                       CONTROL_LEASE_ACQUIRE_PERMISSION,
                       CONTROL_LEASE_RELEASE_PERMISSION})),
        ApiKeyCredential(
            "operator-b", "b" * 32,
            frozenset({CONTROL_LEASE_ACQUIRE_PERMISSION,
                       CONTROL_LEASE_RELEASE_PERMISSION})),
        ApiKeyCredential(
            "supervisor", "c" * 32,
            frozenset({CONTROL_LEASE_REVOKE_PERMISSION})),
        ApiKeyCredential("reader", "d" * 32, frozenset()),
    ])


class ControlLeaseHttpTest(unittest.TestCase):
    def setUp(self):
        self.server = make_server(
            "127.0.0.1", 0, MockSnapshotProvider(),
            authenticator=authenticator(), control_lease_capacity=8,
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
        return caught.exception, json.loads(caught.exception.read())

    @staticmethod
    def _body(idempotency="request-1"):
        return {
            "node_id": "mock-node-1", "resource_id": "gpio-0",
            "command_group": "write", "ttl_ms": 1000,
            "idempotency_key": idempotency,
        }

    def test_permission_conflict_idempotency_release_and_audit(self):
        root = json.loads(urlopen(Request(
            self.base + "/api/v1", headers={"X-API-Key": "a" * 32})).read())
        self.assertFalse(root["data"]["capabilities"]["read_only"])
        self.assertTrue(root["data"]["capabilities"][
            "control_leases"]["available"])

        denied, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", "d" * 32, self._body()))
        self.assertEqual(denied.code, 403)
        self.assertEqual(payload["error"]["code"], "permission_denied")

        with urlopen(self._request(
                "POST", "/api/v1/control-leases", "a" * 32,
                self._body())) as response:
            self.assertEqual(response.status, 201)
            first = json.loads(response.read())
        lease_id = first["data"]["lease"]["lease_id"]
        self.assertFalse(first["data"]["replayed"])
        self.assertNotIn("api_key", json.dumps(first))

        with urlopen(self._request(
                "POST", "/api/v1/control-leases", "a" * 32,
                self._body())) as response:
            self.assertEqual(response.status, 200)
            replay = json.loads(response.read())
        self.assertTrue(replay["data"]["replayed"])
        self.assertEqual(replay["data"]["lease"]["lease_id"], lease_id)

        conflict, payload = self._error(self._request(
            "POST", "/api/v1/control-leases", "b" * 32,
            self._body("request-2")))
        self.assertEqual(conflict.code, 409)
        self.assertEqual(payload["error"]["code"], "control_lease_conflict")
        self.assertNotIn("operator-a", json.dumps(payload))

        foreign, payload = self._error(self._request(
            "DELETE", f"/api/v1/control-leases/{lease_id}", "b" * 32))
        self.assertEqual(foreign.code, 403)
        self.assertEqual(payload["error"]["code"],
                         "control_lease_not_owner")
        with urlopen(self._request(
                "DELETE", f"/api/v1/control-leases/{lease_id}",
                "c" * 32)) as response:
            self.assertEqual(response.status, 204)

        records = self.server.audit_sink.snapshot()  # type: ignore[attr-defined]
        self.assertEqual(records[-1].result, "control_lease_released")
        self.assertEqual(records[-1].path_category, "control_leases")
        self.assertEqual(records[-1].key_id, "supervisor")

    def test_disabled_auth_and_malformed_body_fail_closed(self):
        unauthenticated = make_server(
            "127.0.0.1", 0, MockSnapshotProvider())
        thread = threading.Thread(
            target=unauthenticated.serve_forever, daemon=True)
        thread.start()
        try:
            request = Request(
                f"http://127.0.0.1:{unauthenticated.server_port}"
                "/api/v1/control-leases", data=b"{}",
                headers={"Content-Type": "application/json"}, method="POST")
            with self.assertRaises(HTTPError) as caught:
                urlopen(request)
            self.assertEqual(caught.exception.code, 503)
            self.assertEqual(json.loads(caught.exception.read())[
                "error"]["code"], "control_authentication_disabled")
        finally:
            unauthenticated.shutdown()
            unauthenticated.server_close()
            thread.join(timeout=2)

        malformed = self._request(
            "POST", "/api/v1/control-leases", "a" * 32, self._body())
        malformed.data = b'{"ttl_ms":1000,"ttl_ms":1000}'
        malformed.headers["Content-length"] = str(len(malformed.data))
        error, payload = self._error(malformed)
        self.assertEqual(error.code, 400)
        self.assertEqual(payload["error"]["code"], "request_body_invalid")

        wrong_method, payload = self._error(self._request(
            "PUT", "/api/v1/control-leases", "a" * 32, self._body()))
        self.assertEqual(wrong_method.code, 405)
        self.assertEqual(payload["error"]["code"], "read_only")
        self.assertEqual(
            self.server.control_leases.active_count(), 0)  # type: ignore[attr-defined]

    def test_daemon_unavailable_release_maps_to_503(self):
        identity = ["1" * 32]

        def reader():
            if identity[0] is None:
                raise RuntimeError("daemon unavailable")
            return identity[0]

        manager = DaemonBoundControlLeaseManager(reader, capacity=4)
        server = make_server(
            "127.0.0.1", 0, MockSnapshotProvider(),
            authenticator=authenticator(), control_lease_manager=manager,
            control_audit_journal=FakeControlAuditJournal())
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            request = Request(
                base + "/api/v1/control-leases",
                data=json.dumps(self._body()).encode("utf-8"),
                headers={"X-API-Key": "a" * 32,
                         "Content-Type": "application/json"},
                method="POST")
            with urlopen(request) as response:
                lease_id = json.loads(response.read())["data"]["lease"][
                    "lease_id"]
            identity[0] = None
            release = Request(
                base + f"/api/v1/control-leases/{lease_id}",
                headers={"X-API-Key": "a" * 32}, method="DELETE")
            with self.assertRaises(HTTPError) as caught:
                urlopen(release)
            self.assertEqual(caught.exception.code, 503)
            payload = json.loads(caught.exception.read())
            self.assertEqual(payload["error"]["code"],
                             "control_lease_unavailable")
            self.assertIsNone(manager.daemon_instance_id)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_transfer_encoding_is_rejected_even_with_content_length(self):
        body = json.dumps(self._body()).encode("utf-8")
        connection = HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=2)
        try:
            connection.putrequest("POST", "/api/v1/control-leases")
            connection.putheader("X-API-Key", "a" * 32)
            connection.putheader("Content-Type", "application/json")
            connection.putheader("Content-Length", str(len(body)))
            connection.putheader("Transfer-Encoding", "chunked")
            connection.endheaders(body)
            response = connection.getresponse()
            payload = json.loads(response.read())
        finally:
            connection.close()
        self.assertEqual(response.status, 400)
        self.assertEqual(payload["error"]["code"],
                         "transfer_encoding_unsupported")
        self.assertEqual(
            self.server.control_leases.active_count(), 0)  # type: ignore[attr-defined]

    def test_non_loopback_listener_disables_control_even_with_auth(self):
        server = make_server(
            "0.0.0.0", 0, MockSnapshotProvider(),
            authenticator=authenticator())
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            request = Request(
                f"http://127.0.0.1:{server.server_port}"
                "/api/v1/control-leases",
                data=json.dumps(self._body()).encode("utf-8"),
                headers={"X-API-Key": "a" * 32,
                         "Content-Type": "application/json"},
                method="POST")
            with self.assertRaises(HTTPError) as caught:
                urlopen(request)
            self.assertEqual(caught.exception.code, 503)
            payload = json.loads(caught.exception.read())
            self.assertEqual(payload["error"]["code"],
                             "control_transport_insecure")
            self.assertFalse(server.control_leases_available)  # type: ignore[attr-defined]
            root = json.loads(urlopen(Request(
                f"http://127.0.0.1:{server.server_port}/api/v1",
                headers={"X-API-Key": "a" * 32})).read())
            self.assertTrue(root["data"]["capabilities"]["read_only"])
            self.assertFalse(root["data"]["capabilities"][
                "control_leases"]["available"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_stalled_control_body_has_total_deadline(self):
        server = make_server(
            "127.0.0.1", 0, MockSnapshotProvider(),
            authenticator=authenticator(), request_io_timeout_seconds=0.1)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        client = socket.create_connection(
            ("127.0.0.1", server.server_port), timeout=2)
        client.settimeout(2)
        try:
            request = (
                "POST /api/v1/control-leases HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                f"X-API-Key: {'a' * 32}\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 100\r\n"
                "Connection: close\r\n\r\n{").encode("ascii")
            client.sendall(request)
            response = client.recv(4096)
        finally:
            client.close()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
        self.assertIn(b" 408 ", response.split(b"\r\n", 1)[0])

    def test_slow_drip_header_has_total_deadline(self):
        server = make_server(
            "127.0.0.1", 0, MockSnapshotProvider(),
            authenticator=authenticator(), request_io_timeout_seconds=0.1)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        client = socket.create_connection(
            ("127.0.0.1", server.server_port), timeout=2)
        client.settimeout(1)
        try:
            for chunk in (
                    b"POST /api/v1/control-leases HTTP/1.1\r\n",
                    b"Host: 127.0.0.1\r\n",
                    b"X-API-Key: "):
                try:
                    client.sendall(chunk)
                except OSError:
                    break
                threading.Event().wait(0.045)
            try:
                response = client.recv(4096)
            except (ConnectionError, socket.timeout):
                response = b""
        finally:
            client.close()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
        self.assertEqual(response, b"")

    def test_deeply_nested_json_is_rejected_without_handler_crash(self):
        nested = b"[" * 1100 + b"]" * 1100
        request = Request(
            self.base + "/api/v1/control-leases", data=nested,
            headers={"X-API-Key": "a" * 32,
                     "Content-Type": "application/json"}, method="POST")
        error, payload = self._error(request)
        self.assertEqual(error.code, 400)
        self.assertEqual(payload["error"]["code"], "request_body_invalid")


if __name__ == "__main__":
    unittest.main()

import copy
import json
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from runtime_api.auth import (
    RUNTIME_READ_PERMISSION,
    ApiKeyAuthenticator,
    ApiKeyCredential,
)
from runtime_api.events import (
    EventLogError,
    ExpiredEventCursor,
    InvalidEventCursor,
    RuntimeEventLog,
)
from runtime_api.provider import RuntimeProvider, RuntimeProviderError, mock_snapshot
from runtime_api.server import make_server


EVENT_INCARNATION = "0123456789abcdef0123456789abcdef"
OTHER_INCARNATION = "fedcba9876543210fedcba9876543210"


def _cursor(sequence: int,
            incarnation: str = EVENT_INCARNATION) -> str:
    return f"e1:{incarnation}:{sequence}"


def _authenticator(api_key: str) -> ApiKeyAuthenticator:
    return ApiKeyAuthenticator([ApiKeyCredential(
        "event-reader", api_key, frozenset({RUNTIME_READ_PERMISSION}))])


class RuntimeEventLogTest(unittest.TestCase):
    def test_diff_order_dedup_and_clock_sample_age_filter(self):
        snapshot = mock_snapshot()
        snapshot["nodes"][0]["runtime"]["clock_sync"] = {
            "state": "synced", "model_generation": 1, "sample_age_ms": 10,
        }
        log = RuntimeEventLog(
            capacity=16, incarnation=EVENT_INCARNATION)
        self.assertTrue(log.observe(snapshot))
        first = log.read_page(_cursor(0), 16)
        self.assertEqual(
            [(event.entity_type, event.change) for event in first.events],
            [("node", "added"), ("resource", "added"),
             ("resource", "added"), ("clock_quality", "added")])

        self.assertTrue(log.observe(copy.deepcopy(snapshot)))
        self.assertEqual(log.read_page(first.next_cursor, 16).events, ())

        changed = copy.deepcopy(snapshot)
        changed["snapshot_id"] = "mock-2"
        changed["captured_at_ms"] = 20
        changed["nodes"][0]["runtime"]["clock_sync"]["sample_age_ms"] = 30
        self.assertTrue(log.observe(changed))
        self.assertEqual(log.read_page(first.next_cursor, 16).events, ())

        changed["snapshot_id"] = "mock-3"
        changed["captured_at_ms"] = 30
        changed["nodes"][0]["resources"][0]["available"] = False
        changed["nodes"][0]["runtime"]["clock_sync"]["model_generation"] = 2
        self.assertTrue(log.observe(changed))
        page = log.read_page(first.next_cursor, 16)
        self.assertEqual([event.entity_type for event in page.events],
                         ["resource", "clock_quality"])
        self.assertEqual([event.change for event in page.events],
                         ["updated", "updated"])

    def test_cursor_expiry_capacity_and_malicious_values(self):
        with self.assertRaises(ValueError):
            RuntimeEventLog(capacity=2, incarnation="0" * 31)
        with self.assertRaises(ValueError):
            RuntimeEventLog(capacity=2, incarnation="A" * 32)
        log = RuntimeEventLog(
            capacity=2, incarnation=EVENT_INCARNATION)
        self.assertTrue(log.observe(mock_snapshot()))
        with self.assertRaises(ExpiredEventCursor) as caught:
            log.read_page(_cursor(0), 2)
        self.assertEqual(caught.exception.reset_cursor, _cursor(3))
        baseline = log.read_page(None, 2)
        self.assertEqual(baseline.events, ())
        self.assertEqual(baseline.next_cursor, _cursor(3))
        for cursor in (
                "", f"e2:{EVENT_INCARNATION}:1", "e1:short:1",
                f"e1:{EVENT_INCARNATION}:-1",
                f"e1:{EVENT_INCARNATION}:01",
                f"e1:{EVENT_INCARNATION}:abc",
                f"e1:{EVENT_INCARNATION}:9999999999999999999",
                "x" * 4096):
            with self.subTest(cursor=cursor), \
                    self.assertRaises(InvalidEventCursor):
                log.read_page(cursor, 1)
        with self.assertRaises(ExpiredEventCursor) as mismatch:
            log.read_page(_cursor(3, OTHER_INCARNATION), 1)
        self.assertEqual(mismatch.exception.reset_cursor, _cursor(3))
        with self.assertRaises(ValueError):
            log.read_page(None, 101)

    def test_process_restart_incarnation_expires_old_cursor(self):
        before_restart = RuntimeEventLog(
            capacity=16, incarnation=EVENT_INCARNATION)
        self.assertTrue(before_restart.observe(mock_snapshot()))
        old_cursor = before_restart.latest_cursor

        after_restart = RuntimeEventLog(
            capacity=16, incarnation=OTHER_INCARNATION)
        changed = mock_snapshot()
        changed["nodes"][0]["resources"][0]["available"] = False
        self.assertTrue(after_restart.observe(changed))
        with self.assertRaises(ExpiredEventCursor) as caught:
            after_restart.read_page(old_cursor, 16)
        self.assertEqual(caught.exception.reset_cursor,
                         after_restart.latest_cursor)

    def test_concurrent_same_snapshot_is_deduplicated(self):
        log = RuntimeEventLog(
            capacity=16, incarnation=EVENT_INCARNATION)
        snapshot = mock_snapshot()
        threads = [threading.Thread(target=log.observe, args=(snapshot,))
                   for _ in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())
        self.assertEqual(len(log.read_page(_cursor(0), 16).events), 3)

    def test_same_capture_time_conflict_fails_closed_and_recovers(self):
        log = RuntimeEventLog(
            capacity=16, incarnation=EVENT_INCARNATION)
        baseline = mock_snapshot()
        baseline["captured_at_ms"] = 10
        self.assertTrue(log.observe(baseline))

        different_id = copy.deepcopy(baseline)
        different_id["snapshot_id"] = "mock-2"
        self.assertFalse(log.observe(different_id))
        with self.assertRaises(EventLogError):
            log.read_page(log.latest_cursor, 1)

        self.assertFalse(log.observe(copy.deepcopy(baseline)))
        recovery = copy.deepcopy(baseline)
        recovery["snapshot_id"] = "mock-3"
        recovery["captured_at_ms"] = 11
        self.assertTrue(log.observe(recovery))

        same_id_changed = copy.deepcopy(recovery)
        same_id_changed["nodes"][0]["resources"][0]["available"] = False
        self.assertFalse(log.observe(same_id_changed))
        with self.assertRaises(EventLogError):
            log.read_page(log.latest_cursor, 1)

        recovered = copy.deepcopy(same_id_changed)
        recovered["snapshot_id"] = "mock-4"
        recovered["captured_at_ms"] = 12
        self.assertTrue(log.observe(recovered))
        page = log.read_page(_cursor(3), 16)
        self.assertEqual([(event.entity_type, event.change)
                          for event in page.events],
                         [("resource", "updated")])

    def test_late_stale_failure_does_not_poison_newer_state(self):
        import runtime_api.events as events_module

        log = RuntimeEventLog(
            capacity=16, incarnation=EVENT_INCARNATION)
        baseline = mock_snapshot()
        baseline["captured_at_ms"] = 100
        self.assertTrue(log.observe(baseline))
        old_cursor = log.latest_cursor

        stale = copy.deepcopy(baseline)
        stale["snapshot_id"] = "stale-computing"
        stale["captured_at_ms"] = 101
        newer = copy.deepcopy(baseline)
        newer["snapshot_id"] = "newer"
        newer["captured_at_ms"] = 102
        newer["nodes"][0]["resources"][0]["available"] = False
        entered = threading.Event()
        release = threading.Event()
        result: list[bool] = []
        original_snapshot_states = events_module._snapshot_states

        def controlled_snapshot_states(snapshot):
            if snapshot["snapshot_id"] == "stale-computing":
                entered.set()
                self.assertTrue(release.wait(timeout=2))
                raise EventLogError("过期计算失败")
            return original_snapshot_states(snapshot)

        with patch("runtime_api.events._snapshot_states",
                   side_effect=controlled_snapshot_states):
            thread = threading.Thread(
                target=lambda: result.append(log.observe(stale)))
            thread.start()
            self.assertTrue(entered.wait(timeout=2))
            self.assertTrue(log.observe(newer))
            release.set()
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())

        self.assertEqual(result, [True])
        page = log.read_page(old_cursor, 16)
        self.assertEqual([(event.entity_type, event.change)
                          for event in page.events],
                         [("resource", "updated")])

    def test_large_payload_is_omitted_and_state_limit_is_explicit(self):
        snapshot = mock_snapshot()
        snapshot["nodes"][0]["resources"][0]["state"] = {
            "value": "x" * 5000,
        }
        log = RuntimeEventLog(
            capacity=8, incarnation=EVENT_INCARNATION)
        self.assertTrue(log.observe(snapshot))
        resource = next(event for event in log.read_page(_cursor(0), 8).events
                        if event.resource_id == "gpio-0")
        self.assertTrue(resource.payload_omitted)
        self.assertIsNone(resource.payload)

        oversized = mock_snapshot()
        oversized["nodes"][0]["resources"][0]["state"] = {
            "value": "x" * (2 * 1024 * 1024),
        }
        self.assertFalse(log.observe(oversized))
        with self.assertRaises(EventLogError):
            log.read_page(None, 1)


class MutableSnapshotProvider(RuntimeProvider):
    def __init__(self):
        self.snapshot = mock_snapshot()
        self.fail = False
        self.calls = 0

    def get_snapshot(self) -> dict:
        self.calls += 1
        if self.fail:
            raise RuntimeProviderError("测试缓存刷新失败")
        return copy.deepcopy(self.snapshot)


class RuntimeEventsHttpTest(unittest.TestCase):
    API_KEY = "event-test-key-0123456789-abcdefg"

    def setUp(self):
        self.provider = MutableSnapshotProvider()
        self.server = make_server(
            "127.0.0.1", 0, self.provider,
            authenticator=_authenticator(self.API_KEY), event_capacity=8,
            event_incarnation=EVENT_INCARNATION)
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"
        self.headers = {"X-API-Key": self.API_KEY}

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def _read(self, path: str) -> tuple[object, object]:
        response = urlopen(Request(self.base + path, headers=self.headers))
        return response, json.loads(response.read())

    def _error(self, path: str, *, authenticated: bool = True):
        headers = self.headers if authenticated else {}
        with self.assertRaises(HTTPError) as caught:
            urlopen(Request(self.base + path, headers=headers))
        return caught.exception, json.loads(caught.exception.read())

    def test_authenticated_short_poll_paginates_without_duplicates(self):
        error, payload = self._error("/api/v1/events?limit=1",
                                     authenticated=False)
        self.assertEqual(error.code, 401)
        self.assertEqual(payload["error"]["code"], "authentication_required")

        response, first = self._read(
            f"/api/v1/events?cursor={_cursor(0)}&limit=1")
        self.assertEqual(first["data"]["event_schema_version"], 1)
        self.assertEqual(len(first["data"]["events"]), 1)
        self.assertTrue(first["data"]["has_more"])
        cursor = first["data"]["next_cursor"]
        self.assertLess(int(response.headers["Content-Length"]), 16 * 1024)

        _, second = self._read(f"/api/v1/events?cursor={cursor}&limit=2")
        self.assertEqual(len(second["data"]["events"]), 2)
        self.assertFalse(second["data"]["has_more"])
        latest = second["data"]["next_cursor"]
        _, empty = self._read(f"/api/v1/events?cursor={latest}")
        self.assertEqual(empty["data"]["events"], [])
        self.assertEqual(empty["data"]["next_cursor"], latest)

    def test_invalid_cursor_is_rejected_before_provider_read(self):
        calls = self.provider.calls
        error, payload = self._error("/api/v1/events?cursor=../../secret")
        self.assertEqual(error.code, 400)
        self.assertEqual(payload["error"]["code"], "event_cursor_invalid")
        self.assertEqual(self.provider.calls, calls)

        error, payload = self._error(
            "/api/v1/events?limit=" + "9" * 5000)
        self.assertEqual(error.code, 400)
        self.assertEqual(payload["error"]["code"], "event_limit_invalid")

        error, payload = self._error(
            "/api/v1/events?api_key=" + self.API_KEY,
            authenticated=False)
        self.assertEqual(error.code, 400)
        self.assertEqual(payload["error"]["code"], "credential_in_query")

        self.server.authenticator = ApiKeyAuthenticator([  # type: ignore[attr-defined]
            ApiKeyCredential("no-read", self.API_KEY, frozenset()),
        ])
        error, payload = self._error("/api/v1/events")
        self.assertEqual(error.code, 403)
        self.assertEqual(payload["error"]["code"], "permission_denied")
        self.assertEqual(self.provider.calls, calls)

    def test_expired_cursor_and_provider_failure_are_explicit(self):
        self.server.event_log = RuntimeEventLog(  # type: ignore[attr-defined]
            2, incarnation=EVENT_INCARNATION)
        error, payload = self._error(
            f"/api/v1/events?cursor={_cursor(0)}")
        self.assertEqual(error.code, 409)
        self.assertEqual(payload["error"]["code"], "event_cursor_expired")
        reset = payload["error"]["details"]["reset_cursor"]

        self.provider.snapshot["snapshot_id"] = "mock-2"
        self.provider.snapshot["captured_at_ms"] = 10
        self.provider.snapshot["nodes"][0]["resources"][0]["available"] = False
        self.provider.fail = True
        error, payload = self._error(
            f"/api/v1/events?cursor={reset}")
        self.assertEqual(error.code, 503)
        self.assertEqual(payload["error"]["code"], "provider_unavailable")

        self.provider.fail = False
        _, recovered = self._read(f"/api/v1/events?cursor={reset}")
        self.assertEqual(len(recovered["data"]["events"]), 1)
        self.assertEqual(recovered["data"]["events"][0]["entity_type"],
                         "resource")

    def test_old_incarnation_returns_conflict_and_reset_cursor(self):
        calls = self.provider.calls
        self.provider.fail = True
        error, payload = self._error(
            f"/api/v1/events?cursor={_cursor(0, OTHER_INCARNATION)}")
        self.assertEqual(error.code, 409)
        self.assertEqual(payload["error"]["code"], "event_cursor_expired")
        self.assertEqual(payload["error"]["details"]["reset_cursor"],
                         _cursor(0))
        self.assertEqual(self.provider.calls, calls)


if __name__ == "__main__":
    unittest.main()

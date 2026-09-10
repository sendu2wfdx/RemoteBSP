import json
import threading
import time
import unittest

from runtime_api.control_leases import (
    ControlLeaseManager,
    DaemonBoundControlLeaseManager,
)
from runtime_api.deadline import (
    MonotonicDeadline,
    RequestDeadlineExceeded,
    accepts_deadline,
    call_with_deadline,
)
from runtime_api.provider import RuntimeProviderOperationError
from runtime_api.toolbusd_provider import (
    RemoteCliIpcClient,
    ToolbusIpcOperationError,
    ToolbusIpcProtocolError,
    ToolbusdSnapshotProvider,
)
from runtime_api.tests.test_toolbusd_provider import (
    RemoteCliIpcClientTest as _RemoteCliIpcClientTest,
)


class FakeNanosecondClock:
    def __init__(self):
        self.now_ns = 0

    def __call__(self):
        return self.now_ns

    def advance_ms(self, milliseconds):
        self.now_ns += milliseconds * 1_000_000


class DeadlineContractTest(unittest.TestCase):
    def test_integer_budget_is_never_rounded_up(self):
        clock = FakeNanosecondClock()
        deadline = MonotonicDeadline.after_seconds(1, clock_ns=clock)
        clock.now_ns = 999_000_001
        with self.assertRaises(RequestDeadlineExceeded):
            deadline.remaining_milliseconds()

    def test_positional_only_deadline_is_not_treated_as_keyword_compatible(self):
        def legacy(deadline, /):
            return deadline

        clock = FakeNanosecondClock()
        deadline = MonotonicDeadline.after_seconds(1, clock_ns=clock)
        self.assertFalse(accepts_deadline(legacy))
        self.assertEqual(call_with_deadline(
            legacy, "legacy-value", deadline=deadline), "legacy-value")

    def test_legacy_callback_cannot_return_success_after_deadline(self):
        clock = FakeNanosecondClock()
        deadline = MonotonicDeadline.after_seconds(0.1, clock_ns=clock)

        def legacy():
            clock.advance_ms(101)
            return "late-success"

        with self.assertRaises(RequestDeadlineExceeded):
            call_with_deadline(legacy, deadline=deadline)

    def test_structured_cli_error_contract_is_exact_and_typed(self):
        def document(error):
            return json.dumps({
                "schema_version": 1,
                "command": "runtime-gpio-write",
                "error": error,
            })

        valid = {
            "ipc_error_version": 1, "code": 105, "category": 3,
            "retryable": False, "possibly_committed": False,
            "message": "目标合同拒绝",
        }
        error = ToolbusIpcOperationError.from_document(
            document(valid), "runtime-gpio-write")
        self.assertEqual(error.code, 105)
        self.assertFalse(error.possibly_committed)
        invalid_values = []
        for changes in (
                {"category": 5}, {"code": 999}, {"retryable": 0},
                {"code": 103, "category": 4, "retryable": True},
                {"retryable": True, "possibly_committed": True},
                {"code": 103, "category": 4,
                 "possibly_committed": True},
                {"ipc_error_version": 2}, {"message": "bad\x7f"},
                {"message": "x" * 257}):
            invalid_values.append({**valid, **changes})
        invalid_values.append({**valid, "detail": "/secret/path"})
        for invalid in invalid_values:
            with self.subTest(invalid=invalid), self.assertRaises(
                    ToolbusIpcProtocolError):
                ToolbusIpcOperationError.from_document(
                    document(invalid), "runtime-gpio-write")
        retryable = ToolbusIpcOperationError.from_document(document({
            **valid, "code": 102, "category": 4, "retryable": True,
        }), "runtime-gpio-write")
        self.assertTrue(retryable.retryable)

    def test_provider_operation_error_requires_exact_code_category_pair(self):
        with self.assertRaisesRegex(ValueError, "错误码与类别不匹配"):
            RuntimeProviderOperationError(
                "deadline_exceeded", category="target", retryable=False,
                possibly_committed=False)


class UnifiedControlBudgetTest(unittest.TestCase):
    def test_structured_cli_error_survives_provider_mapping(self):
        class FailingClient:
            structured_output = True

            def runtime_control_acquire(self, *arguments):
                raise AssertionError

            def runtime_gpio_write(self, *arguments):
                raise AssertionError

            def runtime_control_release(self, *arguments):
                raise ToolbusIpcOperationError(
                    105, 3, False, False, "目标合同拒绝")

        provider = ToolbusdSnapshotProvider(FailingClient())
        with self.assertRaises(RuntimeProviderOperationError) as caught:
            provider.gpio_control_release(
                "1" * 32, "2" * 32, "operator")
        error = caught.exception
        self.assertEqual(error.code, "target_rejected")
        self.assertEqual(error.category, "target")
        self.assertFalse(error.retryable)
        self.assertFalse(error.possibly_committed)

    def test_identity_target_resolution_and_acquire_share_one_budget(self):
        clock = FakeNanosecondClock()
        timeouts = []
        document = json.loads(
            _RemoteCliIpcClientTest._runtime_snapshot_document())
        resource = document["data"]["resources"][0]
        resource["status_valid"] = True
        resource["status"] = {
            "resource_id": resource["descriptor"]["resource_id"],
            "health": 0, "health_name": "normal", "error_flags": 0,
            "rx_buffered": 0, "tx_buffered": 0,
            "rx_overruns": 0, "tx_overruns": 0,
        }

        def runner(command, timeout_seconds, _maximum_output):
            timeouts.append(timeout_seconds)
            operation = next(item for item in (
                "daemon-identity", "runtime-snapshot",
                "runtime-control-acquire") if item in command)
            clock.advance_ms(100)
            if operation == "daemon-identity":
                data = {"ipc_version": 1, "instance_id": "1" * 32}
            elif operation == "runtime-snapshot":
                return json.dumps(document)
            else:
                data = {}
            return json.dumps({
                "schema_version": 1, "command": operation, "data": data,
            })

        client = RemoteCliIpcClient(
            "/tmp/deadline.sock", timeout_seconds=2, runner=runner)
        provider = ToolbusdSnapshotProvider(
            client, clock_ms=lambda: clock.now_ns // 1_000_000,
            cache_ttl_ms=0)
        manager = DaemonBoundControlLeaseManager(
            client.daemon_identity,
            manager=ControlLeaseManager(
                4, monotonic_ns=clock,
                wall_time_ms=lambda: clock.now_ns // 1_000_000))
        deadline = MonotonicDeadline.after_seconds(1, clock_ns=clock)
        lease, _ = manager.acquire(
            owner_key_id="operator", node_id="node-" + "ab" * 16,
            resource_id="resource-01000001", command_group="gpio.write",
            ttl_ms=1000, idempotency_key="request-1", deadline=deadline)
        provider.gpio_control_acquire(
            "1" * 32, lease.lease_id, "operator",
            "node-" + "ab" * 16, "resource-01000001",
            lambda: manager.remaining_ttl_ms(
                lease.lease_id, requester_key_id="operator",
                deadline=deadline),
            deadline=deadline)
        self.assertEqual(len(timeouts), 4)
        self.assertTrue(all(earlier > later for earlier, later in zip(
            timeouts, timeouts[1:])))
        self.assertLessEqual(timeouts[0], 1.0)

    def test_target_resolution_deadline_is_definite_before_ipc(self):
        clock = FakeNanosecondClock()

        class Client:
            structured_output = True

            def __init__(self):
                self.acquire_calls = 0

            def runtime_control_acquire(self, *_arguments, **_keywords):
                self.acquire_calls += 1

            def runtime_gpio_write(self, *_arguments, **_keywords):
                raise AssertionError("本用例不应执行GPIO写")

            def runtime_control_release(self, *_arguments, **_keywords):
                raise AssertionError("本用例不应执行租约释放")

        class SlowTargetProvider(ToolbusdSnapshotProvider):
            def read_snapshot(self, *, deadline=None):
                clock.advance_ms(101)
                deadline.check()
                raise AssertionError("期限检查后不可继续")

        client = Client()
        provider = SlowTargetProvider(client)
        deadline = MonotonicDeadline.after_seconds(0.1, clock_ns=clock)
        with self.assertRaises(RuntimeProviderOperationError) as caught:
            provider.gpio_control_acquire(
                "1" * 32, "2" * 32, "operator",
                "node-" + "ab" * 16, "resource-01000001",
                lambda: 1000, deadline=deadline)
        error = caught.exception
        self.assertEqual(error.code, "deadline_exceeded")
        self.assertTrue(error.retryable)
        self.assertFalse(error.possibly_committed)
        self.assertEqual(client.acquire_calls, 0)

    def test_exhausted_budget_does_not_start_runner(self):
        clock = FakeNanosecondClock()
        calls = []
        client = RemoteCliIpcClient(
            "/tmp/deadline.sock",
            runner=lambda *arguments: calls.append(arguments))
        deadline = MonotonicDeadline.after_seconds(0.1, clock_ns=clock)
        clock.advance_ms(100)
        with self.assertRaises(RequestDeadlineExceeded):
            client.daemon_identity(deadline=deadline)
        self.assertEqual(calls, [])

    def test_precise_late_daemon_error_is_not_masked_by_deadline(self):
        clock = FakeNanosecondClock()

        def runner(*_arguments):
            clock.advance_ms(101)
            raise ToolbusIpcOperationError(
                105, 3, False, False, "目标合同拒绝")

        client = RemoteCliIpcClient(
            "/tmp/deadline.sock", runner=runner)
        deadline = MonotonicDeadline.after_seconds(0.1, clock_ns=clock)
        with self.assertRaises(ToolbusIpcOperationError) as caught:
            client.runtime_gpio_write(
                "1" * 32, "2" * 32, "3" * 32, "operator", 1,
                0x01000001, "write-1", True, deadline=deadline)
        self.assertEqual(caught.exception.code, 105)
        self.assertFalse(caught.exception.possibly_committed)

    def test_post_runner_deadline_is_uncertain_and_not_retryable(self):
        clock = FakeNanosecondClock()

        def runner(*_arguments):
            clock.advance_ms(101)
            return json.dumps({
                "schema_version": 1, "command": "runtime-gpio-write",
                "data": {"object_id": 7, "value": True,
                         "replayed": False},
            })

        client = RemoteCliIpcClient(
            "/tmp/deadline.sock", runner=runner)
        deadline = MonotonicDeadline.after_seconds(0.1, clock_ns=clock)
        with self.assertRaises(ToolbusIpcOperationError) as caught:
            client.runtime_gpio_write(
                "1" * 32, "2" * 32, "3" * 32, "operator", 1,
                0x01000001, "write-1", True, deadline=deadline)
        self.assertEqual(caught.exception.code, 202)
        self.assertFalse(caught.exception.retryable)
        self.assertTrue(caught.exception.possibly_committed)

    def test_status_fanout_cancels_pending_work_at_shared_deadline(self):
        started = []
        lock = threading.Lock()

        class SlowStatusClient:
            def resource_status(self, _node_id, resource_id):
                with lock:
                    started.append(resource_id)
                time.sleep(0.15)
                return {"resource_id": resource_id}

        provider = ToolbusdSnapshotProvider(
            SlowStatusClient(), maximum_concurrent_status_queries=1)
        descriptors = [{"resource_id": value} for value in (1, 2, 3)]
        deadline = MonotonicDeadline.after_seconds(0.03)
        began = time.monotonic()
        with self.assertRaises(RequestDeadlineExceeded):
            provider._read_resource_statuses(  # type: ignore[attr-defined]
                SlowStatusClient(), 1, descriptors, deadline)
        self.assertLess(time.monotonic() - began, 0.12)
        with lock:
            self.assertEqual(started, [1])
        # 等待唯一已运行任务结束，避免后台线程影响后续用例。
        time.sleep(0.14)

    def test_repeated_legacy_timeouts_keep_one_bounded_generation(self):
        release = threading.Event()
        started = []
        started_condition = threading.Condition()

        class BlockingLegacyClient:
            def resource_status(self, _node_id, resource_id):
                with started_condition:
                    started.append(resource_id)
                    started_condition.notify_all()
                release.wait(2)
                return {"resource_id": resource_id}

        client = BlockingLegacyClient()
        provider = ToolbusdSnapshotProvider(
            client, maximum_concurrent_status_queries=2)
        baseline_threads = sum(
            thread.name.startswith("runtime-status")
            for thread in threading.enumerate())
        descriptors = [
            {"resource_id": resource_id}
            for resource_id in (1, 2, 3)
        ]
        try:
            for attempt in range(3):
                with self.assertRaises(RequestDeadlineExceeded):
                    provider._read_resource_statuses(  # type: ignore[attr-defined]
                        client, 1, descriptors,
                        MonotonicDeadline.after_seconds(0.03))
                if attempt == 0:
                    with started_condition:
                        reached = started_condition.wait_for(
                            lambda: len(started) == 2, timeout=1)
                    self.assertTrue(reached)
                self.assertEqual(len(started), 2)
                self.assertLessEqual(sum(
                    thread.name.startswith("runtime-status")
                    for thread in threading.enumerate()),
                    baseline_threads + 2)

            release.set()
            recovered = provider._read_resource_statuses(
                client, 1, descriptors,
                MonotonicDeadline.after_seconds(1))
            self.assertEqual(
                [status[0]["resource_id"] for status in recovered],
                [1, 2, 3])
            self.assertEqual(len(started), 5)
        finally:
            release.set()
            provider._status_executor.shutdown(  # type: ignore[attr-defined]
                wait=True, cancel_futures=True)

    def test_short_identity_refresh_does_not_invalidate_long_waiter_or_lease(self):
        calls = 0
        calls_lock = threading.Lock()

        def reader():
            nonlocal calls
            with calls_lock:
                calls += 1
                current = calls
            if current == 2:
                time.sleep(0.04)
            return "1" * 32

        manager = DaemonBoundControlLeaseManager(reader, capacity=4)
        lease, _ = manager.acquire(
            owner_key_id="operator", node_id="node-1",
            resource_id="gpio-0", command_group="write", ttl_ms=1000,
            idempotency_key="request-1")
        outcomes = []

        def short_request():
            try:
                manager.active_count(deadline=
                    MonotonicDeadline.after_seconds(0.01))
            except Exception as error:
                outcomes.append(type(error))

        worker = threading.Thread(target=short_request)
        worker.start()
        time.sleep(0.005)
        count = manager.active_count(
            deadline=MonotonicDeadline.after_seconds(1))
        worker.join(timeout=1)
        self.assertFalse(worker.is_alive())
        self.assertEqual(outcomes, [RequestDeadlineExceeded])
        self.assertEqual(count, 1)
        self.assertEqual(manager.daemon_instance_id, "1" * 32)
        self.assertEqual(manager.authorize(
            lease.lease_id, requester_key_id="operator").lease_id,
            lease.lease_id)
        self.assertGreaterEqual(calls, 3)


if __name__ == "__main__":
    unittest.main()

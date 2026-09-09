import threading
import time
import unittest

from runtime_api.audit import (
    AUDIT_SCHEMA_VERSION,
    BoundedAuditSink,
    new_audit_record,
)


class BoundedAuditSinkTest(unittest.TestCase):
    @staticmethod
    def _record(index: int):
        return new_audit_record(
            request_id=f"{index:032x}",
            key_id="concurrent-reader",
            method_category="read",
            path_category="snapshot",
            result="allowed")

    def test_capacity_remains_bounded_under_concurrency(self):
        sink = BoundedAuditSink(capacity=16)

        def emit_range(start: int) -> None:
            for index in range(start, start + 25):
                sink.emit(self._record(index))

        threads = [threading.Thread(target=emit_range, args=(index * 25,))
                   for index in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())

        records = sink.snapshot()
        self.assertEqual(len(records), 16)
        self.assertEqual(sink.overwritten_count, 84)
        self.assertTrue(all(record.schema_version == AUDIT_SCHEMA_VERSION
                            for record in records))

    def test_injected_output_is_structured_and_failure_is_isolated(self):
        received: list[dict] = []
        sink = BoundedAuditSink(capacity=2, output=received.append)
        sink.emit(self._record(1))
        self.assertTrue(sink.close())
        self.assertEqual(received[0]["path_category"], "snapshot")
        self.assertNotIn("api_key", received[0])

        failing = BoundedAuditSink(
            capacity=2,
            output=lambda _: (_ for _ in ()).throw(RuntimeError("失败")))
        failing.emit(self._record(2))
        self.assertTrue(failing.close())
        self.assertEqual(failing.output_failure_count, 1)
        self.assertEqual(len(failing.snapshot()), 1)

    def test_slow_output_does_not_block_emit_and_full_queue_is_counted(self):
        entered = threading.Event()
        release = threading.Event()

        def slow_output(_: dict) -> None:
            entered.set()
            release.wait(timeout=2)

        sink = BoundedAuditSink(capacity=1, output=slow_output)
        sink.emit(self._record(1))
        self.assertTrue(entered.wait(timeout=1))
        started = time.monotonic()
        sink.emit(self._record(2))
        sink.emit(self._record(3))
        self.assertLess(time.monotonic() - started, 0.1)
        self.assertEqual(sink.dropped_output_count, 1)
        release.set()
        self.assertTrue(sink.close())

    def test_capacity_and_categories_are_strict(self):
        for capacity in (0, 4097):
            with self.subTest(capacity=capacity), self.assertRaises(ValueError):
                BoundedAuditSink(capacity=capacity)
        with self.assertRaises(ValueError):
            BoundedAuditSink(output="not-callable")  # type: ignore[arg-type]
        with self.assertRaises(ValueError):
            new_audit_record(
                request_id="r" * 33, key_id=None, method_category="read",
                path_category="snapshot", result="allowed")
        with self.assertRaises(ValueError):
            new_audit_record(
                request_id="0" * 32, key_id="bad\nidentity",
                method_category="read", path_category="snapshot",
                result="allowed")
        with self.assertRaises(ValueError):
            new_audit_record(
                request_id="r", key_id=None, method_category="read",
                path_category="snapshot", result="raw value\n")


if __name__ == "__main__":
    unittest.main()

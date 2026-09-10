import unittest

from runtime_api.operations import RuntimeOperationalState


class OperationalStateTest(unittest.TestCase):
    def test_snapshot_is_bounded_and_contains_no_error_details(self):
        ticks = iter([1_000_000, 2_000_000, 3_000_000, 5_000_000])
        state = RuntimeOperationalState(error_capacity=2,
                                        monotonic_ns=lambda: next(ticks))
        state.record_error("first_error")
        state.record_error("second_error")
        snapshot = state.snapshot(
            stream_limit=8,
            trend={"availability": "available", "configured": False},
            toolbusd={"availability": "unknown", "connection": "unknown"})
        self.assertEqual(snapshot["runtime"]["uptime_ms"], 4)
        self.assertEqual(snapshot["recent_errors"]["count"], 2)
        self.assertEqual(snapshot["recent_errors"]["items"][0]["code"],
                         "second_error")
        self.assertNotIn("message", str(snapshot))
        self.assertEqual(snapshot["logical_recording"]["availability"],
                         "unknown")

    def test_stream_count_cannot_underflow(self):
        state = RuntimeOperationalState()
        state.stream_closed()
        state.stream_opened()
        state.stream_closed()
        snapshot = state.snapshot(
            stream_limit=1, trend={},
            toolbusd={"availability": "unknown", "connection": "unknown"})
        self.assertEqual(snapshot["overview_stream"]["active_connections"], 0)


if __name__ == "__main__":
    unittest.main()

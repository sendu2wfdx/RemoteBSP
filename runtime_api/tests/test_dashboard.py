import unittest

from runtime_api.dashboard import RuntimeDashboard
from runtime_api.provider import mock_snapshot


class RuntimeDashboardTests(unittest.TestCase):
    def test_node_health_preserves_unavailable_instead_of_zero(self):
        snapshot = mock_snapshot()
        snapshot["nodes"][0]["runtime"]["health_snapshot"] = {
            "availability": "unavailable", "reason": "sample_stale",
            "sample_age_ms": 6000, "snapshot": None,
        }
        view = RuntimeDashboard().observe(snapshot)
        health = view["nodes"][0]["health"]
        self.assertEqual(health["availability"], "unavailable")
        self.assertEqual(health["overall"], "unknown")
        self.assertEqual(health["metrics"], [])
        self.assertEqual(health["sample_age_ms"], 6000)

    def test_resource_availability_and_missing_health_are_explicit(self):
        snapshot = mock_snapshot()
        snapshot["nodes"][0]["resources"][1]["available"] = False
        view = RuntimeDashboard().observe(snapshot)
        resources = view["nodes"][0]["resources"]
        self.assertEqual(resources[0]["availability"], "available")
        self.assertEqual(resources[0]["health"], "unknown")
        self.assertEqual(resources[1]["availability"], "unavailable")
        self.assertEqual(view["toolbusd_health"]["availability"], "unknown")
        self.assertEqual(view["toolbusd_health"]["metrics"], [])

    def test_trend_is_bounded_deduplicated_and_has_peaks(self):
        dashboard = RuntimeDashboard(2)
        snapshot = mock_snapshot()
        for sequence, value in ((1, 4), (2, 9), (3, 2)):
            snapshot["snapshot_id"] = f"mock-{sequence}"
            snapshot["captured_at_ms"] = sequence
            snapshot["nodes"][0]["runtime"]["queue_depth"] = value
            view = dashboard.observe(snapshot)
        trend = view["nodes"][0]["trend"]
        self.assertEqual(len(trend["samples"]), 2)
        self.assertEqual(trend["peaks"]["queue_depth"], 9)
        # 同一快照重复读取不能制造趋势样本。
        again = dashboard.observe(snapshot)
        self.assertEqual(len(again["nodes"][0]["trend"]["samples"]), 2)

    def test_health_zero_is_value_only_when_explicitly_available(self):
        health = {"available": True, "snapshot": {
            "overall": "healthy", "producer_generation": 4,
            "sample_sequence": 7, "sample_time_ms": 100, "metrics": [
                {"name": "cpu_load_permille", "availability": "available",
                 "unit": "permille", "value": 0},
                {"name": "isr_load_permille", "availability": "unavailable",
                 "unit": "permille", "value": 0},
                {"name": "cpu_load_permille", "availability": "available",
                 "unit": "permille", "value": 960},
            ]}}
        view = RuntimeDashboard().observe(mock_snapshot(), health)
        metrics = view["toolbusd_health"]["metrics"]
        self.assertEqual(metrics[0]["value"], 0)
        self.assertIsNone(metrics[1]["value"])
        alerts = view["toolbusd_health"]["threshold_alerts"]
        self.assertEqual(alerts[0]["severity"], "critical")
        self.assertEqual(alerts[0]["critical_threshold"], 950)
        self.assertEqual(view["toolbusd_health"]["trend"]["peaks"][
            "cpu_load_permille"], 960)


if __name__ == "__main__":
    unittest.main()

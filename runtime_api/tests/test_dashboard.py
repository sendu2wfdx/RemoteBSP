import unittest
from pathlib import Path

from runtime_api.dashboard import RuntimeDashboard
from runtime_api.provider import mock_snapshot


class RuntimeDashboardTests(unittest.TestCase):
    @staticmethod
    def _ipc_health(instance: str, sequence: int, **changes):
        ipc = {
            "active_clients": 1, "maximum_clients": 4,
            "peak_clients": 2, "accepted_total": 10,
            "capacity_rejected_total": 0, "oversized_frame_total": 0,
            "timeout_total": 0, "thread_creation_failed_total": 0,
        }
        ipc.update(changes)
        return {"available": True, "snapshot": {
            "daemon_instance_id": instance, "overall": "healthy",
            "producer_generation": 1, "sample_sequence": sequence,
            "sample_time_ms": sequence, "metrics": [], "ipc": ipc}}

    def test_ipc_health_deltas_alert_recovery_and_restart_boundary(self):
        dashboard = RuntimeDashboard()
        first = dashboard.observe(
            mock_snapshot(), self._ipc_health("a" * 32, 1))
        self.assertFalse(first["toolbusd_health"]["ipc"][
            "same_daemon_baseline"])
        self.assertIsNone(first["toolbusd_health"]["ipc"]["deltas"][
            "timeout"])

        second = dashboard.observe(mock_snapshot(), self._ipc_health(
            "a" * 32, 2, active_clients=4, peak_clients=4,
            capacity_rejected_total=2, oversized_frame_total=1,
            timeout_total=3, thread_creation_failed_total=1))
        ipc = second["toolbusd_health"]["ipc"]
        self.assertEqual(ipc["deltas"]["capacity_rejected"], 2)
        self.assertEqual(ipc["deltas"]["timeout"], 3)
        self.assertTrue(all(alert["active"] for alert in ipc["alerts"]))
        repeated = dashboard.observe(mock_snapshot(), self._ipc_health(
            "a" * 32, 2, active_clients=4, peak_clients=4,
            capacity_rejected_total=2, oversized_frame_total=1,
            timeout_total=3, thread_creation_failed_total=1))
        self.assertEqual(repeated["toolbusd_health"]["ipc"]["deltas"],
                         ipc["deltas"])

        recovered = dashboard.observe(mock_snapshot(), self._ipc_health(
            "a" * 32, 3, capacity_rejected_total=2,
            oversized_frame_total=1, timeout_total=3,
            thread_creation_failed_total=1))
        self.assertFalse(any(alert["active"] for alert in
                             recovered["toolbusd_health"]["ipc"]["alerts"]))

        restarted = dashboard.observe(
            mock_snapshot(), self._ipc_health("b" * 32, 1))
        self.assertFalse(restarted["toolbusd_health"]["ipc"][
            "same_daemon_baseline"])
        self.assertTrue(all(value is None for value in restarted[
            "toolbusd_health"]["ipc"]["deltas"].values()))

    def test_web_bus_reset_requires_capability_lease_and_locks_unknown(self):
        script = (Path(__file__).parents[1] / "static" / "dashboard.js").read_text()
        self.assertIn("busResetPermitted", script)
        self.assertIn('command_group:"bus.reset"', script)
        self.assertIn('state.busLeases.get(key)', script)
        self.assertIn('state.unknownBusOperations.set', script)
        self.assertIn("禁止重复提交", script)
        self.assertIn("查询操作状态", script)

    def test_bus_reset_operations_are_bounded_in_overview_model(self):
        operations = [{"node_id": "node-a", "resource_id": f"resource-{i:08x}",
                       "operation": {"operation_id": f"{i:064x}",
                                     "state": "unknown"},
                       "audit_result": "bus_resource_reset"}
                      for i in range(300)]
        view = RuntimeDashboard().observe(mock_snapshot(),
                                          bus_reset_operations=operations)
        self.assertEqual(len(view["bus_reset_operations"]), 256)
        self.assertEqual(view["bus_reset_operations"][0]["resource_id"],
                         "resource-0000002c")

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

    def test_bus_health_detail_preserves_units_and_unknown_cumulative(self):
        snapshot = mock_snapshot()
        resource = snapshot["nodes"][0]["resources"][0]
        resource["kind"] = "i2c_device"
        resource["state"]["bus_health"] = {
            "availability": "available", "last_status": "timeout",
            "last_result_age_ms": 125, "consecutive_failures": 3,
            "peak_consecutive_failures": 7, "cumulative_failures": None,
            "cumulative_availability": "unavailable"}
        projected = RuntimeDashboard().observe(snapshot)["nodes"][0]["resources"][0]
        self.assertEqual(projected["bus_health"]["last_status"], "timeout")
        self.assertEqual(projected["bus_health"]["last_result_age_ms"], 125)
        self.assertIsNone(projected["bus_health"]["cumulative_failures"])


if __name__ == "__main__":
    unittest.main()

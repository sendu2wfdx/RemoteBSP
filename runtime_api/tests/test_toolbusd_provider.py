import json
import subprocess
import threading
import time
import unittest
from unittest.mock import patch

from runtime_api.provider import RuntimeProviderError
from runtime_api.toolbusd_provider import (
    RemoteCliIpcClient,
    ToolbusdSnapshotProvider,
    ToolbusIpcError,
    ToolbusIpcProtocolError,
)


class MotionGroupStatusContractTests(unittest.TestCase):
    def _document(self, **changes):
        data = {
            "transaction_id": 21, "group_id": 7, "plan_generation": 3,
            "state": "aborting", "abort_reason": 9, "member_count": 2,
            "ready_count": 2, "committed_count": 1,
            "pending_request_count": 1, "commit_dispatched": True,
            "abort_is_best_effort": True, "result_unknown": True,
        }
        data.update(changes)
        return json.dumps({"schema_version": 1,
                           "command": "motion-group-status", "data": data})

    def test_partial_commit_preserves_original_error_and_unknown_result(self):
        status = RemoteCliIpcClient._json_motion_group_status(self._document())
        self.assertEqual(status["state"], "aborting")
        self.assertEqual(status["committed_count"], 1)
        self.assertEqual(status["abort_reason"], 9)
        self.assertTrue(status["abort_is_best_effort"])
        self.assertTrue(status["result_unknown"])

    def test_rejects_inconsistent_unknown_result(self):
        with self.assertRaisesRegex(ToolbusIpcProtocolError, "未知结果语义"):
            RemoteCliIpcClient._json_motion_group_status(
                self._document(result_unknown=False))

    def test_rejects_invalid_original_error_type(self):
        with self.assertRaisesRegex(ToolbusIpcProtocolError, "abort_reason"):
            RemoteCliIpcClient._json_motion_group_status(
                self._document(abort_reason="9"))


class FakeToolbusClient:
    def traffic_status(self):
        return {"mode": "fd", "available_permille": 900}

    def list_nodes(self):
        return [{
            "node_id": 1,
            "online": True,
            "ready": True,
            "board_type": 0x103,
            "firmware": (1, 2, 3),
            "protocol_version": 1,
            "uuid": "11" * 16,
        }, {
            "node_id": 2,
            "online": False,
            "ready": False,
            "board_type": 0x431,
            "firmware": (2, 0, 0),
            "protocol_version": 1,
            "uuid": "22" * 16,
        }]

    def list_resources(self, node_id):
        if node_id != 1:
            raise AssertionError("离线节点不应读取资源")
        return [{
            "resource_id": 0x01000001,
            "kind": "gpio",
            "instance": 0,
            "source": "native",
            "rx_capacity": 1,
            "tx_capacity": 1,
        }, {
            "resource_id": 0x09000001,
            "kind": "stepgen_axis",
            "instance": 0,
            "source": "native",
            "rx_capacity": 0,
            "tx_capacity": 32,
        }]

    def resource_status(self, node_id, resource_id):
        self.last_status_request = (node_id, resource_id)
        health = "failed" if resource_id == 0x09000001 else "normal"
        return {
            "resource_id": resource_id,
            "health": health,
            "error_flags": 4 if health == "failed" else 0,
            "rx_buffered": 0,
            "tx_buffered": 0,
            "rx_overruns": 0,
            "tx_overruns": 0,
        }


def node_health_source(node_id, generation=10, sequence=1, sample_time_ms=100,
                       source=2, overall=1, value=125):
    health = {
        "contract_version": 1, "source": source, "overall": overall,
        "sample_sequence": sequence, "sample_time_ms": sample_time_ms,
        "node_id": node_id, "producer_generation": generation,
        "metrics": [{"metric_id": 1, "availability": 1,
                     "unit": 3, "value": value}],
    }
    document = json.dumps({"schema_version": 1,
                           "command": "node-health-snapshot",
                           "data": {"health": health}})
    return RemoteCliIpcClient._json_node_health_snapshot(document, node_id)


class RemoteCliIpcClientTest(unittest.TestCase):
    def test_logical_recording_status_is_strict_and_hides_output_name(self):
        document = json.dumps({
            "schema_version": 1, "command": "logical-recording-status",
            "data": {"configured": True, "active": True,
                     "evidence_scope": "logical_link_frames",
                     "output_name": "private-capture.json",
                     "event_count": 12, "maximum_events": 100,
                     "maximum_file_bytes": 4096}})
        client = RemoteCliIpcClient(
            "/private/toolbusd.sock", "remote-cli",
            runner=lambda *_: document)
        status = client.logical_recording_status()
        self.assertTrue(status["active"])
        self.assertEqual(status["event_count"], 12)
        self.assertNotIn("output_name", status)
        bad = json.loads(document)
        bad["data"]["private_path"] = "/secret"
        client.runner = lambda *_: json.dumps(bad)
        with self.assertRaises(ToolbusIpcProtocolError):
            client.logical_recording_status()

    @staticmethod
    def _runtime_snapshot_document():
        counters = {
            "admitted_packets": 0, "rejected_packets": 0,
            "admitted_frames": 0, "estimated_wire_time_ns": 0,
        }
        return json.dumps({
            "schema_version": 1,
            "command": "runtime-snapshot",
            "data": {
                "snapshot_version": 3,
                "snapshot_sequence": 7,
                "traffic": {
                    "mode": "fd", "arbitration_bitrate": 1000000,
                    "data_bitrate": 5000000,
                    "max_utilization_permille": 700,
                    "burst_window_ms": 20, "available_permille": 900,
                    "admitted_packets": 0, "rejected_packets": 0,
                    "guaranteed_overruns": 0, "admitted_frames": 0,
                    "estimated_wire_time_ns": 0,
                    "classes": [dict(counters, **{"class": name})
                                for name in ("safety", "motion", "system",
                                             "interactive", "streaming",
                                             "bulk")],
                },
                "nodes": [{
                    "node_id": 1, "online": True, "ready": True,
                    "board_type": 0x431,
                    "firmware": {"major": 1, "minor": 2, "patch": 3},
                    "protocol_version": 1, "uuid": "ab" * 16,
                }],
                "resources": [{
                    "node_id": 1, "status_valid": False,
                    "descriptor": {
                        "resource_id": 0x01000001, "type": "gpio",
                        "instance": 0, "source": "native",
                        "rx_capacity": 1, "tx_capacity": 1,
                    },
                    "status": {
                        "resource_id": 0x01000001, "health": 0,
                        "health_name": "normal", "error_flags": 0,
                        "rx_buffered": 0, "tx_buffered": 0,
                        "rx_overruns": 0, "tx_overruns": 0,
                    },
                }],
                "bus_health": [{
                    "node_id": 1, "resource_id": 0x0C000001,
                    "last_status_valid": True, "last_status": 2,
                    "consecutive_failures": 2,
                    "peak_consecutive_failures": 4,
                    "last_result_time_us": 123456,
                }],
                "node_issues": [],
                "clocks": [{
                    "node_id": 1,
                    "registered": True,
                    "estimate_valid": True,
                    "state": "synced",
                    "boot_epoch": 9,
                    "model_generation": 12,
                    "sample_count": 8,
                    "selected_sample_count": 6,
                    "rate_deviation_ppb": -80,
                    "drift_uncertainty_ppm": 25,
                    "minimum_network_rtt_ns": 100000,
                    "error_bound_ns": 60000,
                    "sample_age_ns": 500000,
                    "last_sample_host_time_ns": 123456,
                }],
            },
        })

    def test_runtime_snapshot_is_one_strict_structured_invocation(self):
        calls = []

        def runner(command, timeout, maximum_output):
            calls.append(list(command))
            return self._runtime_snapshot_document()

        client = RemoteCliIpcClient(
            "/tmp/test.sock", timeout_seconds=1.5, runner=runner)
        snapshot = client.runtime_snapshot(64)
        self.assertEqual(snapshot["sequence"], 7)
        self.assertFalse(snapshot["resources"][0]["status_valid"])
        self.assertEqual(snapshot["version"], 3)
        self.assertEqual(snapshot["bus_health"][0]["consecutive_failures"], 2)
        self.assertEqual(snapshot["clocks"][0]["state"], "synced")
        self.assertEqual(snapshot["clocks"][0]["rate_deviation_ppb"], -80)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][-3:], ["runtime-snapshot", "64", "1400"])

    def test_runtime_snapshot_v4_gpio_diagnostics_and_v3_compatibility(self):
        legacy = RemoteCliIpcClient._json_runtime_snapshot(
            self._runtime_snapshot_document())
        self.assertEqual(legacy["version"], 3)
        self.assertEqual(legacy["gpio_input_diagnostics"], [])

        document = json.loads(self._runtime_snapshot_document())
        document["data"]["snapshot_version"] = 4
        document["data"]["gpio_input_diagnostics_total_count"] = 2
        document["data"]["gpio_input_diagnostics_truncated"] = True
        document["data"]["gpio_input_diagnostics"] = [{
            "node_id": 1, "resource_id": 0x01000001,
            "object_id": 17, "pin": 1, "status_valid": True,
            "status_version": 2, "queued_events": 2,
            "queue_capacity": 8, "dropped_events": 3,
            "last_sequence": 9, "exti_diagnostics_available": True,
            "mailbox_dropped": 4, "hints_matched": 12,
            "hints_ignored": 1,
        }]
        parsed = RemoteCliIpcClient._json_runtime_snapshot(
            json.dumps(document))
        self.assertEqual(parsed["gpio_input_diagnostics"][0]
                         ["mailbox_dropped"], 4)
        self.assertTrue(parsed["gpio_input_diagnostics_truncated"])

        unavailable = document["data"]["gpio_input_diagnostics"][0]
        unavailable.update({
            "status_valid": False, "status_version": 0,
            "queued_events": 0, "queue_capacity": 0,
            "dropped_events": 0, "last_sequence": 0,
            "exti_diagnostics_available": False, "mailbox_dropped": 0,
            "hints_matched": 0, "hints_ignored": 0,
        })
        parsed = RemoteCliIpcClient._json_runtime_snapshot(
            json.dumps(document))
        self.assertFalse(parsed["gpio_input_diagnostics"][0]["status_valid"])

    def test_daemon_identity_is_strict_and_normalized(self):
        calls = []

        def runner(command, timeout, maximum_output):
            calls.append(list(command))
            return json.dumps({
                "schema_version": 1,
                "command": "daemon-identity",
                "data": {"ipc_version": 1, "instance_id": "AB" * 16},
            })

        client = RemoteCliIpcClient(
            "/tmp/test.sock", timeout_seconds=1.5, runner=runner)
        self.assertEqual(client.daemon_identity(), "ab" * 16)
        self.assertEqual(calls[0][-1], "daemon-identity")

        for value in ("0" * 32, "ab" * 15, "z" * 32):
            with self.subTest(value=value), self.assertRaises(
                    ToolbusIpcProtocolError):
                client = RemoteCliIpcClient(
                    "/tmp/test.sock", runner=lambda *_: json.dumps({
                        "schema_version": 1,
                        "command": "daemon-identity",
                        "data": {"ipc_version": 1,
                                 "instance_id": value},
                    }))
                client.daemon_identity()

    def test_runtime_snapshot_v2_rejects_v1_and_bad_clock_quality(self):
        def document():
            return json.loads(self._runtime_snapshot_document())

        invalid_cases = []
        old_version = document()
        old_version["data"]["snapshot_version"] = 1
        invalid_cases.append((old_version, "版本不受支持"))

        missing_clocks = document()
        del missing_clocks["data"]["clocks"]
        invalid_cases.append((missing_clocks, "字段不匹配"))

        unknown_node = document()
        unknown_node["data"]["clocks"][0]["node_id"] = 2
        invalid_cases.append((unknown_node, "引用未知节点"))

        invalid_unknown = document()
        clock = invalid_unknown["data"]["clocks"][0]
        clock["estimate_valid"] = False
        clock["state"] = "unsynced"
        invalid_cases.append((invalid_unknown, "未知时钟估计字段不一致"))

        invalid_unregistered = document()
        clock = invalid_unregistered["data"]["clocks"][0]
        clock["registered"] = False
        clock["state"] = "unregistered"
        invalid_cases.append((invalid_unregistered, "未注册时钟字段不一致"))

        for payload, message in invalid_cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(
                        ToolbusIpcProtocolError, message):
                    RemoteCliIpcClient._json_runtime_snapshot(
                        json.dumps(payload))

    def test_explicit_legacy_mode_parses_existing_text_without_shell(self):
        """旧文本兼容必须显式启用，且仍只组合参数数组。"""
        calls = []

        def runner(command, timeout, maximum_output):
            calls.append(list(command))
            self.assertEqual(timeout, 1.5)
            self.assertEqual(maximum_output, 4096)
            operation = next(item for item in (
                "traffic-status", "node-list", "resource-list",
                "resource-status") if item in command)
            if operation == "traffic-status":
                return (
                    "mode=fd arbitration_bitrate=1000000 "
                    "data_bitrate=5000000 max_utilization_permille=700 "
                    "burst_window_ms=20 available_permille=900 "
                    "admitted_packets=10 rejected_packets=1 "
                    "guaranteed_overruns=0 admitted_frames=12 "
                    "estimated_wire_time_ns=5000\n"
                    "class=safety admitted_packets=1 rejected_packets=0 "
                    "admitted_frames=1 estimated_wire_time_ns=100\n")
            if operation == "node-list":
                return (
                    "node_id=1 online=1 ready=1 board_type=0x431 "
                    "firmware=1.2.3 protocol_version=1 uuid=" +
                    "ab" * 16 + "\n")
            if operation == "resource-list":
                return (
                    "resource_id=0x1000001 type=gpio instance=0 "
                    "source=native rx_capacity=1 tx_capacity=1\n")
            return (
                "resource_id=0x1000001 health=0 error_flags=0x0 "
                "health_name=normal rx_buffered=0 tx_buffered=0 "
                "rx_overruns=0 tx_overruns=0\n")

        client = RemoteCliIpcClient(
            "/tmp/test-toolbusd.sock", "/opt/remotebsp/remote-cli",
            timeout_seconds=1.5, maximum_output_bytes=4096,
            structured_output=False, runner=runner)
        self.assertEqual(client.traffic_status()["mode"], "fd")
        self.assertEqual(client.list_nodes()[0]["firmware"], (1, 2, 3))
        self.assertEqual(client.list_resources(1)[0]["kind"], "gpio")
        self.assertEqual(client.resource_status(
            1, 0x01000001)["health"], "normal")
        self.assertTrue(all(call[:3] == [
            "/opt/remotebsp/remote-cli", "--socket",
            "/tmp/test-toolbusd.sock"] for call in calls))
        self.assertTrue(all("--json" not in call for call in calls))
        self.assertTrue(all(isinstance(call, list) for call in calls))
        self.assertEqual(calls[-1][-2:], ["resource-status", "16777217"])

    def test_rejects_incomplete_or_inconsistent_cli_output(self):
        def incomplete(command, timeout, maximum_output):
            return "node_id=1 online=1\n"

        with self.assertRaisesRegex(ToolbusIpcProtocolError, "缺少字段"):
            RemoteCliIpcClient(
                "/tmp/test.sock", structured_output=False,
                runner=incomplete).list_nodes()

        def inconsistent(command, timeout, maximum_output):
            return (
                "resource_id=0x1 health=3 error_flags=0 "
                "health_name=normal rx_buffered=0 tx_buffered=0 "
                "rx_overruns=0 tx_overruns=0\n")

        with self.assertRaisesRegex(ToolbusIpcProtocolError, "不一致"):
            RemoteCliIpcClient(
                "/tmp/test.sock", structured_output=False,
                runner=inconsistent).resource_status(1, 1)

    def test_structured_output_is_default_and_parses_all_read_operations(self):
        calls = []

        def document(command, data):
            return json.dumps({
                "schema_version": 1,
                "command": command,
                "data": data,
            })

        def runner(command, timeout, maximum_output):
            calls.append((list(command), timeout, maximum_output))
            operation = next(item for item in (
                "traffic-status", "node-list", "resource-list",
                "resource-status") if item in command)
            if operation == "traffic-status":
                counters = {
                    "admitted_packets": 0,
                    "rejected_packets": 0,
                    "admitted_frames": 0,
                    "estimated_wire_time_ns": 0,
                }
                traffic = {
                    "mode": "fd",
                    "arbitration_bitrate": 1000000,
                    "data_bitrate": 5000000,
                    "max_utilization_permille": 700,
                    "burst_window_ms": 20,
                    "available_permille": 900,
                    "admitted_packets": 10,
                    "rejected_packets": 1,
                    "guaranteed_overruns": 0,
                    "admitted_frames": 12,
                    "estimated_wire_time_ns": 5000,
                    "classes": [dict(counters, **{"class": name}) for name in (
                        "safety", "motion", "system", "interactive",
                        "streaming", "bulk")],
                }
                return document(operation, {"traffic": traffic})
            if operation == "node-list":
                return document(operation, {"nodes": [{
                    "node_id": 1,
                    "online": True,
                    "ready": True,
                    "board_type": 0x431,
                    "firmware": {"major": 1, "minor": 2, "patch": 3},
                    "protocol_version": 1,
                    "uuid": "ab" * 16,
                }]})
            if operation == "resource-list":
                return document(operation, {
                    "node_id": 1,
                    "resources": [{
                        "resource_id": 0x01000001,
                        "type": "gpio",
                        "instance": 0,
                        "source": "native",
                        "rx_capacity": 1,
                        "tx_capacity": 1,
                    }],
                })
            return document(operation, {
                "node_id": 1,
                "resource": {
                    "resource_id": 0x01000001,
                    "health": 0,
                    "health_name": "normal",
                    "error_flags": 0,
                    "rx_buffered": 0,
                    "tx_buffered": 0,
                    "rx_overruns": 0,
                    "tx_overruns": 0,
                },
            })

        client = RemoteCliIpcClient(
            "/tmp/test-toolbusd.sock", timeout_seconds=1.25,
            maximum_output_bytes=8192, runner=runner)
        self.assertEqual(client.traffic_status()["available_permille"], 900)
        self.assertEqual(client.list_nodes()[0]["firmware"], (1, 2, 3))
        self.assertEqual(client.list_resources(1)[0]["kind"], "gpio")
        self.assertEqual(
            client.resource_status(1, 0x01000001)["health"], "normal")
        self.assertTrue(all(call[0][1:4] == [
            "--json", "--socket", "/tmp/test-toolbusd.sock"]
                            for call in calls))
        self.assertTrue(all(call[1:] == (1.25, 8192) for call in calls))

    def test_structured_output_rejects_bad_envelope_without_legacy_fallback(self):
        invalid_outputs = (
            ("不是JSON", "合法JSON"),
            ('{"schema_version":1,"schema_version":1,'
             '"command":"node-list","data":{"nodes":[]}}', "重复字段"),
            ('{"schema_version":NaN,"command":"node-list",'
             '"data":{"nodes":[]}}', "非标准数值"),
            (json.dumps({
                "schema_version": 2, "command": "node-list",
                "data": {"nodes": []},
            }), "schema_version"),
            (json.dumps({
                "schema_version": 1, "command": "node-list",
                "data": {"nodes": []}, "unexpected": True,
            }), "字段不匹配"),
            (json.dumps({
                "schema_version": 1.0, "command": "node-list",
                "data": {"nodes": []},
            }), "JSON整数"),
        )
        for output, message in invalid_outputs:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ToolbusIpcProtocolError, message):
                    RemoteCliIpcClient(
                        "/tmp/test.sock",
                        runner=lambda command, timeout, maximum: output,
                    ).list_nodes()

        with self.assertRaisesRegex(ToolbusIpcProtocolError, "输出超过"):
            RemoteCliIpcClient(
                "/tmp/test.sock", maximum_output_bytes=8,
                runner=lambda command, timeout, maximum: "{}" * 5,
            ).list_nodes()

    def test_structured_output_rejects_wrong_parent_identifiers(self):
        def wrong_resource_node(command, timeout, maximum):
            return json.dumps({
                "schema_version": 1,
                "command": "resource-list",
                "data": {"node_id": 2, "resources": []},
            })

        with self.assertRaisesRegex(ToolbusIpcProtocolError, "错误的节点ID"):
            RemoteCliIpcClient(
                "/tmp/test.sock", runner=wrong_resource_node,
            ).list_resources(1)

        def wrong_status_resource(command, timeout, maximum):
            return json.dumps({
                "schema_version": 1,
                "command": "resource-status",
                "data": {
                    "node_id": 1,
                    "resource": {
                        "resource_id": 2,
                        "health": 0,
                        "health_name": "normal",
                        "error_flags": 0,
                        "rx_buffered": 0,
                        "tx_buffered": 0,
                        "rx_overruns": 0,
                        "tx_overruns": 0,
                    },
                },
            })

        with self.assertRaisesRegex(ToolbusIpcProtocolError, "错误的资源ID"):
            RemoteCliIpcClient(
                "/tmp/test.sock", runner=wrong_status_resource,
            ).resource_status(1, 1)

    def test_subprocess_timeout_is_explicit(self):
        with patch(
                "runtime_api.toolbusd_provider.subprocess.run",
                side_effect=subprocess.TimeoutExpired("remote-cli", 0.1)):
            with self.assertRaisesRegex(ToolbusIpcError, "调用失败"):
                RemoteCliIpcClient(
                    "/tmp/test.sock", timeout_seconds=0.1,
                ).list_nodes()


    def test_pwm_control_commands_and_strict_outcomes(self):
        calls = []
        def runner(command, _timeout, _maximum):
            calls.append(list(command))
            operation = command[-10] if "runtime-pwm-configure-operation" in command else command[-7]
            if "runtime-pwm-acquire" in command:
                operation = "runtime-pwm-acquire"
                data = {}
            elif "runtime-pwm-configure-operation" in command:
                operation = "runtime-pwm-configure-operation"
                data = {"operation_id": "a" * 64, "lease_id": "2" * 32,
                    "expected_node_uuid": "3" * 32, "resource_id": 0x06000000,
                    "kind": "pwm_configure", "state": "committed", "replayed": False,
                    "recovery": "none", "object_id": 9, "value": None,
                    "frequency_hz": 20000, "duty": 4200, "active_low": True,
                    "error_code": None}
            else:
                operation = "runtime-pwm-stop-operation"
                data = {"operation_id": "b" * 64, "lease_id": "2" * 32,
                    "expected_node_uuid": "3" * 32, "resource_id": 0x06000000,
                    "kind": "pwm_stop", "state": "committed", "replayed": False,
                    "recovery": "safe_closed", "object_id": 9, "value": None,
                    "frequency_hz": None, "duty": None, "active_low": None,
                    "error_code": None}
            return json.dumps({"schema_version": 1, "command": operation, "data": data})
        client = RemoteCliIpcClient("/tmp/test.sock", runner=runner)
        client.runtime_pwm_acquire("1" * 32, "2" * 32, "3" * 32,
                                   "owner", 7, 0x06000000, 1000)
        configured = client.runtime_pwm_configure_operation(
            "1" * 32, "2" * 32, "3" * 32, "owner", 7, 0x06000000,
            "pwm-1", 20000, 4200, True)
        self.assertEqual(configured["duty"], 4200)
        stopped = client.runtime_pwm_stop_operation(
            "1" * 32, "2" * 32, "3" * 32, "owner", 7, 0x06000000, "stop-1")
        self.assertEqual(stopped["kind"], "pwm_stop")
        self.assertTrue(any("runtime-pwm-acquire" in call for call in calls))
        with self.assertRaises(ToolbusIpcProtocolError):
            client.runtime_pwm_configure_operation(
                "1" * 32, "2" * 32, "3" * 32, "owner", 7, 0x06000000,
                "bad", 0, 10001, False)

    def test_bus_reset_control_commands_and_strict_outcome(self):
        calls = []
        def runner(command, _timeout, _maximum):
            calls.append(list(command))
            if "runtime-bus-reset-acquire" in command:
                operation, data = "runtime-bus-reset-acquire", {}
            else:
                operation = "runtime-bus-resource-reset-operation"
                data = {"operation_id": "d" * 64, "lease_id": "2" * 32,
                    "expected_node_uuid": "3" * 32,
                    "resource_id": 0x0c000001,
                    "kind": "bus_resource_reset", "state": "unknown",
                    "replayed": False, "recovery": "scope_blocked",
                    "object_id": None, "value": None,
                    "frequency_hz": None, "duty": None,
                    "active_low": None, "error_code": "deadline"}
            return json.dumps({"schema_version": 1, "command": operation,
                               "data": data})
        client = RemoteCliIpcClient("/tmp/test.sock", runner=runner)
        client.runtime_bus_reset_acquire(
            "1" * 32, "2" * 32, "3" * 32, "owner", 7, 0x0c000001, 900)
        outcome = client.runtime_bus_resource_reset_operation(
            "1" * 32, "2" * 32, "3" * 32, "owner", 7, 0x0c000001,
            "reset-1")
        self.assertEqual(outcome["state"], "unknown")
        self.assertEqual(outcome["recovery"], "scope_blocked")
        self.assertTrue(any("runtime-bus-reset-acquire" in call for call in calls))


class ToolbusdSnapshotProviderTest(unittest.TestCase):
    def test_bus_health_detail_uses_stable_status_and_explicit_unknown_total(self):
        with patch("runtime_api.toolbusd_provider.time.monotonic_ns",
                   return_value=2_000_000_000):
            detail = ToolbusdSnapshotProvider._bus_health_detail({
                "last_status_valid": True, "last_status": 2,
                "consecutive_failures": 3,
                "peak_consecutive_failures": 8,
                "last_result_time_us": 1_900_000})
        self.assertEqual(detail["last_status"], "timeout")
        self.assertEqual(detail["last_result_age_ms"], 100)
        self.assertEqual(detail["consecutive_failures"], 3)
        self.assertEqual(detail["peak_consecutive_failures"], 8)
        self.assertIsNone(detail["cumulative_failures"])
        self.assertEqual(detail["cumulative_availability"], "unavailable")

    def test_pwm_target_resolution_reuses_verified_structure_cache(self):
        provider = ToolbusdSnapshotProvider(FakeToolbusClient(), cache_ttl_ms=0)
        provider._cached_snapshot = {
            "nodes": [{
                "node_id": "node-" + "3" * 32,
                "state": "online",
                "runtime": {"bus_node_id": 7},
                "resources": [{
                    "resource_id": "resource-06000000",
                    "kind": "pwm",
                    "available": True,
                }],
            }],
        }
        with patch.object(provider, "read_snapshot",
                          side_effect=AssertionError("不应刷新完整快照")):
            self.assertEqual(provider._resolve_pwm_target(
                "node-" + "3" * 32, "resource-06000000"),
                ("3" * 32, 7, 0x06000000))

    def test_node_health_is_bound_to_route_and_source(self):
        valid = node_health_source(1)
        self.assertEqual(valid["source"], 2)
        with self.assertRaisesRegex(ToolbusIpcProtocolError, "错误的节点ID"):
            RemoteCliIpcClient._json_node_health_snapshot(json.dumps({
                "schema_version": 1, "command": "node-health-snapshot",
                "data": {"health": {
                    "contract_version": 1, "source": 2, "overall": 1,
                    "sample_sequence": 1, "sample_time_ms": 1,
                    "node_id": 2, "producer_generation": 1,
                    "metrics": [{"metric_id": 1, "availability": 1,
                                 "unit": 3, "value": 1}],
                }}}), 1)
        with self.assertRaisesRegex(ToolbusIpcProtocolError, "来源"):
            node_health_source(1, source=3)

    def test_node_health_isolated_stale_and_retired_generation(self):
        now = [100]

        class HealthClient(FakeToolbusClient):
            def __init__(self):
                self.generation = 10
                self.sequence = 1
                self.fail_node = None

            def node_health_snapshot(self, node_id):
                if node_id == self.fail_node:
                    raise ToolbusIpcError("节点局部读取失败")
                return node_health_source(
                    node_id, self.generation, self.sequence,
                    sample_time_ms=self.sequence * 10)

        client = HealthClient()
        provider = ToolbusdSnapshotProvider(
            client, clock_ms=lambda: now[0], cache_ttl_ms=0,
            maximum_node_health_sample_age_ms=50)
        first = provider.get_snapshot()
        online = next(node for node in first["nodes"]
                      if node["runtime"]["bus_node_id"] == 1)
        self.assertEqual(online["runtime"]["health_snapshot"]["availability"],
                         "available")
        self.assertEqual(online["runtime"]["health_snapshot"]["snapshot"]
                         ["metrics"][0]["value"], 125)
        offline = next(node for node in first["nodes"]
                       if node["runtime"]["bus_node_id"] == 2)
        self.assertEqual(offline["runtime"]["health_snapshot"]["availability"],
                         "unknown")

        now[0] = 151
        stale = provider.get_snapshot()["nodes"][0]
        self.assertEqual(stale["runtime"]["health_snapshot"]["reason"],
                         "node_health_sample_stale")
        self.assertIsNone(stale["runtime"]["health_snapshot"]["snapshot"])

        now[0] = 160
        client.generation = 11
        restarted = provider.get_snapshot()["nodes"][0]
        self.assertEqual(restarted["runtime"]["health_snapshot"]["availability"],
                         "available")
        client.generation = 10
        rejected = provider.get_snapshot()["nodes"][0]
        self.assertEqual(rejected["runtime"]["health_snapshot"]["reason"],
                         "node_health_retired_generation")

        client.generation = 11
        client.sequence = 2
        client.fail_node = 1
        isolated = provider.get_snapshot()
        self.assertEqual(len(isolated["nodes"]), 2)
        bad = next(node for node in isolated["nodes"]
                   if node["runtime"]["bus_node_id"] == 1)
        self.assertEqual(bad["runtime"]["health_snapshot"]["reason"],
                         "node_health_read_failed")
    def test_explicit_legacy_client_keeps_existing_fanout_path(self):
        class LegacyCapableClient(FakeToolbusClient):
            structured_output = False

            def __init__(self):
                self.snapshot_calls = 0

            def runtime_snapshot(self, maximum_resources):
                self.snapshot_calls += 1
                raise AssertionError("显式旧文本模式不应调用单次快照")

        client = LegacyCapableClient()
        snapshot = ToolbusdSnapshotProvider(
            client, clock_ms=lambda: 5).get_snapshot()
        self.assertEqual(client.snapshot_calls, 0)
        self.assertEqual(snapshot["snapshot_id"], "toolbusd-5")
        clock = snapshot["nodes"][0]["runtime"]["clock_sync"]
        self.assertFalse(clock["source_available"])
        self.assertIsNone(clock["registered"])
        self.assertEqual(clock["state"], "unknown")
        codes = {alert["code"] for alert in snapshot["alerts"]}
        self.assertIn("clock_sync_observability_unavailable", codes)
        self.assertNotIn("clock_sync_unregistered", codes)
        self.assertNotIn("clock_sync_unsynced", codes)
        capability = ToolbusdSnapshotProvider(client).runtime_capabilities()[
            "clock_sync_quality"]
        self.assertFalse(capability["available"])
        self.assertEqual(capability["source"], "legacy_text")
        self.assertEqual(capability["estimate_kind"], "unavailable")

    def test_single_snapshot_path_preserves_resource_failure_isolation(self):
        parsed = RemoteCliIpcClient._json_runtime_snapshot(
            RemoteCliIpcClientTest._runtime_snapshot_document())

        class AtomicClient:
            def __init__(self):
                self.calls = 0

            def runtime_snapshot(self, maximum_resources):
                self.calls += 1
                self.maximum_resources = maximum_resources
                return parsed

            def traffic_status(self):
                raise AssertionError("不应调用旧traffic-status")

            def list_nodes(self):
                raise AssertionError("不应调用旧node-list")

        client = AtomicClient()
        snapshot = ToolbusdSnapshotProvider(
            client, clock_ms=lambda: 123).get_snapshot()
        self.assertEqual(client.calls, 1)
        self.assertEqual(client.maximum_resources, 128)
        self.assertEqual(snapshot["snapshot_id"], "toolbusd-7")
        clock = snapshot["nodes"][0]["runtime"]["clock_sync"]
        self.assertTrue(clock["source_available"])
        self.assertTrue(clock["estimate_valid"])
        self.assertEqual(clock["boot_epoch"], 9)
        self.assertEqual(clock["model_generation"], 12)
        self.assertEqual(clock["error_bound_ns"], 60000)
        resource = snapshot["nodes"][0]["resources"][0]
        self.assertEqual(resource["state"]["health"], "unknown")
        self.assertFalse(resource["available"])
        self.assertIn("resource_status_unavailable",
                      {alert["code"] for alert in snapshot["alerts"]})

    def test_gpio_input_diagnostics_are_projected_and_isolated(self):
        document = json.loads(
            RemoteCliIpcClientTest._runtime_snapshot_document())
        document["data"]["snapshot_version"] = 4
        document["data"]["gpio_input_diagnostics_total_count"] = 3
        document["data"]["gpio_input_diagnostics_truncated"] = True
        document["data"]["gpio_input_diagnostics"] = [{
            "node_id": 1, "resource_id": 0x01000001,
            "object_id": 17, "pin": 1, "status_valid": True,
            "status_version": 2, "queued_events": 1,
            "queue_capacity": 8, "dropped_events": 2,
            "last_sequence": 9, "exti_diagnostics_available": True,
            "mailbox_dropped": 0, "hints_matched": 12,
            "hints_ignored": 1,
        }, {
            "node_id": 1, "resource_id": 0x01000001,
            "object_id": 18, "pin": 1, "status_valid": False,
            "status_version": 0, "queued_events": 0,
            "queue_capacity": 0, "dropped_events": 0,
            "last_sequence": 0, "exti_diagnostics_available": False,
            "mailbox_dropped": 0, "hints_matched": 0,
            "hints_ignored": 0,
        }]
        parsed = RemoteCliIpcClient._json_runtime_snapshot(
            json.dumps(document))

        class AtomicClient:
            def runtime_snapshot(self, maximum_resources):
                return parsed

        snapshot = ToolbusdSnapshotProvider(
            AtomicClient(), clock_ms=lambda: 123).get_snapshot()
        node = snapshot["nodes"][0]
        self.assertEqual(len(node["runtime"]["gpio_input_diagnostics"]), 2)
        codes = {alert["code"] for alert in snapshot["alerts"]}
        self.assertIn("gpio_input_events_dropped", codes)
        self.assertIn("gpio_input_diagnostics_unavailable", codes)
        self.assertIn("gpio_input_diagnostics_truncated", codes)
        self.assertEqual(node["runtime"]["gpio_input_diagnostics_projection"]
                         ["total_count"], 3)

    def test_maps_nodes_resources_health_and_link_state(self):
        provider = ToolbusdSnapshotProvider(
            FakeToolbusClient(), clock_ms=lambda: 12345)
        snapshot = provider.get_snapshot()
        self.assertEqual(snapshot["snapshot_id"], "toolbusd-12345")
        self.assertEqual(len(snapshot["nodes"]), 2)
        online = next(node for node in snapshot["nodes"]
                      if node["runtime"]["bus_node_id"] == 1)
        offline = next(node for node in snapshot["nodes"]
                       if node["runtime"]["bus_node_id"] == 2)
        self.assertEqual(online["links"][0]["kind"], "can_fd")
        self.assertEqual(online["state"], "degraded")
        self.assertEqual(len(online["resources"]), 2)
        failed = next(resource for resource in online["resources"]
                      if resource["state"]["health"] == "failed")
        self.assertFalse(failed["available"])
        self.assertIn("resource_health",
                      {alert["code"] for alert in snapshot["alerts"]})
        self.assertIn("clock_sync_observability_unavailable",
                      {alert["code"] for alert in snapshot["alerts"]})
        self.assertEqual(offline["state"], "offline")
        self.assertEqual(offline["resources"], [])
        self.assertEqual(offline["last_seen_ms"], 0)
        self.assertFalse(offline["runtime"]["last_seen_known"])
        self.assertEqual(online["last_seen_ms"], 12345)
        self.assertTrue(online["runtime"]["last_seen_known"])

        capability = provider.runtime_capabilities()["clock_sync_quality"]
        self.assertFalse(capability["available"])
        self.assertEqual(capability["source"],
                         "runtime_snapshot_unavailable")
        self.assertEqual(capability["estimate_kind"], "unavailable")

    def test_clock_quality_alerts_are_stable_and_thresholds_are_bounded(self):
        def build(clock_updates, *, error_limit=250_000,
                  age_limit_ms=1_000):
            document = json.loads(
                RemoteCliIpcClientTest._runtime_snapshot_document())
            document["data"]["clocks"][0].update(clock_updates)
            parsed = RemoteCliIpcClient._json_runtime_snapshot(
                json.dumps(document))

            class AtomicClient:
                def runtime_snapshot(self, maximum_resources):
                    return parsed

            provider = ToolbusdSnapshotProvider(
                AtomicClient(), maximum_clock_error_bound_ns=error_limit,
                maximum_clock_sample_age_ms=age_limit_ms,
                cache_ttl_ms=0)
            return provider.get_snapshot(), provider.runtime_capabilities()

        null_estimate = {
            "estimate_valid": False,
            "state": "unsynced",
            "rate_deviation_ppb": None,
            "drift_uncertainty_ppm": None,
            "minimum_network_rtt_ns": None,
            "error_bound_ns": None,
            "sample_age_ns": None,
            "last_sample_host_time_ns": None,
        }
        unsynced, _ = build(null_estimate)
        self.assertEqual(
            {alert["code"] for alert in unsynced["alerts"]},
            {"clock_sync_unsynced", "resource_status_unavailable"})

        unregistered_fields = dict(null_estimate, **{
            "registered": False,
            "state": "unregistered",
            "boot_epoch": None,
            "model_generation": None,
            "sample_count": 0,
            "selected_sample_count": 0,
        })
        unregistered, _ = build(unregistered_fields)
        self.assertEqual(
            {alert["code"] for alert in unregistered["alerts"]},
            {"clock_sync_unregistered", "resource_status_unavailable"})

        degraded, capabilities = build({
            "state": "degraded",
            "error_bound_ns": 300_000,
            "sample_age_ns": 1_500_000_000,
        })
        codes = {alert["code"] for alert in degraded["alerts"]}
        self.assertTrue({
            "clock_sync_degraded", "clock_sync_error_bound_exceeded",
            "clock_sync_sample_stale",
        }.issubset(codes))
        quality = capabilities["clock_sync_quality"]
        self.assertTrue(quality["available"])
        self.assertEqual(quality["source"], "runtime_snapshot_v2")
        self.assertEqual(quality["estimate_kind"], "host_model_estimate")
        self.assertEqual(quality["maximum_error_bound_ns"], 250_000)
        self.assertEqual(quality["maximum_sample_age_ms"], 1_000)

        for keyword, value, message in (
                ("maximum_clock_error_bound_ns", 0, "误差告警阈值"),
                ("maximum_clock_error_bound_ns", 1_000_000_001,
                 "误差告警阈值"),
                ("maximum_clock_sample_age_ms", 0, "样本年龄告警阈值"),
                ("maximum_clock_sample_age_ms", 60_001,
                 "样本年龄告警阈值")):
            with self.subTest(keyword=keyword, value=value):
                with self.assertRaisesRegex(ValueError, message):
                    ToolbusdSnapshotProvider(
                        FakeToolbusClient(), **{keyword: value})

    def test_cache_refreshes_before_clock_sample_crosses_stale_threshold(self):
        def parsed_with_age(sample_age_ns):
            document = json.loads(
                RemoteCliIpcClientTest._runtime_snapshot_document())
            document["data"]["clocks"][0]["sample_age_ns"] = sample_age_ns
            return RemoteCliIpcClient._json_runtime_snapshot(
                json.dumps(document))

        snapshots = [parsed_with_age(900_000_000),
                     parsed_with_age(1_001_000_000)]

        class AdvancingClient:
            def __init__(self):
                self.calls = 0

            def runtime_snapshot(self, maximum_resources):
                value = snapshots[min(self.calls, len(snapshots) - 1)]
                self.calls += 1
                return value

        now = [0]
        client = AdvancingClient()
        provider = ToolbusdSnapshotProvider(
            client, clock_ms=lambda: now[0], cache_ttl_ms=10_000,
            maximum_clock_sample_age_ms=1_000)
        first = provider.read_snapshot()
        self.assertEqual(first.cache_status, "refresh")
        self.assertNotIn(
            "clock_sync_sample_stale",
            {alert["code"] for alert in first.snapshot["alerts"]})

        now[0] = 100
        boundary = provider.read_snapshot()
        self.assertEqual(boundary.cache_status, "hit")
        self.assertEqual(client.calls, 1)

        now[0] = 101
        stale = provider.read_snapshot()
        self.assertEqual(stale.cache_status, "refresh")
        self.assertEqual(client.calls, 2)
        self.assertIn(
            "clock_sync_sample_stale",
            {alert["code"] for alert in stale.snapshot["alerts"]})

    def test_duplicate_numeric_node_id_fails_before_resource_fanout(self):
        """相同路由ID不能因UUID不同而被当作两个节点重复查询。"""
        class DuplicateRouteClient(FakeToolbusClient):
            def __init__(self):
                self.resource_calls = 0

            def list_nodes(self):
                nodes = super().list_nodes()
                duplicate = dict(nodes[0])
                duplicate["uuid"] = "33" * 16
                nodes.append(duplicate)
                return nodes

            def list_resources(self, node_id):
                self.resource_calls += 1
                return super().list_resources(node_id)

        client = DuplicateRouteClient()
        with self.assertRaisesRegex(RuntimeProviderError,
                                    "重复numeric node_id"):
            ToolbusdSnapshotProvider(
                client, clock_ms=lambda: 12345).get_snapshot()
        self.assertEqual(client.resource_calls, 0)

    def test_resource_status_failure_is_isolated_and_visible(self):
        class PartialFailureClient(FakeToolbusClient):
            def resource_status(self, node_id, resource_id):
                if resource_id == 0x09000001:
                    raise ToolbusIpcError("测试超时")
                return super().resource_status(node_id, resource_id)

        snapshot = ToolbusdSnapshotProvider(
            PartialFailureClient(), clock_ms=lambda: 99).get_snapshot()
        online = next(node for node in snapshot["nodes"]
                      if node["runtime"]["bus_node_id"] == 1)
        self.assertEqual(len(online["resources"]), 2)
        unavailable = next(resource for resource in online["resources"]
                           if resource["state"]["health"] == "unknown")
        self.assertFalse(unavailable["available"])
        self.assertIn("resource_status_unavailable",
                      {alert["code"] for alert in snapshot["alerts"]})

    def test_protocol_mismatch_fails_whole_snapshot_explicitly(self):
        class IncompatibleClient(FakeToolbusClient):
            def list_resources(self, node_id):
                raise ToolbusIpcProtocolError("测试版本不兼容")

        with self.assertRaisesRegex(RuntimeProviderError, "协议不兼容"):
            ToolbusdSnapshotProvider(
                IncompatibleClient(), clock_ms=lambda: 1).get_snapshot()

    def test_resource_query_fanout_has_explicit_limit(self):
        """Web请求不能把资源状态读取放大为无界IPC调用。"""
        with self.assertRaisesRegex(RuntimeProviderError, "查询上限"):
            ToolbusdSnapshotProvider(
                FakeToolbusClient(), clock_ms=lambda: 1,
                maximum_resources_per_snapshot=1).get_snapshot()

    def test_short_cache_reports_age_and_returns_detached_snapshots(self):
        class CountingClient(FakeToolbusClient):
            def __init__(self):
                self.node_calls = 0

            def list_nodes(self):
                self.node_calls += 1
                return super().list_nodes()

        now = [1000]
        client = CountingClient()
        provider = ToolbusdSnapshotProvider(
            client, clock_ms=lambda: now[0], cache_ttl_ms=250)
        first = provider.read_snapshot()
        first.snapshot["nodes"].clear()
        now[0] += 100
        second = provider.read_snapshot()
        self.assertEqual(first.cache_status, "refresh")
        self.assertEqual(second.cache_status, "hit")
        self.assertEqual(second.age_ms, 100)
        self.assertEqual(second.cache_ttl_ms, 250)
        self.assertEqual(len(second.snapshot["nodes"]), 2)
        self.assertEqual(client.node_calls, 1)

        now[0] += 151
        third = provider.read_snapshot()
        self.assertEqual(third.cache_status, "refresh")
        self.assertEqual(client.node_calls, 2)

    def test_concurrent_web_reads_share_one_snapshot_refresh(self):
        class SlowClient(FakeToolbusClient):
            def __init__(self):
                self.node_calls = 0
                self.started = threading.Event()
                self.release = threading.Event()

            def list_nodes(self):
                self.node_calls += 1
                self.started.set()
                self.release.wait(timeout=2)
                return super().list_nodes()

        client = SlowClient()
        provider = ToolbusdSnapshotProvider(
            client, clock_ms=lambda: 100, cache_ttl_ms=250)
        results = []
        errors = []

        def read():
            try:
                results.append(provider.read_snapshot())
            except Exception as error:  # pragma: no cover - 便于断言线程异常
                errors.append(error)

        first = threading.Thread(target=read)
        second = threading.Thread(target=read)
        first.start()
        self.assertTrue(client.started.wait(timeout=1))
        second.start()
        time.sleep(0.02)
        client.release.set()
        first.join(timeout=2)
        second.join(timeout=2)
        self.assertEqual(errors, [])
        self.assertEqual(client.node_calls, 1)
        self.assertEqual({item.cache_status for item in results},
                         {"refresh", "hit"})

    def test_failed_refresh_is_short_cached_without_stale_fallback(self):
        class FailingClient(FakeToolbusClient):
            def __init__(self):
                self.node_calls = 0

            def list_nodes(self):
                self.node_calls += 1
                if self.node_calls == 1:
                    # 模拟刷新本身耗时超过缓存窗口；失败合并应从完成时起算。
                    now[0] += 1000
                raise ToolbusIpcError("测试不可用")

        now = [1]
        client = FailingClient()
        provider = ToolbusdSnapshotProvider(
            client, clock_ms=lambda: now[0], cache_ttl_ms=250)
        for _ in range(2):
            with self.assertRaisesRegex(RuntimeProviderError, "测试不可用"):
                provider.get_snapshot()
        self.assertEqual(client.node_calls, 1)
        now[0] += 251
        with self.assertRaises(RuntimeProviderError):
            provider.get_snapshot()
        self.assertEqual(client.node_calls, 2)

    def test_resource_status_parallelism_is_bounded(self):
        class ManyResourcesClient(FakeToolbusClient):
            def __init__(self):
                self.active = 0
                self.maximum_active = 0
                self.lock = threading.Lock()

            def list_resources(self, node_id):
                descriptor = super().list_resources(node_id)[0]
                return [dict(descriptor, resource_id=index + 1)
                        for index in range(12)]

            def resource_status(self, node_id, resource_id):
                with self.lock:
                    self.active += 1
                    self.maximum_active = max(self.maximum_active,
                                              self.active)
                time.sleep(0.01)
                with self.lock:
                    self.active -= 1
                return {
                    "resource_id": resource_id,
                    "health": "normal",
                    "error_flags": 0,
                    "rx_buffered": 0,
                    "tx_buffered": 0,
                    "rx_overruns": 0,
                    "tx_overruns": 0,
                }

        client = ManyResourcesClient()
        snapshot = ToolbusdSnapshotProvider(
            client, clock_ms=lambda: 1,
            maximum_concurrent_status_queries=3).get_snapshot()
        self.assertEqual(len(snapshot["nodes"][0]["resources"]), 12)
        self.assertGreater(client.maximum_active, 1)
        self.assertLessEqual(client.maximum_active, 3)


if __name__ == "__main__":
    unittest.main()

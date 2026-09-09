import unittest

from runtime_api.provider import RuntimeProviderError
from runtime_api.toolbusd_provider import (
    RemoteCliIpcClient,
    ToolbusdSnapshotProvider,
    ToolbusIpcError,
    ToolbusIpcProtocolError,
)


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


class RemoteCliIpcClientTest(unittest.TestCase):
    def test_parses_existing_read_only_cli_contract_without_shell(self):
        """适配器只组合参数数组，并解析现有 remote-cli 只读输出。"""
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
            runner=runner)
        self.assertEqual(client.traffic_status()["mode"], "fd")
        self.assertEqual(client.list_nodes()[0]["firmware"], (1, 2, 3))
        self.assertEqual(client.list_resources(1)[0]["kind"], "gpio")
        self.assertEqual(client.resource_status(
            1, 0x01000001)["health"], "normal")
        self.assertTrue(all(call[:3] == [
            "/opt/remotebsp/remote-cli", "--socket",
            "/tmp/test-toolbusd.sock"] for call in calls))
        self.assertTrue(all(isinstance(call, list) for call in calls))
        self.assertEqual(calls[-1][-2:], ["resource-status", "16777217"])

    def test_rejects_incomplete_or_inconsistent_cli_output(self):
        def incomplete(command, timeout, maximum_output):
            return "node_id=1 online=1\n"

        with self.assertRaisesRegex(ToolbusIpcProtocolError, "缺少字段"):
            RemoteCliIpcClient(
                "/tmp/test.sock", runner=incomplete).list_nodes()

        def inconsistent(command, timeout, maximum_output):
            return (
                "resource_id=0x1 health=3 error_flags=0 "
                "health_name=normal rx_buffered=0 tx_buffered=0 "
                "rx_overruns=0 tx_overruns=0\n")

        with self.assertRaisesRegex(ToolbusIpcProtocolError, "不一致"):
            RemoteCliIpcClient(
                "/tmp/test.sock", runner=inconsistent).resource_status(1, 1)


class ToolbusdSnapshotProviderTest(unittest.TestCase):
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
        self.assertEqual(snapshot["alerts"][0]["code"], "resource_health")
        self.assertEqual(offline["state"], "offline")
        self.assertEqual(offline["resources"], [])
        self.assertEqual(offline["last_seen_ms"], 0)
        self.assertFalse(offline["runtime"]["last_seen_known"])
        self.assertEqual(online["last_seen_ms"], 12345)
        self.assertTrue(online["runtime"]["last_seen_known"])

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


if __name__ == "__main__":
    unittest.main()

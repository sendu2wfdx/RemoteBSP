import copy
import json
import socket
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from runtime_api.models import RuntimeContractError, normalize_snapshot
from runtime_api.provider import (
    FileSnapshotProvider,
    MockSnapshotProvider,
    RuntimeProviderError,
    mock_snapshot,
)
from runtime_api.server import make_server


PACKAGE_ROOT = Path(__file__).resolve().parents[1]


class RuntimeModelTest(unittest.TestCase):
    def test_json_schema_document_is_valid(self):
        """随包交付的机器可读契约必须始终是合法 JSON。"""
        schema = json.loads((PACKAGE_ROOT /
                             "runtime-snapshot-v1.schema.json").read_text(
                                 encoding="utf-8"))
        self.assertEqual(schema["properties"]["schema_version"]["const"], 1)
        self.assertIn("node", schema["$defs"])

    def test_snapshot_is_sorted_and_detached(self):
        """规范化结果顺序稳定，且不与 Provider 输入共享可变状态。"""
        source = mock_snapshot()
        second = copy.deepcopy(source["nodes"][0])
        second["node_id"] = "another-node"
        second["resources"] = []
        source["nodes"].insert(0, second)
        source["nodes"][1]["resources"].reverse()
        normalized = normalize_snapshot(source)
        self.assertEqual([item["node_id"] for item in normalized["nodes"]],
                         ["another-node", "mock-node-1"])
        self.assertEqual([item["resource_id"] for item in
                          normalized["nodes"][1]["resources"]],
                         ["gpio-0", "motion-axis-0"])
        source["nodes"].clear()
        self.assertEqual(len(normalized["nodes"]), 2)

    def test_snapshot_rejects_duplicate_and_dangling_references(self):
        """重复身份和悬空告警在进入 HTTP 层前即被拒绝。"""
        duplicate = mock_snapshot()
        duplicate["nodes"].append(copy.deepcopy(duplicate["nodes"][0]))
        with self.assertRaisesRegex(RuntimeContractError, "重复node_id"):
            normalize_snapshot(duplicate)

        dangling = mock_snapshot()
        dangling["alerts"].append({
            "alert_id": "alert-1",
            "node_id": "missing-node",
            "resource_id": None,
            "severity": "error",
            "code": "node_lost",
            "message": "节点失联",
            "active": True,
            "occurred_at_ms": 10,
        })
        with self.assertRaisesRegex(RuntimeContractError, "未知节点"):
            normalize_snapshot(dangling)

        duplicate_link = mock_snapshot()
        duplicate_link["nodes"][0]["links"].append(
            {"kind": "mock", "state": "offline"})
        with self.assertRaisesRegex(RuntimeContractError, "重复kind"):
            normalize_snapshot(duplicate_link)

    def test_required_fields_and_empty_mock_are_not_silently_defaulted(self):
        """缺少契约字段或空 Mock 必须暴露错误，不能伪装成在线节点。"""
        missing_state = mock_snapshot()
        missing_state["nodes"][0]["resources"][0].pop("state")
        with self.assertRaisesRegex(RuntimeContractError, "state.*缺省"):
            normalize_snapshot(missing_state)
        missing_runtime = mock_snapshot()
        missing_runtime["nodes"][0].pop("runtime")
        with self.assertRaisesRegex(RuntimeContractError, "runtime.*缺省"):
            normalize_snapshot(missing_runtime)
        with self.assertRaises(RuntimeContractError):
            MockSnapshotProvider({})

    def test_file_provider_reloads_and_bounds_input(self):
        """文件 Provider 每次读取新快照，并限制输入大小。"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "snapshot.json"
            first = mock_snapshot()
            path.write_text(json.dumps(first), encoding="utf-8")
            provider = FileSnapshotProvider(path)
            self.assertEqual(provider.get_snapshot()["snapshot_id"], "mock-1")
            first["snapshot_id"] = "mock-2"
            path.write_text(json.dumps(first), encoding="utf-8")
            self.assertEqual(provider.get_snapshot()["snapshot_id"], "mock-2")
            with self.assertRaisesRegex(RuntimeProviderError, "超过"):
                FileSnapshotProvider(path, maximum_bytes=1).get_snapshot()

    def test_ipv6_loopback_uses_ipv6_server_when_available(self):
        """声明支持的 ::1 必须真正使用 IPv6，而不是生成无效监听。"""
        try:
            server = make_server("::1", 0, MockSnapshotProvider())
        except OSError as error:
            self.skipTest(f"当前环境没有IPv6回环：{error}")
        try:
            self.assertEqual(server.address_family, socket.AF_INET6)
        finally:
            server.server_close()


class RuntimeHttpTest(unittest.TestCase):
    def test_server_factory_enforces_loopback_boundary(self):
        """库调用也不能绕过无认证阶段的本机监听限制。"""
        with self.assertRaisesRegex(ValueError, "回环地址"):
            make_server("0.0.0.0", 0, MockSnapshotProvider())
        with self.assertRaisesRegex(ValueError, "HTTP工作线程数"):
            make_server("127.0.0.1", 0, MockSnapshotProvider(),
                        maximum_workers=0)

    def setUp(self):
        snapshot = mock_snapshot()
        snapshot["alerts"].append({
            "alert_id": "warning-1",
            "node_id": "mock-node-1",
            "resource_id": "gpio-0",
            "severity": "warning",
            "code": "mock_warning",
            "message": "用于测试的告警",
            "active": True,
            "occurred_at_ms": 0,
        })
        self.server = make_server("127.0.0.1", 0,
                                  MockSnapshotProvider(snapshot))
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def _get(self, path):
        return json.loads(urlopen(self.base + path).read())

    def test_versioned_read_endpoints(self):
        root = self._get("/api/v1")
        self.assertTrue(root["ok"])
        self.assertTrue(root["data"]["capabilities"]["read_only"])
        self.assertFalse(root["data"]["capabilities"]["write_commands"])

        health = self._get("/api/v1/health")
        self.assertEqual(health["data"]["snapshot_id"], "mock-1")
        nodes = self._get("/api/v1/nodes")["data"]
        self.assertEqual(nodes[0]["resource_count"], 2)
        self.assertEqual(nodes[0]["active_alert_count"], 1)
        node = self._get("/api/v1/nodes/mock-node-1")["data"]
        self.assertEqual(node["board_type"], "mock-generic-v1")
        resources = self._get("/api/v1/resources")["data"]
        self.assertEqual({item["node_id"] for item in resources},
                         {"mock-node-1"})
        self.assertEqual(len(self._get(
            "/api/v1/nodes/mock-node-1/alerts")["data"]), 1)
        self.assertEqual(self._get("/api/v1/snapshot")["data"][
            "schema_version"], 1)
        request = Request(self.base + "/api/v1/nodes", method="HEAD")
        with urlopen(request) as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), b"")

    def test_snapshot_freshness_is_explicit_without_guessing_file_age(self):
        with urlopen(self.base + "/api/v1/snapshot") as response:
            payload = json.loads(response.read())
            self.assertEqual(response.headers[
                "X-RemoteBSP-Snapshot-Cache"], "disabled")
            self.assertEqual(response.headers[
                "X-RemoteBSP-Snapshot-Age-Ms"], "unknown")
        freshness = payload["meta"]["snapshot_freshness"]
        self.assertEqual(freshness, {
            "cache_status": "disabled",
            "age_ms": None,
            "cache_ttl_ms": None,
        })

    def test_illegal_paths_queries_and_writes_are_bounded(self):
        for path, status, code in (
                ("/api/v1/nodes/missing", 404, "node_not_found"),
                ("/api/v2/nodes", 404, "not_found"),
                ("/api/v1/nodes?limit=1", 400, "query_not_supported"),
                ("/api/v1/nodes/mock-node-1/unknown", 404, "not_found")):
            with self.subTest(path=path), self.assertRaises(HTTPError) as caught:
                urlopen(self.base + path)
            self.assertEqual(caught.exception.code, status)
            payload = json.loads(caught.exception.read())
            self.assertEqual(payload["error"]["code"], code)

        request = Request(self.base + "/api/v1/nodes", data=b"{}",
                          method="POST")
        with self.assertRaises(HTTPError) as caught:
            urlopen(request)
        self.assertEqual(caught.exception.code, 405)
        self.assertEqual(caught.exception.headers["Allow"], "GET, HEAD")

    def test_provider_failure_returns_service_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "broken.json"
            path.write_text("not-json", encoding="utf-8")
            self.server.provider = FileSnapshotProvider(path)  # type: ignore[attr-defined]
            with self.assertRaises(HTTPError) as caught:
                urlopen(self.base + "/api/v1/snapshot")
            self.assertEqual(caught.exception.code, 503)
            payload = json.loads(caught.exception.read())
            self.assertEqual(payload["error"]["code"],
                             "provider_unavailable")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""真实 Mock MCU/toolbusd 后端上的 Runtime HTTP 总线复位闭环。"""

import json
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from runtime_api.auth import (ApiKeyAuthenticator, ApiKeyCredential,
    BUS_RESET_PERMISSION, CONTROL_LEASE_ACQUIRE_PERMISSION,
    CONTROL_OPERATION_READ_PERMISSION)
from runtime_api.control_leases import ControlLeaseManager, DaemonBoundControlLeaseManager
from runtime_api.server import make_server
from runtime_api.tests.control_audit_support import FakeControlAuditJournal
from runtime_api.toolbusd_provider import RemoteCliIpcClient, ToolbusdSnapshotProvider


def wait_path(path: Path) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.02)
    raise RuntimeError(f"等待套接字超时：{path}")


def request(base: str, path: str, body=None):
    data = None if body is None else json.dumps(body).encode()
    headers = {"X-API-Key": "a" * 32}
    if data is not None:
        headers["Content-Type"] = "application/json"
    return Request(base + path, data=data, headers=headers,
                   method="GET" if body is None else "POST")


def main() -> int:
    toolbusd, mock_mcu, remote_cli, manifest = sys.argv[1:5]
    with tempfile.TemporaryDirectory(prefix="remotebsp-http-bus-reset-") as work:
        root = Path(work); link = root / "usb.sock"; ipc = root / "toolbusd.sock"
        ledger = root / "ledger"
        daemon = subprocess.Popen([toolbusd, str(link), "usb-mock", str(ipc),
            "--runtime-operation-ledger-dir", str(ledger),
            "--test-bus-reset-drop-response", "1"])
        node = None; server = None; thread = None
        try:
            wait_path(ipc); wait_path(link)
            node = subprocess.Popen([mock_mcu, str(link), "usb-mock", "--board", manifest])
            client = RemoteCliIpcClient(ipc, executable=remote_cli, timeout_seconds=3)
            provider = ToolbusdSnapshotProvider(client, cache_ttl_ms=0)
            deadline = time.monotonic() + 8
            snapshot = None
            while time.monotonic() < deadline:
                try:
                    snapshot = provider.read_snapshot().snapshot
                    if snapshot["nodes"] and snapshot["nodes"][0]["resources"]:
                        break
                except Exception:
                    time.sleep(0.05)
            if snapshot is None or not snapshot["nodes"] or \
                    not snapshot["nodes"][0]["resources"]:
                raise RuntimeError("未发现真实 Mock 节点")
            runtime_node = snapshot["nodes"][0]
            devices = [item for item in runtime_node["resources"]
                       if item["kind"] in {"i2c_device", "spi_device"}]
            if len(devices) != 2:
                raise RuntimeError("Mock 清单没有两个总线设备：" +
                    repr([(item["resource_id"], item["kind"])
                          for item in runtime_node["resources"]]))
            manager = DaemonBoundControlLeaseManager(
                client.daemon_identity, manager=ControlLeaseManager(8))
            auth = ApiKeyAuthenticator([ApiKeyCredential("operator", "a" * 32,
                frozenset({BUS_RESET_PERMISSION, CONTROL_LEASE_ACQUIRE_PERMISSION,
                           CONTROL_OPERATION_READ_PERMISSION}))])
            server = make_server("127.0.0.1", 0, provider, authenticator=auth,
                control_lease_manager=manager,
                control_audit_journal=FakeControlAuditJournal())
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            base = f"http://127.0.0.1:{server.server_port}"

            def acquire(resource, key):
                body = {"node_id": runtime_node["node_id"],
                        "resource_id": resource["resource_id"],
                        "command_group": "bus.reset", "ttl_ms": 5000,
                        "idempotency_key": key}
                return json.loads(urlopen(request(base, "/api/v1/control-leases", body)).read())[
                    "data"]["lease"]["lease_id"]

            first_lease = acquire(devices[0], "lease-first")
            reset = {"lease_id": first_lease, "node_id": runtime_node["node_id"],
                     "resource_id": devices[0]["resource_id"],
                     "idempotency_key": "reset-lost"}
            try:
                urlopen(request(base, "/api/v1/control/bus/reset", reset))
                raise AssertionError("首次复位丢响应必须返回Unknown")
            except HTTPError as error:
                assert error.code == 409
                document = json.loads(error.read())
                operation = document["error"]["details"]["operation"]
                assert operation["state"] == "unknown"
                assert operation["recovery"] == "scope_blocked"
                location = error.headers["Location"]
            try:
                urlopen(request(base, location))
                raise AssertionError("Unknown状态查询必须保持冲突响应")
            except HTTPError as error:
                assert error.code == 409
                queried = json.loads(error.read())["error"]["details"]["operation"]
            assert queried["operation_id"] == operation["operation_id"]
            assert queried["replayed"] is True

            blocked = {"node_id": runtime_node["node_id"],
                       "resource_id": devices[0]["resource_id"],
                       "command_group": "bus.reset", "ttl_ms": 5000,
                       "idempotency_key": "lease-blocked"}
            try:
                urlopen(request(base, "/api/v1/control-leases", blocked))
                raise AssertionError("Unknown目标资源必须保持冻结")
            except HTTPError as error:
                assert error.code == 409
                assert json.loads(error.read())["error"]["code"] == \
                    "control_scope_blocked"

            peer_lease = acquire(devices[1], "lease-peer")
            peer = {"lease_id": peer_lease, "node_id": runtime_node["node_id"],
                    "resource_id": devices[1]["resource_id"],
                    "idempotency_key": "reset-peer"}
            peer_result = json.loads(urlopen(request(
                base, "/api/v1/control/bus/reset", peer)).read())["data"]["operation"]
            assert peer_result["state"] == "committed"
            assert peer_result["recovery"] == "safe_closed"
            print("Runtime HTTP BusReset 真实 Mock 进程测试通过")
            return 0
        finally:
            if server is not None:
                server.shutdown(); server.server_close()
            if thread is not None:
                thread.join(timeout=2)
            for process in (node, daemon):
                if process is not None and process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=3)
                    except subprocess.TimeoutExpired: process.kill()


if __name__ == "__main__":
    raise SystemExit(main())

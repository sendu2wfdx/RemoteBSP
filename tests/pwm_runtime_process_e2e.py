#!/usr/bin/env python3
"""真实 HTTP→remote-cli→toolbusd→Mock MCU 的 Runtime PWM 闭环。"""

from __future__ import annotations

import json
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from runtime_api.auth import (
    ApiKeyAuthenticator, ApiKeyCredential,
    CONTROL_LEASE_ACQUIRE_PERMISSION, CONTROL_LEASE_RELEASE_PERMISSION,
    CONTROL_OPERATION_READ_PERMISSION,
    PWM_WRITE_PERMISSION, RUNTIME_READ_PERMISSION,
)
from runtime_api.control_leases import DaemonBoundControlLeaseManager
from runtime_api.control_audit_journal import ControlAuditJournal
from runtime_api.server import make_server
from runtime_api.toolbusd_provider import RemoteCliIpcClient, ToolbusdSnapshotProvider


def request(base: str, path: str, secret: str, *, body=None, method=None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    headers = {"Authorization": "Bearer " + secret}
    if data is not None:
        headers["Content-Type"] = "application/json"
    return Request(base + path, data=data, headers=headers,
                   method=method or ("GET" if body is None else "POST"))


def document(base: str, path: str, secret: str, *, body=None, method=None):
    try:
        with urlopen(request(base, path, secret, body=body, method=method), timeout=15) as response:
            return response.status, json.loads(response.read())["data"]
    except HTTPError as error:
        print(error.read().decode("utf-8"), file=sys.stderr)
        raise


def resource_health(base: str, secret: str, node_id: str,
                    resource_id: str) -> str:
    _, snapshot = document(base, "/api/v1/snapshot", secret)
    node = next(item for item in snapshot["nodes"] if item["node_id"] == node_id)
    resource = next(item for item in node["resources"]
                    if item["resource_id"] == resource_id)
    return resource["state"]["health"]


def main() -> int:
    if len(sys.argv) not in (3, 4):
        raise SystemExit("用法: pwm_runtime_process_e2e.py <socket> <remote-cli> [lease-required]")
    socket_path, remote_cli = sys.argv[1:3]
    mode = sys.argv[3] if len(sys.argv) == 4 else "normal"
    lease_required = mode in {"lease-required", "quarantine"}
    ipc = RemoteCliIpcClient(socket_path, remote_cli, timeout_seconds=5.0)
    provider = ToolbusdSnapshotProvider(ipc, cache_ttl_ms=0)
    manager = DaemonBoundControlLeaseManager(ipc.daemon_identity, capacity=8)
    operator = "pwm-e2e-operator-key-00000000001"
    observer = "pwm-e2e-observer-key-00000000001"
    auth = ApiKeyAuthenticator([
        ApiKeyCredential("pwm-operator", operator, frozenset({
            RUNTIME_READ_PERMISSION, CONTROL_LEASE_ACQUIRE_PERMISSION,
            CONTROL_LEASE_RELEASE_PERMISSION, CONTROL_OPERATION_READ_PERMISSION,
            PWM_WRITE_PERMISSION,
        })),
        ApiKeyCredential("pwm-observer", observer,
                         frozenset({RUNTIME_READ_PERMISSION})),
    ])
    temporary = tempfile.TemporaryDirectory(prefix="remotebsp-pwm-http-")
    root = Path(temporary.name)
    audit_key = root / "audit.key"
    audit_key.write_bytes(bytes(range(32)))
    audit_key.chmod(0o600)
    journal = ControlAuditJournal(root / "audit", audit_key)
    server = make_server("127.0.0.1", 0, provider, authenticator=auth,
                         control_lease_manager=manager,
                         control_audit_journal=journal)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        base = f"http://127.0.0.1:{server.server_address[1]}"
        _, snapshot = document(base, "/api/v1/snapshot", operator)
        candidates = [(node["node_id"], resource["resource_id"])
                      for node in snapshot["nodes"]
                      for resource in node["resources"]
                      if resource["kind"] == "pwm"]
        assert candidates, "Mock MCU 未公开 PWM 资源"
        node_id, resource_id = candidates[0]
        initial = next(resource for node in snapshot["nodes"]
                       if node["node_id"] == node_id
                       for resource in node["resources"]
                       if resource["resource_id"] == resource_id)
        assert initial["state"]["health"] == "normal"

        lease_body = {"node_id": node_id, "resource_id": resource_id,
                      "command_group": "pwm.write",
                      "ttl_ms": 100 if mode == "quarantine" else 10000,
                      "idempotency_key": "pwm-e2e-lease"}
        if mode == "quarantine":
            started = time.monotonic()
            try:
                document(base, "/api/v1/control-leases", operator,
                         body=lease_body)
                raise AssertionError("丢失响应的acquire意外成功")
            except HTTPError as error:
                assert error.code in {409, 503, 504}
            assert time.monotonic() - started < 2.0
            time.sleep(0.15)
            blocked = dict(lease_body, idempotency_key="pwm-quarantine-blocked")
            try:
                document(base, "/api/v1/control-leases", operator, body=blocked)
                raise AssertionError("quarantine期间作用域意外被接管")
            except HTTPError as error:
                assert error.code == 409
            time.sleep(0.25)
            recovered = dict(lease_body, idempotency_key="pwm-quarantine-recovered")
            status, acquired = document(base, "/api/v1/control-leases", operator,
                                        body=recovered)
            assert status == 201
            document(base, f"/api/v1/control-leases/{acquired['lease']['lease_id']}",
                     operator, method="DELETE")
            return 0
        status, acquired = document(base, "/api/v1/control-leases", operator,
                                    body=lease_body)
        assert status == 201
        lease_id = acquired["lease"]["lease_id"]
        configure = {"lease_id": lease_id, "node_id": node_id,
                     "resource_id": resource_id,
                     "idempotency_key": "pwm-e2e-configure",
                     "frequency_hz": 20000, "duty": 4200,
                     "active_low": False}

        # 无 PWM 权限的认证身份必须在接触 toolbusd 前失败，随后合法请求仍成功。
        try:
            document(base, "/api/v1/control/pwm/configure", observer,
                     body=configure)
            raise AssertionError("只读身份意外获得 PWM 写权限")
        except HTTPError as error:
            assert error.code == 403

        first = document(base, "/api/v1/control/pwm/configure", operator,
                         body=configure)[1]["operation"]
        assert first["operation_kind"] == "pwm_configure"
        assert first["state"] == "committed" and first["replayed"] is False
        assert first["result"]["object_id"] > 0
        time.sleep(0.3)
        assert resource_health(base, operator, node_id, resource_id) == "busy"
        replay = document(base, "/api/v1/control/pwm/configure", operator,
                          body=configure)[1]["operation"]
        assert replay["operation_id"] == first["operation_id"]
        assert replay["replayed"] is True

        stop = {"lease_id": lease_id, "node_id": node_id,
                "resource_id": resource_id,
                "idempotency_key": "pwm-e2e-stop"}
        stopped = document(base, "/api/v1/control/pwm/stop", operator,
                           body=stop)[1]["operation"]
        assert stopped["operation_kind"] == "pwm_stop"
        assert stopped["state"] == "committed"
        assert stopped["recovery"] == "safe_closed"
        assert resource_health(base, operator, node_id, resource_id) == \
            ("busy" if lease_required else "normal")
        stop_replay = document(base, "/api/v1/control/pwm/stop", operator,
                               body=stop)[1]["operation"]
        assert stop_replay["operation_id"] == stopped["operation_id"]
        assert stop_replay["replayed"] is True
        released = document(base, f"/api/v1/control-leases/{lease_id}",
                            operator, method="DELETE")[1]["operation"]
        assert released["operation_kind"] == "control_release"
        assert released["state"] == "committed"
        assert resource_health(base, operator, node_id, resource_id) == "normal"
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)
        journal.close()
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

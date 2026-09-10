#!/usr/bin/env python3
"""在真实 toolbusd/remote-cli 进程上验证认证 Runtime Health 只读链路。"""

from __future__ import annotations

import json
import sys
import threading
from urllib.request import Request, urlopen

from runtime_api.auth import (
    ApiKeyAuthenticator,
    ApiKeyCredential,
    RUNTIME_READ_PERMISSION,
)
from runtime_api.server import make_server
from runtime_api.toolbusd_provider import (
    RemoteCliIpcClient,
    ToolbusdSnapshotProvider,
)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("用法: health_runtime_process_e2e.py <socket> <remote-cli>")
    socket_path, remote_cli = sys.argv[1:]
    provider = ToolbusdSnapshotProvider(
        RemoteCliIpcClient(socket_path, remote_cli, timeout_seconds=3.0),
        cache_ttl_ms=0)
    secret = "health-e2e-readonly-key-00000001"
    authenticator = ApiKeyAuthenticator([ApiKeyCredential(
        "health-e2e", secret, frozenset({RUNTIME_READ_PERMISSION}))])
    server = make_server("127.0.0.1", 0, provider,
                         authenticator=authenticator)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        base = f"http://127.0.0.1:{server.server_address[1]}/api/v1/health"
        public = json.loads(urlopen(base, timeout=5).read())
        assert public["data"] == {"status": "ok", "scope": "liveness"}

        request = Request(base, headers={"Authorization": "Bearer " + secret})
        first = json.loads(urlopen(request, timeout=10).read())["data"]
        health = first["toolbusd_health"]
        assert health["available"] is True
        assert health["reason"] is None
        snapshot = health["snapshot"]
        assert len(snapshot["daemon_instance_id"]) == 32
        assert snapshot["source"] == "toolbusd"
        assert snapshot["node_id"] == 0
        assert snapshot["producer_generation"] > 0
        assert snapshot["sample_sequence"] > 0
        metrics = {item["name"]: item for item in snapshot["metrics"]}
        assert metrics["request_queue_depth"]["availability"] == "available"
        assert metrics["tx_frame_total"] == {
            "metric_id": 15, "name": "tx_frame_total",
            "availability": "unavailable", "unit": "count", "value": None,
        }
        assert metrics["traffic_admitted_packet_total"]["value"] >= 0

        second = json.loads(urlopen(request, timeout=10).read())["data"][
            "toolbusd_health"]["snapshot"]
        assert second["daemon_instance_id"] == snapshot["daemon_instance_id"]
        assert second["producer_generation"] == snapshot["producer_generation"]
        assert second["sample_sequence"] > snapshot["sample_sequence"]
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

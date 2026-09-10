"""趋势持久化进程重启测试的最小子进程入口。"""

import json
import sys
from pathlib import Path

from runtime_api.dashboard import RuntimeDashboard
from runtime_api.provider import mock_snapshot
from runtime_api.trend_store import RuntimeTrendStore


directory = Path(sys.argv[1])
sequence = int(sys.argv[2])
snapshot = mock_snapshot()
snapshot["snapshot_id"] = f"process-{sequence}"
snapshot["captured_at_ms"] = sequence
snapshot["nodes"][0]["runtime"]["queue_depth"] = sequence * 10
dashboard = RuntimeDashboard(4, RuntimeTrendStore(directory, capacity=4))
view = dashboard.observe(snapshot)
print(json.dumps(view["nodes"][0]["trend"], sort_keys=True))

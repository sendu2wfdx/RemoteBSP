"""Runtime 快照来源；Provider 不包含任何总线访问逻辑。"""

from __future__ import annotations

import copy
import json
from abc import ABC, abstractmethod
from pathlib import Path

from .models import RuntimeContractError, normalize_snapshot


class RuntimeProviderError(RuntimeError):
    """快照来源暂时不可用或数据损坏。"""


class RuntimeProvider(ABC):
    """只读 Runtime 数据来源。"""

    @abstractmethod
    def get_snapshot(self) -> dict:
        """返回一份通过契约校验的独立快照。"""


class MockSnapshotProvider(RuntimeProvider):
    """用于开发、演示和测试的内存快照来源。"""

    def __init__(self, snapshot: dict | None = None):
        source = mock_snapshot() if snapshot is None else snapshot
        self._snapshot = normalize_snapshot(source)

    def get_snapshot(self) -> dict:
        return copy.deepcopy(self._snapshot)


class FileSnapshotProvider(RuntimeProvider):
    """每次请求重新读取原子替换的 JSON 快照文件。"""

    def __init__(self, path: Path, *, maximum_bytes: int = 1024 * 1024):
        if maximum_bytes < 1:
            raise ValueError("maximum_bytes必须大于0")
        self.path = path
        self.maximum_bytes = maximum_bytes

    def get_snapshot(self) -> dict:
        try:
            with self.path.open("rb") as stream:
                encoded = stream.read(self.maximum_bytes + 1)
            if len(encoded) > self.maximum_bytes:
                raise RuntimeProviderError(
                    f"快照文件超过{self.maximum_bytes}字节上限")
            raw = json.loads(encoded.decode("utf-8"))
            return normalize_snapshot(raw)
        except RuntimeProviderError:
            raise
        except (OSError, UnicodeDecodeError, json.JSONDecodeError,
                RuntimeContractError) as error:
            raise RuntimeProviderError(f"读取Runtime快照失败：{error}") from error


def mock_snapshot() -> dict:
    return {
        "schema_version": 1,
        "snapshot_id": "mock-1",
        "captured_at_ms": 0,
        "nodes": [{
            "node_id": "mock-node-1",
            "board_type": "mock-generic-v1",
            "display_name": "Mock 工具板 1",
            "state": "online",
            "last_seen_ms": 0,
            "links": [{"kind": "mock", "state": "online"}],
            "resources": [{
                "resource_id": "gpio-0",
                "kind": "gpio",
                "name": "状态输出",
                "available": True,
                "state": {"direction": "output", "value": False},
            }, {
                "resource_id": "motion-axis-0",
                "kind": "motion_axis",
                "name": "X 轴",
                "available": True,
                "state": {"enabled": False, "position_steps": 0},
            }],
            "runtime": {
                "uptime_ms": 0,
                "motion_state": "idle",
                "queue_depth": 0,
            },
        }],
        "alerts": [],
    }

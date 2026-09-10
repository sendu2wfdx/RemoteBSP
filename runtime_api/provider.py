"""Runtime 快照来源；Provider 不包含任何总线访问逻辑。"""

from __future__ import annotations

import copy
import json
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path

from .models import RuntimeContractError, normalize_snapshot


class RuntimeProviderError(RuntimeError):
    """快照来源暂时不可用或数据损坏。"""


class RuntimeProviderOperationError(RuntimeProviderError):
    """供 HTTP 层稳定映射的脱敏 Provider 操作错误。"""

    _CODE_CATEGORIES = {
        "deadline_exceeded": "timeout",
        "backend_unavailable": "transport",
        "protocol_incompatible": "protocol",
        "target_rejected": "target",
    }

    def __init__(self, code: str, *, category: str,
                 retryable: bool, possibly_committed: bool,
                 detail: str | None = None,
                 invalidates_global_operational: bool = False):
        expected_category = self._CODE_CATEGORIES.get(code)
        if expected_category is None:
            raise ValueError("未知Runtime Provider错误码")
        if category != expected_category:
            raise ValueError("Runtime Provider错误码与类别不匹配")
        if type(retryable) is not bool or type(possibly_committed) is not bool:
            raise ValueError("Runtime Provider错误标志必须是布尔值")
        if retryable and possibly_committed:
            raise ValueError("可能已提交的操作不得标记为可直接重试")
        if type(invalidates_global_operational) is not bool:
            raise ValueError("全局控制能力失效标志必须是布尔值")
        self.code = code
        self.category = category
        self.retryable = retryable
        self.possibly_committed = possibly_committed
        self.invalidates_global_operational = invalidates_global_operational
        # detail 仅供进程内诊断链使用，HTTP 层不得回显。
        self.detail = detail
        super().__init__(code)


@dataclass(frozen=True)
class SnapshotRead:
    """一次快照读取及其进程内新鲜度信息。"""

    snapshot: dict
    cache_status: str
    age_ms: int | None
    cache_ttl_ms: int | None


class RuntimeProvider(ABC):
    """只读 Runtime 数据来源。"""

    @abstractmethod
    def get_snapshot(self) -> dict:
        """返回一份通过契约校验的独立快照。"""

    def read_snapshot(self) -> SnapshotRead:
        """返回快照及可安全声明的新鲜度；默认来源不推算跨进程年龄。"""
        return SnapshotRead(
            snapshot=self.get_snapshot(),
            cache_status="disabled",
            age_ms=None,
            cache_ttl_ms=None,
        )

    def runtime_capabilities(self) -> dict:
        """返回不会随单次快照变化的只读 Runtime 能力。"""
        return {
            "clock_sync_quality": {
                "available": False,
                "source": "unavailable",
                "estimate_kind": "unavailable",
                "maximum_error_bound_ns": None,
                "maximum_sample_age_ms": None,
            },
        }


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

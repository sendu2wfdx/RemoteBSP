"""Runtime 普通用户运维状态的有界、脱敏进程内投影。"""

from __future__ import annotations

import threading
import time
from collections import deque
from typing import Callable

RUNTIME_OPERATIONS_SCHEMA_VERSION = 1
RUNTIME_VERSION = "0.2.0"


class RuntimeOperationalState:
    """只记录错误码和单调时间；不保留消息、URL、路径或请求内容。"""

    def __init__(self, *, error_capacity: int = 16,
                 monotonic_ns: Callable[[], int] = time.monotonic_ns):
        if type(error_capacity) is not int or not 1 <= error_capacity <= 64:
            raise ValueError("最近错误容量必须位于1～64")
        self.error_capacity = error_capacity
        self._clock = monotonic_ns
        self._started_ns = monotonic_ns()
        self._errors: deque[tuple[str, int]] = deque(maxlen=error_capacity)
        self._active_streams = 0
        self._lock = threading.Lock()

    def record_error(self, code: str) -> None:
        if not isinstance(code, str) or not 1 <= len(code) <= 64 or not all(
                char.isascii() and (char.isalnum() or char == "_")
                for char in code):
            code = "internal_error"
        with self._lock:
            self._errors.append((code, self._clock()))

    def stream_opened(self) -> None:
        with self._lock:
            self._active_streams += 1

    def stream_closed(self) -> None:
        with self._lock:
            self._active_streams = max(0, self._active_streams - 1)

    def snapshot(self, *, stream_limit: int, trend: dict,
                 toolbusd: dict) -> dict:
        now_ns = self._clock()
        with self._lock:
            errors = list(self._errors)
            active_streams = self._active_streams
        recent = [{"code": code,
                   "age_ms": max(0, (now_ns - observed_ns) // 1_000_000)}
                  for code, observed_ns in reversed(errors)]
        unknown_recording = {
            "availability": "unknown", "configured": None, "active": None,
            "event_count": None, "maximum_events": None,
            "maximum_file_bytes": None, "evidence_scope": None,
        }
        return {
            "schema_version": RUNTIME_OPERATIONS_SCHEMA_VERSION,
            "runtime": {"version": RUNTIME_VERSION,
                        "uptime_ms": max(0, (now_ns - self._started_ns) // 1_000_000),
                        "state": "running"},
            "toolbusd": toolbusd,
            "logical_recording": toolbusd.get(
                "logical_recording", unknown_recording),
            "trend_store": trend,
            "overview_stream": {
                "availability": "available",
                "active_connections": active_streams,
                "maximum_connections": stream_limit,
                "queue_capacity_per_connection": 1,
            },
            "recent_errors": {"capacity": self.error_capacity,
                              "count": len(recent), "items": recent},
        }

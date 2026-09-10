"""Runtime API 的有界、脱敏安全审计记录。"""

from __future__ import annotations

import queue
import re
import threading
import time
from collections import deque
from dataclasses import asdict, dataclass
from typing import Callable, Protocol


AUDIT_SCHEMA_VERSION = 1
DEFAULT_AUDIT_CAPACITY = 256
MAXIMUM_AUDIT_CAPACITY = 4096
_METHOD_CATEGORIES = frozenset({"read", "write", "options", "other"})
_PATH_CATEGORIES = frozenset({
    "root", "dashboard", "health", "snapshot", "overview", "nodes", "resources", "alerts", "events",
    "control_leases", "gpio_control", "control_operations", "unknown",
})
_REQUEST_ID_PATTERN = re.compile(r"[0-9a-f]{32}")
_KEY_ID_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
_RESULT_PATTERN = re.compile(r"[a-z][a-z0-9_]{0,63}")


@dataclass(frozen=True)
class SecurityAuditRecord:
    """不含密钥、请求头、原始路径或查询串的审计事件。"""

    schema_version: int
    occurred_at_ms: int
    request_id: str
    key_id: str | None
    method_category: str
    path_category: str
    result: str

    def to_dict(self) -> dict:
        return asdict(self)


class SecurityAuditSink(Protocol):
    def emit(self, record: SecurityAuditRecord) -> None:
        """接收一条已经脱敏的审计记录。"""


class BoundedAuditSink:
    """线程安全环形缓冲，通过有界队列异步转发到注入输出端。"""

    def __init__(self, capacity: int = DEFAULT_AUDIT_CAPACITY, *,
                 output: Callable[[dict], None] | None = None):
        if capacity < 1 or capacity > MAXIMUM_AUDIT_CAPACITY:
            raise ValueError(
                f"审计容量必须位于1～{MAXIMUM_AUDIT_CAPACITY}")
        if output is not None and not callable(output):
            raise ValueError("审计输出端必须可调用")
        self._records: deque[SecurityAuditRecord] = deque(maxlen=capacity)
        self._output = output
        self._lock = threading.Lock()
        self._overwritten_count = 0
        self._dropped_output_count = 0
        self._output_failure_count = 0
        self._stop_event = threading.Event()
        self._output_queue: queue.Queue[SecurityAuditRecord] | None = None
        self._worker: threading.Thread | None = None
        if output is not None:
            self._output_queue = queue.Queue(maxsize=capacity)
            self._worker = threading.Thread(
                target=self._run_output, name="runtime-audit-output",
                daemon=True)
            self._worker.start()

    def _run_output(self) -> None:
        assert self._output_queue is not None
        assert self._output is not None
        while not self._stop_event.is_set() or not self._output_queue.empty():
            try:
                record = self._output_queue.get(timeout=0.05)
            except queue.Empty:
                continue
            try:
                self._output(record.to_dict())
            except Exception:  # 审计输出失败不能破坏只读请求故障隔离。
                with self._lock:
                    self._output_failure_count += 1
            finally:
                self._output_queue.task_done()

    def emit(self, record: SecurityAuditRecord) -> None:
        if not isinstance(record, SecurityAuditRecord) or \
                record.schema_version != AUDIT_SCHEMA_VERSION or \
                type(record.occurred_at_ms) is not int or \
                record.occurred_at_ms < 0 or \
                not isinstance(record.request_id, str) or \
                _REQUEST_ID_PATTERN.fullmatch(record.request_id) is None or \
                (record.key_id is not None and
                 (not isinstance(record.key_id, str) or
                  _KEY_ID_PATTERN.fullmatch(record.key_id) is None)) or \
                not isinstance(record.method_category, str) or \
                record.method_category not in _METHOD_CATEGORIES or \
                not isinstance(record.path_category, str) or \
                record.path_category not in _PATH_CATEGORIES or \
                not isinstance(record.result, str) or \
                _RESULT_PATTERN.fullmatch(record.result) is None:
            raise ValueError("审计记录包含越界字段、未知版本或类别")
        with self._lock:
            if len(self._records) == self._records.maxlen:
                self._overwritten_count += 1
            self._records.append(record)
        if self._output_queue is not None:
            if self._stop_event.is_set():
                with self._lock:
                    self._dropped_output_count += 1
                return
            try:
                self._output_queue.put_nowait(record)
            except queue.Full:
                with self._lock:
                    self._dropped_output_count += 1

    def close(self, timeout_seconds: float = 1.0) -> bool:
        """停止接收输出并有界等待队列排空；返回线程是否已退出。"""
        if timeout_seconds < 0 or timeout_seconds > 5.0:
            raise ValueError("审计关闭等待必须位于0～5秒")
        self._stop_event.set()
        if self._worker is None:
            return True
        self._worker.join(timeout=timeout_seconds)
        return not self._worker.is_alive()

    def snapshot(self) -> tuple[SecurityAuditRecord, ...]:
        with self._lock:
            return tuple(self._records)

    @property
    def overwritten_count(self) -> int:
        with self._lock:
            return self._overwritten_count

    @property
    def output_failure_count(self) -> int:
        with self._lock:
            return self._output_failure_count

    @property
    def dropped_output_count(self) -> int:
        with self._lock:
            return self._dropped_output_count


def new_audit_record(*, request_id: str, key_id: str | None,
                     method_category: str, path_category: str,
                     result: str) -> SecurityAuditRecord:
    """只从受控枚举和已校验身份构建固定大小记录。"""
    if not isinstance(request_id, str) or \
            _REQUEST_ID_PATTERN.fullmatch(request_id) is None:
        raise ValueError("request_id格式无效")
    if key_id is not None and (
            not isinstance(key_id, str) or
            _KEY_ID_PATTERN.fullmatch(key_id) is None):
        raise ValueError("key_id格式无效")
    if method_category not in _METHOD_CATEGORIES or \
            path_category not in _PATH_CATEGORIES:
        raise ValueError("审计类别无效")
    if not isinstance(result, str) or _RESULT_PATTERN.fullmatch(result) is None:
        raise ValueError("审计结果格式无效")
    return SecurityAuditRecord(
        schema_version=AUDIT_SCHEMA_VERSION,
        occurred_at_ms=time.time_ns() // 1_000_000,
        request_id=request_id,
        key_id=key_id,
        method_category=method_category,
        path_category=path_category,
        result=result)

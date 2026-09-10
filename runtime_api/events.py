"""基于 RuntimeSnapshot 的版本化、有界增量事件日志。"""

from __future__ import annotations

import copy
import json
import re
import secrets
import threading
from collections import deque
from dataclasses import dataclass

from .event_store import (
    EVENT_STORE_KIND, EVENT_STORE_SCHEMA_VERSION, EventStoreError,
    RuntimeEventStore)


EVENT_SCHEMA_VERSION = 1
DEFAULT_EVENT_CAPACITY = 1024
MAXIMUM_EVENT_CAPACITY = 4096
DEFAULT_EVENT_PAGE_LIMIT = 50
MAXIMUM_EVENT_PAGE_LIMIT = 100
MAXIMUM_TRACKED_ENTITIES = 4096
MAXIMUM_TRACKED_STATE_BYTES = 2 * 1024 * 1024
MAXIMUM_EVENT_PAYLOAD_BYTES = 4096
MAXIMUM_EVENT_SEQUENCE = (1 << 63) - 1
_INCARNATION_PATTERN = re.compile(r"[0-9a-f]{32}")
_CURSOR_PATTERN = re.compile(
    r"e1:([0-9a-f]{32}):(0|[1-9][0-9]{0,18})")
_KIND_ORDER = {"node": 0, "resource": 1, "alert": 2, "clock_quality": 3}


class EventLogError(RuntimeError):
    """事件日志无法提供可信增量。"""


class InvalidEventCursor(EventLogError):
    """游标格式或位置无效。"""


class ExpiredEventCursor(EventLogError):
    """游标早于当前保留窗口。"""

    def __init__(self, reset_cursor: str):
        super().__init__("事件游标已超过保留窗口，请重新读取完整快照")
        self.reset_cursor = reset_cursor


@dataclass(frozen=True)
class _EntityState:
    encoded: bytes
    payload_size: int
    node_id: str | None
    resource_id: str | None
    alert_id: str | None
    payload: dict


@dataclass(frozen=True)
class RuntimeEvent:
    incarnation: str
    sequence: int
    snapshot_id: str
    captured_at_ms: int
    entity_type: str
    change: str
    node_id: str | None
    resource_id: str | None
    alert_id: str | None
    payload: dict | None
    payload_omitted: bool

    def to_dict(self) -> dict:
        return {
            "event_schema_version": EVENT_SCHEMA_VERSION,
            "sequence": self.sequence,
            "cursor": _encode_cursor(self.incarnation, self.sequence),
            "snapshot_id": self.snapshot_id,
            "captured_at_ms": self.captured_at_ms,
            "entity_type": self.entity_type,
            "change": self.change,
            "node_id": self.node_id,
            "resource_id": self.resource_id,
            "alert_id": self.alert_id,
            "payload": copy.deepcopy(self.payload),
            "payload_omitted": self.payload_omitted,
        }


@dataclass(frozen=True)
class EventPage:
    events: tuple[RuntimeEvent, ...]
    next_cursor: str
    has_more: bool


def _encode_cursor(incarnation: str, sequence: int) -> str:
    return f"e{EVENT_SCHEMA_VERSION}:{incarnation}:{sequence}"


def _decode_cursor(cursor: str) -> tuple[str, int]:
    match = _CURSOR_PATTERN.fullmatch(cursor) \
        if isinstance(cursor, str) else None
    if match is None:
        raise InvalidEventCursor("事件游标格式无效")
    sequence = int(match.group(2))
    if sequence > MAXIMUM_EVENT_SEQUENCE:
        raise InvalidEventCursor("事件游标超出范围")
    return match.group(1), sequence


def validate_event_cursor(cursor: str) -> None:
    """仅校验游标线格式；不读取或推进日志状态。"""
    _decode_cursor(cursor)


def _states_equal(
        left: dict[str, dict[object, _EntityState]],
        right: dict[str, dict[object, _EntityState]]) -> bool:
    """只比较会驱动事件的规范状态，忽略明确排除的年龄类字段。"""
    for kind in _KIND_ORDER:
        if left[kind].keys() != right[kind].keys():
            return False
        if any(left[kind][identity].encoded != right[kind][identity].encoded
               for identity in left[kind]):
            return False
    return True


def _state(payload: dict, *, node_id: str | None,
           resource_id: str | None = None,
           alert_id: str | None = None,
           canonical_payload: dict | None = None) -> _EntityState:
    canonical = payload if canonical_payload is None else canonical_payload
    encoded = json.dumps(canonical, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    payload_size = len(json.dumps(
        payload, ensure_ascii=False, separators=(",", ":"),
        allow_nan=False).encode("utf-8"))
    return _EntityState(encoded=encoded, payload_size=payload_size,
                        node_id=node_id,
                        resource_id=resource_id, alert_id=alert_id,
                        payload=copy.deepcopy(payload))


def _snapshot_states(snapshot: dict) -> dict[str, dict[object, _EntityState]]:
    states: dict[str, dict[object, _EntityState]] = {
        kind: {} for kind in _KIND_ORDER
    }
    for node in snapshot["nodes"]:
        node_id = node["node_id"]
        node_payload = {
            key: copy.deepcopy(node[key])
            for key in ("node_id", "board_type", "display_name", "state",
                        "links")
        }
        states["node"][node_id] = _state(node_payload, node_id=node_id)
        for resource in node["resources"]:
            resource_id = resource["resource_id"]
            states["resource"][(node_id, resource_id)] = _state(
                resource, node_id=node_id, resource_id=resource_id)
        clock = node["runtime"].get("clock_sync")
        if isinstance(clock, dict):
            canonical_clock = {
                key: value for key, value in clock.items()
                if key != "sample_age_ms"
            }
            states["clock_quality"][node_id] = _state(
                clock, node_id=node_id, canonical_payload=canonical_clock)
    for alert in snapshot["alerts"]:
        alert_id = alert["alert_id"]
        states["alert"][alert_id] = _state(
            alert, node_id=alert["node_id"],
            resource_id=alert["resource_id"], alert_id=alert_id)
    entity_count = sum(len(items) for items in states.values())
    state_bytes = sum(item.payload_size + len(item.encoded)
                      for items in states.values() for item in items.values())
    if entity_count > MAXIMUM_TRACKED_ENTITIES:
        raise EventLogError("事件跟踪实体数量超过上限")
    if state_bytes > MAXIMUM_TRACKED_STATE_BYTES:
        raise EventLogError("事件跟踪状态超过字节上限")
    return states


class RuntimeEventLog:
    """线程安全差分日志；观察快照和分页读取均为有限操作。"""

    def __init__(self, capacity: int = DEFAULT_EVENT_CAPACITY, *,
                 incarnation: str | None = None,
                 store: RuntimeEventStore | None = None):
        if capacity < 1 or capacity > MAXIMUM_EVENT_CAPACITY:
            raise ValueError(f"事件容量必须位于1～{MAXIMUM_EVENT_CAPACITY}")
        selected_incarnation = secrets.token_hex(16) \
            if incarnation is None else incarnation
        if not isinstance(selected_incarnation, str) or \
                _INCARNATION_PATTERN.fullmatch(selected_incarnation) is None:
            raise ValueError("事件日志incarnation必须是128位小写十六进制")
        self.capacity = capacity
        self.incarnation = selected_incarnation
        self._events: deque[RuntimeEvent] = deque(maxlen=capacity)
        self._states: dict[str, dict[object, _EntityState]] = {
            kind: {} for kind in _KIND_ORDER
        }
        self._next_sequence = 1
        self._last_captured_at_ms = -1
        self._last_snapshot_id: str | None = None
        self._last_error: str | None = None
        self._lock = threading.Lock()
        self._store = store
        self._baseline_snapshot: dict | None = None
        self.recovered_event_count = 0
        if store is not None:
            self._restore(store.load())

    def _restore(self, document: dict | None) -> None:
        if document is None:
            return
        try:
            next_sequence = document["next_sequence"]
            raw_events = document["events"]
            baseline = document["baseline_snapshot"]
            if type(next_sequence) is not int or not \
                    1 <= next_sequence <= MAXIMUM_EVENT_SEQUENCE + 1 or \
                    not isinstance(raw_events, list) or \
                    len(raw_events) > self.capacity or not isinstance(baseline, dict):
                raise ValueError("事件历史边界不合法")
            states = _snapshot_states(baseline)
            restored: list[RuntimeEvent] = []
            fields = {"sequence", "snapshot_id", "captured_at_ms",
                      "entity_type", "change", "node_id", "resource_id",
                      "alert_id", "payload", "payload_omitted"}
            for raw in raw_events:
                if not isinstance(raw, dict) or set(raw) != fields:
                    raise ValueError("事件历史记录字段不合法")
                sequence = raw["sequence"]
                if type(sequence) is not int or not 1 <= sequence < next_sequence or \
                        (restored and sequence <= restored[-1].sequence) or \
                        raw["entity_type"] not in _KIND_ORDER or \
                        raw["change"] not in {"added", "updated", "removed"} or \
                        type(raw["captured_at_ms"]) is not int or \
                        raw["captured_at_ms"] < 0 or \
                        not isinstance(raw["snapshot_id"], str) or \
                        not raw["snapshot_id"] or \
                        type(raw["payload_omitted"]) is not bool:
                    raise ValueError("事件历史记录值不合法")
                for name in ("node_id", "resource_id", "alert_id"):
                    if raw[name] is not None and not isinstance(raw[name], str):
                        raise ValueError("事件历史身份不合法")
                if raw["payload"] is not None and not isinstance(raw["payload"], dict):
                    raise ValueError("事件历史载荷不合法")
                restored.append(RuntimeEvent(
                    incarnation=self.incarnation, **raw))
            self._events.extend(restored)
            self._states = states
            self._next_sequence = next_sequence
            # captured_at_ms 属于进程内单调时间；新进程必须重新建立时间基线。
            self._last_captured_at_ms = -1
            self._last_snapshot_id = None
            self._baseline_snapshot = copy.deepcopy(baseline)
            self.recovered_event_count = len(restored)
        except (EventLogError, KeyError, TypeError, ValueError) as error:
            # 结构校验属于存储完整性的一部分；复用隔离流程且不载入半条历史。
            assert self._store is not None
            self._store.quarantine()
            self._events.clear()
            self._states = {kind: {} for kind in _KIND_ORDER}
            self._next_sequence = 1
            self._baseline_snapshot = None

    @staticmethod
    def _stored_event(event: RuntimeEvent) -> dict:
        value = event.to_dict()
        value.pop("event_schema_version")
        value.pop("cursor")
        return value

    def _store_document(self, events: list[RuntimeEvent], next_sequence: int,
                        baseline: dict) -> dict:
        return {
            "schema_version": EVENT_STORE_SCHEMA_VERSION,
            "kind": EVENT_STORE_KIND,
            "next_sequence": next_sequence,
            "baseline_snapshot": baseline,
            "events": [self._stored_event(event) for event in events],
        }

    def observe(self, snapshot: dict) -> bool:
        """原子观察一份快照；越界时保留既有游标窗口并标记不可用。"""
        try:
            captured_at_ms = snapshot["captured_at_ms"]
            if isinstance(captured_at_ms, bool) or \
                    not isinstance(captured_at_ms, int) or \
                    captured_at_ms < 0:
                raise ValueError("快照采集时间无效")
        except (KeyError, TypeError, ValueError) as error:
            with self._lock:
                self._last_error = str(error)
            return False

        # 先跳过已经明确过期的输入，避免其昂贵或异常内容污染新状态。
        with self._lock:
            if captured_at_ms < self._last_captured_at_ms:
                return True
        try:
            snapshot_id = snapshot["snapshot_id"]
            if not isinstance(snapshot_id, str) or not snapshot_id:
                raise ValueError("快照标识无效")
            states = _snapshot_states(snapshot)
        except (EventLogError, KeyError, TypeError, ValueError) as error:
            with self._lock:
                if captured_at_ms < self._last_captured_at_ms:
                    return True
                self._last_error = str(error)
            return False

        with self._lock:
            if captured_at_ms < self._last_captured_at_ms:
                return True
            if captured_at_ms == self._last_captured_at_ms:
                if snapshot_id != self._last_snapshot_id or \
                        not _states_equal(self._states, states):
                    self._last_error = (
                        "相同采集时间对应了不同快照，无法确定事件顺序")
                    return False
                return self._last_error is None
            pending: list[tuple[str, object, str, _EntityState | None]] = []
            for kind in sorted(_KIND_ORDER, key=_KIND_ORDER.get):
                before = self._states[kind]
                after = states[kind]
                for identity in sorted(set(before) | set(after)):
                    if identity not in after:
                        pending.append((kind, identity, "removed", before[identity]))
                    elif identity not in before:
                        pending.append((kind, identity, "added", after[identity]))
                    elif before[identity].encoded != after[identity].encoded:
                        pending.append((kind, identity, "updated", after[identity]))
            if len(pending) > MAXIMUM_EVENT_SEQUENCE - self._next_sequence + 1:
                self._last_error = "事件序号空间耗尽"
                return False
            new_events: list[RuntimeEvent] = []
            next_sequence = self._next_sequence
            for kind, _, change, item in pending:
                assert item is not None
                payload = None if change == "removed" else item.payload
                payload_omitted = payload is not None and \
                    item.payload_size > MAXIMUM_EVENT_PAYLOAD_BYTES
                event = RuntimeEvent(
                    incarnation=self.incarnation,
                    sequence=next_sequence,
                    snapshot_id=snapshot_id,
                    captured_at_ms=captured_at_ms,
                    entity_type=kind,
                    change=change,
                    node_id=item.node_id,
                    resource_id=item.resource_id,
                    alert_id=item.alert_id,
                    payload=None if payload_omitted else payload,
                    payload_omitted=payload_omitted)
                new_events.append(event)
                next_sequence += 1
            retained = (list(self._events) + new_events)[-self.capacity:]
            if self._store is not None:
                try:
                    self._store.save(self._store_document(
                        retained, next_sequence, snapshot))
                except EventStoreError as error:
                    self._last_error = str(error)
                    return False
            self._events.clear()
            self._events.extend(retained)
            self._next_sequence = next_sequence
            self._states = states
            self._last_captured_at_ms = captured_at_ms
            self._last_snapshot_id = snapshot_id
            self._baseline_snapshot = copy.deepcopy(snapshot)
            self._last_error = None
            return True

    def read_page(self, cursor: str | None, limit: int) -> EventPage:
        if limit < 1 or limit > MAXIMUM_EVENT_PAGE_LIMIT:
            raise ValueError(
                f"事件页大小必须位于1～{MAXIMUM_EVENT_PAGE_LIMIT}")
        decoded = None if cursor is None else _decode_cursor(cursor)
        with self._lock:
            latest = self._next_sequence - 1
            if decoded is not None and decoded[0] != self.incarnation:
                raise ExpiredEventCursor(
                    _encode_cursor(self.incarnation, latest))
            if self._last_error is not None:
                raise EventLogError(self._last_error)
            requested = None if decoded is None else decoded[1]
            earliest = self._events[0].sequence if self._events else latest + 1
            if requested is None:
                return EventPage(
                    events=(),
                    next_cursor=_encode_cursor(self.incarnation, latest),
                    has_more=False)
            if requested < earliest - 1:
                raise ExpiredEventCursor(
                    _encode_cursor(self.incarnation, latest))
            if requested > latest:
                raise InvalidEventCursor("事件游标领先于服务端")
            available = [event for event in self._events
                         if event.sequence > requested]
            page = tuple(available[:limit])
            next_sequence = page[-1].sequence if page else requested
            return EventPage(
                events=page,
                next_cursor=_encode_cursor(self.incarnation, next_sequence),
                has_more=len(available) > len(page))

    def validate_cursor_incarnation(self, cursor: str) -> None:
        """在读取 Provider 前拒绝必然属于其它日志实例的游标。"""
        incarnation, _ = _decode_cursor(cursor)
        with self._lock:
            if incarnation != self.incarnation:
                earliest = self._events[0].sequence if self._events else \
                    self._next_sequence
                raise ExpiredEventCursor(
                    _encode_cursor(self.incarnation, earliest - 1))

    @property
    def latest_cursor(self) -> str:
        with self._lock:
            return _encode_cursor(self.incarnation, self._next_sequence - 1)

    @property
    def restart_reset_cursor(self) -> str:
        """新进程基线；明确触发409后可从恢复窗口最早位置重读。"""
        with self._lock:
            earliest = self._events[0].sequence if self._events else \
                self._next_sequence
            return _encode_cursor(self.incarnation, earliest - 1)

    @property
    def persistence_status(self) -> dict:
        return {
            "enabled": self._store is not None,
            "load_status": ("disabled" if self._store is None
                            else self._store.load_status),
            "recovered_events": self.recovered_event_count,
        }

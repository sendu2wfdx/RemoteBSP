"""Runtime 写控制的有界、进程内短时租约内核。"""

from __future__ import annotations

import re
import secrets
import threading
import time
from dataclasses import asdict, dataclass
from typing import Callable


CONTROL_LEASE_SCHEMA_VERSION = 1
DEFAULT_CONTROL_LEASE_CAPACITY = 256
MAXIMUM_CONTROL_LEASE_CAPACITY = 4096
MINIMUM_CONTROL_LEASE_TTL_MS = 100
MAXIMUM_CONTROL_LEASE_TTL_MS = 30_000
CONTROL_LEASE_IDEMPOTENCY_RETENTION_MS = 30_000
CONTROL_LEASE_HISTORY_MULTIPLIER = 4
MAXIMUM_CONTROL_ID_BYTES = 128

_ID_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}")
_IDEMPOTENCY_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,63}")
_LEASE_ID_PATTERN = re.compile(r"[0-9a-f]{32}")


class ControlLeaseError(RuntimeError):
    """控制租约操作失败。"""


class ControlLeaseConflict(ControlLeaseError):
    """目标资源已经由活动租约占用。"""


class ControlLeaseCapacityExceeded(ControlLeaseError):
    """活动租约达到固定上限。"""


class ControlLeaseNotFound(ControlLeaseError):
    """租约不存在或已经过期。"""


class ControlLeaseOwnershipError(ControlLeaseError):
    """调用身份不是租约所有者。"""


@dataclass(frozen=True)
class ControlLease:
    """不包含 API 密钥的控制租约公开视图。"""

    schema_version: int
    lease_id: str
    owner_key_id: str
    node_id: str
    resource_id: str
    command_group: str
    acquired_at_ms: int
    expires_at_ms: int
    ttl_ms: int

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass
class _ActiveLease:
    lease: ControlLease
    idempotency_key: str
    deadline_ns: int


@dataclass(frozen=True)
class _CompletedLease:
    lease: ControlLease
    idempotency_key: str
    retain_until_ns: int


def validate_control_id(value: object, field: str) -> str:
    if not isinstance(value, str) or _ID_PATTERN.fullmatch(value) is None or \
            len(value.encode("ascii")) > MAXIMUM_CONTROL_ID_BYTES:
        raise ValueError(
            f"{field}必须是1～{MAXIMUM_CONTROL_ID_BYTES}字节的规范ASCII标识")
    return value


def validate_idempotency_key(value: object) -> str:
    if not isinstance(value, str) or \
            _IDEMPOTENCY_PATTERN.fullmatch(value) is None:
        raise ValueError("idempotency_key必须是1～64字节的规范ASCII标识")
    return value


def validate_ttl_ms(value: object) -> int:
    if type(value) is not int or not MINIMUM_CONTROL_LEASE_TTL_MS <= value <= \
            MAXIMUM_CONTROL_LEASE_TTL_MS:
        raise ValueError(
            f"ttl_ms必须是{MINIMUM_CONTROL_LEASE_TTL_MS}～"
            f"{MAXIMUM_CONTROL_LEASE_TTL_MS}的整数")
    return value


def validate_lease_id(value: object) -> str:
    if not isinstance(value, str) or _LEASE_ID_PATTERN.fullmatch(value) is None:
        raise ValueError("lease_id格式无效")
    return value


class ControlLeaseManager:
    """按资源和命令组互斥的线程安全短租约表。"""

    def __init__(self, capacity: int = DEFAULT_CONTROL_LEASE_CAPACITY, *,
                 monotonic_ns: Callable[[], int] = time.monotonic_ns,
                 wall_time_ms: Callable[[], int] =
                 lambda: time.time_ns() // 1_000_000,
                 lease_id_factory: Callable[[], str] =
                 lambda: secrets.token_hex(16)):
        if type(capacity) is not int or not 1 <= capacity <= \
                MAXIMUM_CONTROL_LEASE_CAPACITY:
            raise ValueError(
                f"控制租约容量必须位于1～{MAXIMUM_CONTROL_LEASE_CAPACITY}")
        self._capacity = capacity
        self._monotonic_ns = monotonic_ns
        self._wall_time_ms = wall_time_ms
        self._lease_id_factory = lease_id_factory
        self._by_id: dict[str, _ActiveLease] = {}
        self._by_scope: dict[tuple[str, str], str] = {}
        self._by_idempotency: dict[tuple[str, str], str] = {}
        self._completed_by_idempotency: dict[
            tuple[str, str], _CompletedLease] = {}
        self._completed_by_id: dict[str, _CompletedLease] = {}
        self._history_capacity = min(
            capacity * CONTROL_LEASE_HISTORY_MULTIPLIER,
            MAXIMUM_CONTROL_LEASE_CAPACITY)
        self._lock = threading.Lock()

    @property
    def capacity(self) -> int:
        return self._capacity

    def _expire_locked(self, now_ns: int) -> None:
        completed_expired = [
            idempotency for idempotency, completed
            in self._completed_by_idempotency.items()
            if completed.retain_until_ns <= now_ns
        ]
        for idempotency in completed_expired:
            completed = self._completed_by_idempotency.pop(idempotency)
            self._completed_by_id.pop(completed.lease.lease_id, None)
        expired = [lease_id for lease_id, active in self._by_id.items()
                   if active.deadline_ns <= now_ns]
        for lease_id in expired:
            self._remove_locked(lease_id, now_ns)

    def _remove_locked(self, lease_id: str, now_ns: int) -> ControlLease:
        active = self._by_id.pop(lease_id)
        lease = active.lease
        self._by_scope.pop((lease.node_id, lease.resource_id), None)
        idempotency = (lease.owner_key_id, active.idempotency_key)
        self._by_idempotency.pop(idempotency, None)
        completed = _CompletedLease(
            lease=lease, idempotency_key=active.idempotency_key,
            retain_until_ns=now_ns +
            CONTROL_LEASE_IDEMPOTENCY_RETENTION_MS * 1_000_000)
        self._completed_by_idempotency[idempotency] = completed
        self._completed_by_id[lease_id] = completed
        return lease

    def acquire(self, *, owner_key_id: str, node_id: str, resource_id: str,
                command_group: str, ttl_ms: int,
                idempotency_key: str) -> tuple[ControlLease, bool]:
        """申请租约；相同身份和幂等键重试返回原租约且不延长时间。"""
        owner_key_id = validate_control_id(owner_key_id, "owner_key_id")
        node_id = validate_control_id(node_id, "node_id")
        resource_id = validate_control_id(resource_id, "resource_id")
        command_group = validate_control_id(command_group, "command_group")
        ttl_ms = validate_ttl_ms(ttl_ms)
        idempotency_key = validate_idempotency_key(idempotency_key)
        # 同一资源的不同命令组仍共享一个排他域，调用者不能通过改名绕过互斥。
        scope = (node_id, resource_id)
        idempotency = (owner_key_id, idempotency_key)
        with self._lock:
            # 必须在取得互斥锁后取时钟；排队时间不能侵蚀新租约的有效期，
            # 更不能让接口成功返回一个已经过期的租约。
            now_ns = self._monotonic_ns()
            self._expire_locked(now_ns)
            existing_id = self._by_idempotency.get(idempotency)
            if existing_id is not None:
                existing = self._by_id[existing_id].lease
                if (existing.node_id, existing.resource_id) != scope or \
                        existing.command_group != command_group or \
                        existing.ttl_ms != ttl_ms:
                    raise ControlLeaseConflict(
                        "同一幂等键已用于不同的租约参数")
                return existing, True
            completed = self._completed_by_idempotency.get(idempotency)
            if completed is not None:
                existing = completed.lease
                if (existing.node_id, existing.resource_id) != scope or \
                        existing.command_group != command_group or \
                        existing.ttl_ms != ttl_ms:
                    raise ControlLeaseConflict(
                        "同一幂等键已用于不同的租约参数")
                raise ControlLeaseConflict(
                    "幂等键对应的租约已经结束，必须使用新的幂等键")
            if scope in self._by_scope:
                raise ControlLeaseConflict("目标控制范围已有活动租约")
            if len(self._by_id) >= self._capacity:
                raise ControlLeaseCapacityExceeded("活动控制租约已达到上限")
            if len(self._by_id) + len(self._completed_by_id) >= \
                    self._history_capacity:
                raise ControlLeaseCapacityExceeded(
                    "控制租约幂等历史已达到上限")
            lease_id = self._lease_id_factory()
            validate_lease_id(lease_id)
            if lease_id in self._by_id or lease_id in self._completed_by_id:
                raise ControlLeaseError("租约ID生成器返回重复值")
            acquired_at_ms = self._wall_time_ms()
            lease = ControlLease(
                schema_version=CONTROL_LEASE_SCHEMA_VERSION,
                lease_id=lease_id,
                owner_key_id=owner_key_id,
                node_id=node_id,
                resource_id=resource_id,
                command_group=command_group,
                acquired_at_ms=acquired_at_ms,
                expires_at_ms=acquired_at_ms + ttl_ms,
                ttl_ms=ttl_ms)
            self._by_id[lease_id] = _ActiveLease(
                lease=lease, idempotency_key=idempotency_key,
                deadline_ns=now_ns + ttl_ms * 1_000_000)
            self._by_scope[scope] = lease_id
            self._by_idempotency[idempotency] = lease_id
            return lease, False

    def release(self, lease_id: str, *, requester_key_id: str,
                allow_foreign: bool = False) -> ControlLease:
        lease_id = validate_lease_id(lease_id)
        requester_key_id = validate_control_id(
            requester_key_id, "requester_key_id")
        with self._lock:
            now_ns = self._monotonic_ns()
            self._expire_locked(now_ns)
            active = self._by_id.get(lease_id)
            if active is None:
                raise ControlLeaseNotFound("控制租约不存在或已经过期")
            if active.lease.owner_key_id != requester_key_id and \
                    not allow_foreign:
                raise ControlLeaseOwnershipError("不能释放其他身份的控制租约")
            return self._remove_locked(lease_id, now_ns)

    def active_count(self) -> int:
        with self._lock:
            now_ns = self._monotonic_ns()
            self._expire_locked(now_ns)
            return len(self._by_id)

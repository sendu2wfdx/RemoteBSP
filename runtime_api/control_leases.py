"""Runtime 写控制的有界、进程内短时租约内核。"""

from __future__ import annotations

import re
import secrets
import threading
import time
from dataclasses import asdict, dataclass
from typing import Callable

from .deadline import (
    MonotonicDeadline,
    RequestDeadlineExceeded,
    call_with_deadline,
)


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

    def authorize(self, lease_id: str, *, requester_key_id: str,
                  allow_foreign: bool = False) -> ControlLease:
        """只读取并校验活动租约；不会续租或改变终态历史。"""
        lease_id = validate_lease_id(lease_id)
        requester_key_id = validate_control_id(
            requester_key_id, "requester_key_id")
        with self._lock:
            self._expire_locked(self._monotonic_ns())
            active = self._by_id.get(lease_id)
            if active is None:
                raise ControlLeaseNotFound("控制租约不存在或已经过期")
            if active.lease.owner_key_id != requester_key_id and \
                    not allow_foreign:
                raise ControlLeaseOwnershipError("不能使用其他身份的控制租约")
            return active.lease

    def remaining_ttl_ms(self, lease_id: str, *,
                         requester_key_id: str) -> int:
        """返回向下游登记时可使用的、不超过本地期限的整毫秒 TTL。"""
        lease_id = validate_lease_id(lease_id)
        requester_key_id = validate_control_id(
            requester_key_id, "requester_key_id")
        with self._lock:
            now_ns = self._monotonic_ns()
            self._expire_locked(now_ns)
            active = self._by_id.get(lease_id)
            if active is None:
                raise ControlLeaseNotFound("控制租约不存在或已经过期")
            if active.lease.owner_key_id != requester_key_id:
                raise ControlLeaseOwnershipError("不能使用其他身份的控制租约")
            remaining = (active.deadline_ns - now_ns) // 1_000_000
            if remaining < 1:
                raise ControlLeaseNotFound("控制租约剩余时间不足1毫秒")
            return min(remaining, MAXIMUM_CONTROL_LEASE_TTL_MS)

    def rollback_acquire(self, lease_id: str, *, owner_key_id: str) -> None:
        """仅撤销一次未完成的下游登记，不生成会阻止安全重试的终态。"""
        lease_id = validate_lease_id(lease_id)
        owner_key_id = validate_control_id(owner_key_id, "owner_key_id")
        with self._lock:
            active = self._by_id.get(lease_id)
            if active is None or active.lease.owner_key_id != owner_key_id:
                return
            lease = active.lease
            self._by_id.pop(lease_id, None)
            self._by_scope.pop((lease.node_id, lease.resource_id), None)
            self._by_idempotency.pop(
                (lease.owner_key_id, active.idempotency_key), None)

    def active_count(self) -> int:
        with self._lock:
            now_ns = self._monotonic_ns()
            self._expire_locked(now_ns)
            return len(self._by_id)

    def invalidate_all(self) -> None:
        """立即撤销当前世代的全部活动租约和幂等历史。"""
        with self._lock:
            self._by_id.clear()
            self._by_scope.clear()
            self._by_idempotency.clear()
            self._completed_by_idempotency.clear()
            self._completed_by_id.clear()


class DaemonBoundControlLeaseManager:
    """把进程内租约绑定到 toolbusd 实例身份。

    同一时刻只进行一次身份读取，并发请求共享该次结果；租约变更仍在身份锁内
    串行提交。这样既不会倒序应用身份，也不会在 daemon 故障时串行累积外部超时。
    身份不可读时立即清空租约并失败关闭。
    """

    def __init__(self, identity_reader: Callable[[], str],
                 capacity: int = DEFAULT_CONTROL_LEASE_CAPACITY, *,
                 manager: ControlLeaseManager | None = None):
        if not callable(identity_reader):
            raise ValueError("toolbusd实例身份读取器必须可调用")
        if manager is not None and capacity != DEFAULT_CONTROL_LEASE_CAPACITY:
            raise ValueError("不能同时注入租约管理器和非默认容量")
        self._identity_reader = identity_reader
        self._manager = manager or ControlLeaseManager(capacity)
        self._daemon_instance_id: str | None = None
        self._binding_condition = threading.Condition()
        self._identity_refreshing = False
        self._identity_refresh_generation = 0
        self._identity_completed_generation = 0
        self._identity_invalidated_generation = 0
        self._identity_refresh_error: str | None = None
        self._identity_refresh_valid = False

    @property
    def capacity(self) -> int:
        return self._manager.capacity

    @property
    def daemon_instance_id(self) -> str | None:
        with self._binding_condition:
            return self._daemon_instance_id

    def matches_current_daemon(
            self, expected_instance_id: str, *,
            deadline: MonotonicDeadline | None = None) -> bool:
        """重新读取 daemon 身份，并判断是否仍为已准入的同一实例。

        身份不可读时沿用租约管理器的失败关闭语义；身份换代时
        ``_with_current_identity`` 会先原子撤销旧租约，再返回 False。
        """
        if not isinstance(expected_instance_id, str) or \
                re.fullmatch(r"[0-9a-f]{32}", expected_instance_id) is None or \
                expected_instance_id == "0" * 32:
            raise ValueError("待核对的toolbusd实例身份无效")
        return bool(self._with_current_identity(
            lambda: self._daemon_instance_id == expected_instance_id,
            deadline=deadline))

    def _read_identity(
            self, deadline: MonotonicDeadline | None = None) -> str:
        try:
            identity = call_with_deadline(
                self._identity_reader, deadline=deadline)
        except RequestDeadlineExceeded:
            raise
        except Exception as error:
            if deadline is not None:
                deadline.check()
            raise ControlLeaseError(
                f"无法确认toolbusd实例身份：{error}") from error
        if not isinstance(identity, str) or \
                re.fullmatch(r"[0-9a-f]{32}", identity) is None or \
                identity == "0" * 32:
            raise ControlLeaseError("toolbusd返回了无效的实例身份")
        return identity

    def _with_current_identity(
            self, operation: Callable[[], object], *,
            deadline: MonotonicDeadline | None = None):
        while True:
            with self._binding_condition:
                if self._identity_refreshing:
                    generation = self._identity_refresh_generation
                    while self._identity_completed_generation < generation:
                        timeout = None if deadline is None else \
                            deadline.remaining_seconds()
                        self._binding_condition.wait(timeout)
                        if deadline is not None:
                            deadline.check()
                    if self._identity_refresh_error is not None:
                        raise ControlLeaseError(self._identity_refresh_error)
                    if self._identity_refresh_valid:
                        return operation()
                    # 刷新者仅因自己的请求期限取消；没有产生共享身份证据。
                    # 当前等待者用自己的剩余预算重新竞争一次刷新。
                    continue
                self._identity_refreshing = True
                self._identity_refresh_generation += 1
                generation = self._identity_refresh_generation
                break
        try:
            identity = self._read_identity(deadline)
            error_message = None
        except RequestDeadlineExceeded:
            with self._binding_condition:
                self._identity_refreshing = False
                self._identity_completed_generation = generation
                self._identity_refresh_error = None
                self._identity_refresh_valid = False
                self._binding_condition.notify_all()
            # 请求取消不是 daemon 失败证据，不得撤销其他请求的租约或能力。
            raise
        except ControlLeaseError as error:
            identity = None
            error_message = str(error)
        with self._binding_condition:
            self._identity_refreshing = False
            self._identity_completed_generation = generation
            if generation <= self._identity_invalidated_generation:
                error_message = "toolbusd实例身份读取已被显式作废"
                identity = None
            self._identity_refresh_error = error_message
            self._identity_refresh_valid = error_message is None
            if error_message is not None:
                self._manager.invalidate_all()
                self._daemon_instance_id = None
                self._binding_condition.notify_all()
                raise ControlLeaseError(error_message)
            if identity != self._daemon_instance_id:
                self._manager.invalidate_all()
                self._daemon_instance_id = identity
            self._binding_condition.notify_all()
            return operation()

    def acquire(self, *, deadline: MonotonicDeadline | None = None,
                **arguments) -> tuple[ControlLease, bool]:
        return self._with_current_identity(
            lambda: self._manager.acquire(**arguments), deadline=deadline)

    def release(self, lease_id: str, *, requester_key_id: str,
                allow_foreign: bool = False,
                deadline: MonotonicDeadline | None = None) -> ControlLease:
        return self._with_current_identity(
            lambda: self._manager.release(
                lease_id, requester_key_id=requester_key_id,
                allow_foreign=allow_foreign), deadline=deadline)

    def authorize(self, lease_id: str, *, requester_key_id: str,
                  allow_foreign: bool = False,
                  deadline: MonotonicDeadline | None = None) -> ControlLease:
        return self._with_current_identity(
            lambda: self._manager.authorize(
                lease_id, requester_key_id=requester_key_id,
                allow_foreign=allow_foreign), deadline=deadline)

    def remaining_ttl_ms(self, lease_id: str, *,
                         requester_key_id: str,
                         deadline: MonotonicDeadline | None = None) -> int:
        return self._with_current_identity(
            lambda: self._manager.remaining_ttl_ms(
                lease_id, requester_key_id=requester_key_id),
            deadline=deadline)

    def rollback_acquire(self, lease_id: str, *, owner_key_id: str) -> None:
        # 普通目标或下游登记失败只回滚本次租约；不把无关租约和幂等历史清空。
        with self._binding_condition:
            self._manager.rollback_acquire(
                lease_id, owner_key_id=owner_key_id)

    def active_count(
            self, *, deadline: MonotonicDeadline | None = None) -> int:
        return self._with_current_identity(
            self._manager.active_count, deadline=deadline)

    def invalidate_all(self) -> None:
        with self._binding_condition:
            self._identity_invalidated_generation = \
                self._identity_refresh_generation
            self._manager.invalidate_all()
            self._daemon_instance_id = None

#!/usr/bin/env python3
"""RemoteBSP 状态读取与短时控制租约 HTTP API。"""

from __future__ import annotations

import argparse
import ipaddress
import json
import re
import secrets
import socket
import sys
import threading
import time
import queue
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable
from urllib.parse import ParseResult, parse_qsl, unquote, urlparse

from .auth import (
    ALERT_RULE_WRITE_PERMISSION,
    CONTROL_OPERATION_READ_PERMISSION,
    CONTROL_LEASE_ACQUIRE_PERMISSION,
    CONTROL_LEASE_RELEASE_PERMISSION,
    CONTROL_LEASE_REVOKE_PERMISSION,
    GPIO_WRITE_PERMISSION,
    MAXIMUM_API_KEY_BYTES,
    RUNTIME_READ_PERMISSION,
    ApiKeyAuthenticator,
    AuthenticatedPrincipal,
    AuthConfigurationError,
    load_api_key_authenticator,
)
from .alert_rules import (
    AlertRuleError, AlertRuleManager, AlertRuleStorageError, AlertRuleStore)
from .control_leases import (
    CONTROL_LEASE_SCHEMA_VERSION,
    DEFAULT_CONTROL_LEASE_CAPACITY,
    MAXIMUM_CONTROL_LEASE_CAPACITY,
    ControlLeaseCapacityExceeded,
    ControlLeaseConflict,
    ControlLeaseError,
    ControlLeaseManager,
    DaemonBoundControlLeaseManager,
    ControlLeaseNotFound,
    ControlLeaseOwnershipError,
    validate_control_id,
    validate_lease_id,
    validate_ttl_ms,
    validate_idempotency_key,
)
from .control_audit_journal import (
    CONTROL_AUDIT_SCHEMA_VERSION,
    ControlAuditError,
    ControlAuditJournal,
    ControlAuditRecord,
)
from .audit import (
    BoundedAuditSink,
    DEFAULT_AUDIT_CAPACITY,
    new_audit_record,
)
from .events import (
    DEFAULT_EVENT_CAPACITY,
    DEFAULT_EVENT_PAGE_LIMIT,
    EVENT_SCHEMA_VERSION,
    MAXIMUM_EVENT_CAPACITY,
    MAXIMUM_EVENT_PAGE_LIMIT,
    EventLogError,
    ExpiredEventCursor,
    InvalidEventCursor,
    RuntimeEventLog,
    validate_event_cursor,
)
from .models import RUNTIME_SNAPSHOT_SCHEMA_VERSION
from .deadline import (
    MonotonicDeadline,
    RequestDeadlineExceeded,
    call_with_deadline,
)
from .provider import (
    FileSnapshotProvider,
    MockSnapshotProvider,
    RuntimeProvider,
    RuntimeProviderError,
    RuntimeProviderOperationError,
    SnapshotRead,
)
from .toolbusd_provider import RemoteCliIpcClient, ToolbusdSnapshotProvider
from .dashboard import RuntimeDashboard
from .trend_store import (
    DEFAULT_MAXIMUM_BYTES as DEFAULT_TREND_STORE_MAXIMUM_BYTES,
    RuntimeTrendStore,
    TrendStoreError,
)
from .operations import RuntimeOperationalState


API_VERSION = "v1"
_DASHBOARD_ASSETS = {
    "/": ("dashboard.html", "text/html; charset=utf-8"),
    "/dashboard": ("dashboard.html", "text/html; charset=utf-8"),
    "/dashboard.css": ("dashboard.css", "text/css; charset=utf-8"),
    "/dashboard.js": ("dashboard.js", "text/javascript; charset=utf-8"),
}
GPIO_WRITE_COMMAND_GROUP = "gpio.write"
MAXIMUM_CONTROL_REQUEST_BYTES = 4096
_OPERATION_ID_PATTERN = re.compile(r"[0-9a-f]{64}")
_OPERATION_KINDS = {"gpio_write", "control_release"}
DEFAULT_REQUEST_IO_TIMEOUT_SECONDS = 5.0
MINIMUM_REQUEST_IO_TIMEOUT_SECONDS = 0.1
MAXIMUM_REQUEST_IO_TIMEOUT_SECONDS = 30.0
_FOREIGN_RECOVERY_LOCATOR_CAPACITY = 256
_FOREIGN_RECOVERY_LOCATOR_RETENTION_NS = 24 * 60 * 60 * 1_000_000_000
_FOREIGN_RECOVERY_RESERVATIONS_PER_LOCATOR = 256
_QUERY_CREDENTIAL_NAMES = {
    "api_key", "api-key", "apikey", "x-api-key", "access_token", "token",
}
DEFAULT_OVERVIEW_STREAM_CONNECTIONS = 8
MAXIMUM_OVERVIEW_STREAM_CONNECTIONS = 32
DEFAULT_OVERVIEW_STREAM_INTERVAL_SECONDS = 2.0
MINIMUM_OVERVIEW_STREAM_INTERVAL_SECONDS = 0.1
MAXIMUM_OVERVIEW_STREAM_INTERVAL_SECONDS = 30.0
OVERVIEW_STREAM_QUEUE_CAPACITY = 1
OVERVIEW_STREAM_HEARTBEAT_SECONDS = 10.0
MAXIMUM_OVERVIEW_STREAM_EVENT_BYTES = 1024 * 1024


def _strict_json_object(pairs: list[tuple[str, object]]) -> dict:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"请求体包含重复字段：{key}")
        value[key] = item
    return value


@dataclass
class _ForeignOperationRecoveryEntry:
    owner_key_id: str
    operation_id: str | None
    expires_at_ns: int
    reservations: set[str] = field(default_factory=set)
    retained_for_lookup: bool = False


class _ForeignOperationRecoveryIndex:
    """有界保存管理员代释放所需的真实 owner，不把 owner 交给客户端。"""

    def __init__(self, capacity: int = _FOREIGN_RECOVERY_LOCATOR_CAPACITY,
                 *, monotonic_ns: Callable[[], int] = time.monotonic_ns):
        if capacity < 1:
            raise ValueError("管理员恢复定位容量必须大于零")
        self.capacity = capacity
        self._monotonic_ns = monotonic_ns
        self._lock = threading.Lock()
        self._entries: dict[
            tuple[str, str, str, str], _ForeignOperationRecoveryEntry] = {}

    def _purge_expired_locked(self, now_ns: int) -> None:
        expired = [key for key, entry in self._entries.items()
                   if entry.expires_at_ns <= now_ns]
        for key in expired:
            self._entries.pop(key, None)

    @staticmethod
    def _key(requester_key_id: str, kind: str, lease_id: str,
             idempotency_key: str) -> tuple[str, str, str, str]:
        return requester_key_id, kind, lease_id, idempotency_key

    def reserve(self, requester_key_id: str, owner_key_id: str, *, kind: str,
                lease_id: str,
                idempotency_key: str) -> tuple[str | None, str | None]:
        """在下游写前预留；返回独立随机token及可选错误。"""
        key = self._key(requester_key_id, kind, lease_id, idempotency_key)
        now_ns = self._monotonic_ns()
        with self._lock:
            self._purge_expired_locked(now_ns)
            existing = self._entries.get(key)
            if existing is not None and existing.owner_key_id != owner_key_id:
                return None, "conflict"
            if existing is None and len(self._entries) >= self.capacity:
                return None, "capacity"
            if existing is not None and len(existing.reservations) >= \
                    _FOREIGN_RECOVERY_RESERVATIONS_PER_LOCATOR:
                return None, "capacity"
            deadline_ns = min(
                (1 << 63) - 1,
                now_ns + _FOREIGN_RECOVERY_LOCATOR_RETENTION_NS)
            if existing is None:
                existing = _ForeignOperationRecoveryEntry(
                    owner_key_id, None, deadline_ns)
                self._entries[key] = existing
            else:
                existing.expires_at_ns = deadline_ns
            token = secrets.token_hex(16)
            while token in existing.reservations:
                token = secrets.token_hex(16)
            existing.reservations.add(token)
            return token, None

    def forget(self, requester_key_id: str, *, kind: str, lease_id: str,
               idempotency_key: str, reservation_token: str) -> None:
        key = self._key(requester_key_id, kind, lease_id, idempotency_key)
        with self._lock:
            existing = self._entries.get(key)
            if existing is None or \
                    reservation_token not in existing.reservations:
                return
            existing.reservations.discard(reservation_token)
            # 一旦有operation绑定，这条恢复证据不再属于任何单次请求，
            # 后到请求的确定失败不得把它删除。
            if existing.operation_id is None and \
                    not existing.retained_for_lookup and \
                    not existing.reservations:
                self._entries.pop(key, None)

    def retain_for_lookup(self, requester_key_id: str, *, kind: str,
                          lease_id: str, idempotency_key: str,
                          reservation_token: str) -> bool:
        """不确定响应前消费本次token，仅留下一个有界locator占位。"""
        key = self._key(requester_key_id, kind, lease_id, idempotency_key)
        now_ns = self._monotonic_ns()
        with self._lock:
            self._purge_expired_locked(now_ns)
            existing = self._entries.get(key)
            if existing is None or \
                    reservation_token not in existing.reservations:
                return False
            existing.reservations.discard(reservation_token)
            existing.retained_for_lookup = True
            return True

    def bind_operation(self, requester_key_id: str, operation_id: str, *,
                       kind: str, lease_id: str,
                       idempotency_key: str,
                       reservation_token: str | None = None) -> bool:
        key = self._key(requester_key_id, kind, lease_id, idempotency_key)
        now_ns = self._monotonic_ns()
        with self._lock:
            self._purge_expired_locked(now_ns)
            existing = self._entries.get(key)
            if existing is None or (reservation_token is not None and
                                    reservation_token not in
                                    existing.reservations):
                return False
            if existing.operation_id is not None and \
                    existing.operation_id != operation_id:
                return False
            existing.operation_id = operation_id
            # operation ID一旦确定，其他并发请求的临时token均不再需要；
            # 后到forget也无法删除已经绑定的稳定映射。
            existing.reservations.clear()
            existing.retained_for_lookup = True
            return True

    def owner_for_selector(self, requester_key_id: str, *, kind: str,
                           lease_id: str,
                           idempotency_key: str) -> str | None:
        key = self._key(requester_key_id, kind, lease_id, idempotency_key)
        now_ns = self._monotonic_ns()
        with self._lock:
            self._purge_expired_locked(now_ns)
            entry = self._entries.get(key)
            return None if entry is None else entry.owner_key_id

    def owner_for_operation(self, requester_key_id: str,
                            operation_id: str) -> str | None:
        now_ns = self._monotonic_ns()
        with self._lock:
            self._purge_expired_locked(now_ns)
            for key, entry in self._entries.items():
                if key[0] == requester_key_id and \
                        entry.operation_id == operation_id:
                    return entry.owner_key_id
        return None


class BoundedThreadingHTTPServer(ThreadingHTTPServer):
    """限制活动请求线程数；满载时由监听队列自然施加背压。"""

    def __init__(self, server_address, request_handler_class, *,
                 maximum_workers: int = 32,
                 request_io_timeout_seconds: float =
                 DEFAULT_REQUEST_IO_TIMEOUT_SECONDS):
        if maximum_workers < 1 or maximum_workers > 256:
            raise ValueError("HTTP工作线程数必须位于1～256")
        if isinstance(request_io_timeout_seconds, bool) or not isinstance(
                request_io_timeout_seconds, (int, float)) or not \
                MINIMUM_REQUEST_IO_TIMEOUT_SECONDS <= \
                request_io_timeout_seconds <= \
                MAXIMUM_REQUEST_IO_TIMEOUT_SECONDS:
            raise ValueError("HTTP请求I/O超时必须位于0.1～30秒")
        self._worker_slots = threading.BoundedSemaphore(maximum_workers)
        self.request_io_timeout_seconds = float(request_io_timeout_seconds)
        super().__init__(server_address, request_handler_class)

    def process_request(self, request, client_address) -> None:
        # 在占用工作线程前设置有界等待，避免无期限的残缺头部或请求体占槽。
        try:
            request.settimeout(self.request_io_timeout_seconds)
        except OSError:
            self.shutdown_request(request)
            return
        self._worker_slots.acquire()
        try:
            super().process_request(request, client_address)
        except BaseException:
            self._worker_slots.release()
            raise

    def process_request_thread(self, request, client_address) -> None:
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._worker_slots.release()

    def server_close(self) -> None:
        try:
            super().server_close()
        finally:
            control_audit = getattr(self, "control_audit_journal", None)
            close_control_audit = getattr(control_audit, "close", None)
            if callable(close_control_audit):
                close_control_audit()
            audit_sink = getattr(self, "audit_sink", None)
            close = getattr(audit_sink, "close", None)
            if callable(close):
                close()


class IPv6ThreadingHTTPServer(BoundedThreadingHTTPServer):
    address_family = socket.AF_INET6


class RuntimeRequestHandler(BaseHTTPRequestHandler):
    server_version = "RemoteBSP-Runtime/0.1"

    def handle(self) -> None:
        try:
            super().handle()
        except (BrokenPipeError, ConnectionAbortedError,
                ConnectionResetError):
            # 期限或对端断开只终止当前连接，不影响其他请求。
            pass

    def handle_one_request(self) -> None:
        # 期限属于单个 HTTP request，keep-alive 上的后续请求
        # 必须获得新期限，不得继承前一次请求的旧预算。
        self._request_deadline = MonotonicDeadline.after_seconds(
            self.server.request_io_timeout_seconds)  # type: ignore[attr-defined]
        header_complete = threading.Event()
        self._header_complete = header_complete

        def close_stalled_header() -> None:
            try:
                timeout = self._request_deadline.remaining_seconds()
            except RequestDeadlineExceeded:
                timeout = 0.0
            if header_complete.wait(timeout):
                return
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

        threading.Thread(target=close_stalled_header, daemon=True).start()
        try:
            super().handle_one_request()
        finally:
            header_complete.set()

    @property
    def request_deadline(self) -> MonotonicDeadline:
        return self._request_deadline

    @property
    def provider(self) -> RuntimeProvider:
        return self.server.provider  # type: ignore[attr-defined]

    @property
    def authenticator(self) -> ApiKeyAuthenticator | None:
        return self.server.authenticator  # type: ignore[attr-defined]

    @property
    def audit_sink(self) -> BoundedAuditSink:
        return self.server.audit_sink  # type: ignore[attr-defined]

    @property
    def alert_rules(self) -> AlertRuleManager:
        return self.server.alert_rules  # type: ignore[attr-defined]

    @property
    def event_log(self) -> RuntimeEventLog:
        return self.server.event_log  # type: ignore[attr-defined]

    @property
    def control_leases(self) -> ControlLeaseManager:
        return self.server.control_leases  # type: ignore[attr-defined]

    @property
    def control_leases_available(self) -> bool:
        return self.server.control_leases_available  # type: ignore[attr-defined]

    @property
    def control_mutation_available(self) -> bool:
        return self.control_leases_available and self._control_audit_ready()

    @property
    def control_audit(self) -> ControlAuditJournal | None:
        return self.server.control_audit_journal  # type: ignore[attr-defined]

    def _control_audit_ready(self) -> bool:
        journal = self.control_audit
        if journal is None:
            return False
        lock = self.server.control_audit_state_lock  # type: ignore[attr-defined]
        with lock:
            failed = self.server.control_audit_failed  # type: ignore[attr-defined]
        try:
            return not failed and bool(journal.operational)
        except Exception:
            return False

    def _mark_control_audit_failed(self) -> None:
        lock = self.server.control_audit_state_lock  # type: ignore[attr-defined]
        with lock:
            self.server.control_audit_failed = True  # type: ignore[attr-defined]

    def _control_audit_health(self) -> dict:
        journal = self.control_audit
        if journal is None:
            return {
                "configured": False,
                "operational": False,
                "schema_version": CONTROL_AUDIT_SCHEMA_VERSION,
                "integrity": "hmac_sha256_chain",
                "durability": "synchronous",
                "records": 0,
                "segments": 0,
                "bytes": 0,
                "last_sequence": 0,
                "dangling_intents": 0,
            }
        try:
            health = asdict(journal.health_snapshot())
        except Exception:
            self._mark_control_audit_failed()
            health = {
                "operational": False, "records": 0, "segments": 0,
                "bytes": 0, "last_sequence": 0, "dangling_intents": 0,
            }
        health["configured"] = True
        health["operational"] = bool(
            health.get("operational") and self._control_audit_ready())
        health["schema_version"] = CONTROL_AUDIT_SCHEMA_VERSION
        health["integrity"] = "hmac_sha256_chain"
        health["durability"] = "synchronous"
        return health

    def _control_request_digest(self, action: str, fields: dict) -> str:
        encoded = json.dumps(
            {"action": action, **fields}, ensure_ascii=True,
            sort_keys=True, separators=(",", ":"),
            allow_nan=False).encode("utf-8")
        journal = self.control_audit
        if journal is None:
            raise ControlAuditError("控制审计日志未配置")
        return journal.request_digest(encoded)

    def _control_audit_failure(
            self, *, possibly_committed: bool,
            intent_recorded: bool,
            outcome: dict | None = None,
            lease: object | None = None,
            lookup: dict | None = None) -> None:
        self._mark_control_audit_failed()
        details: dict[str, object] = {
            "possibly_committed": possibly_committed,
            "safe_to_retry": not possibly_committed,
        }
        if outcome is not None:
            details["operation"] = self._public_operation(outcome)
        if lookup is not None:
            details["lookup"] = lookup
        if lease is not None:
            to_dict = getattr(lease, "to_dict", None)
            if callable(to_dict):
                public_lease = to_dict()
                public_lease.pop("owner_key_id", None)
                details["lease"] = public_lease
        self._error(
            HTTPStatus.SERVICE_UNAVAILABLE,
            ("control_audit_persistence_failed" if intent_recorded else
             "control_audit_unavailable"),
            "控制审计无法同步持久化，新的控制变更已失败关闭",
            details=details)

    def _begin_control_audit(
            self, principal: AuthenticatedPrincipal, *, action: str,
            fields: dict, lease_id: str | None = None
            ) -> ControlAuditRecord | None:
        journal = self.control_audit
        if journal is None or not self._control_audit_ready():
            self._control_audit_failure(
                possibly_committed=False, intent_recorded=False)
            return None
        try:
            return journal.append_intent(
                request_id=self._audit_request_id,
                key_id=principal.key_id,
                action=action,
                request_digest=self._control_request_digest(action, fields),
                lease_id=lease_id,
                node_id=None,
                resource_id=None)
        except ControlAuditError:
            self._control_audit_failure(
                possibly_committed=False, intent_recorded=False)
            return None

    def _complete_control_audit(
            self, intent: ControlAuditRecord, *, result: str | None = None,
            unknown_reason: str | None = None,
            operation_id: str | None = None,
            possibly_committed: bool,
            outcome: dict | None = None,
            lease: object | None = None,
            lookup: dict | None = None) -> bool:
        journal = self.control_audit
        if journal is None:
            self._control_audit_failure(
                possibly_committed=possibly_committed, intent_recorded=True,
                outcome=outcome,
                lease=lease, lookup=lookup)
            return False
        try:
            if unknown_reason is not None:
                journal.append_unknown(
                    intent.sequence, unknown_reason,
                    operation_id=operation_id)
            elif result is not None:
                journal.append_terminal(
                    intent.sequence, result, operation_id=operation_id)
            else:
                raise ValueError("控制审计终态缺少result或unknown_reason")
        except ControlAuditError:
            self._control_audit_failure(
                possibly_committed=possibly_committed, intent_recorded=True,
                outcome=outcome,
                lease=lease, lookup=lookup)
            return False
        return True

    def _complete_operation_audit(
            self, intent: ControlAuditRecord, outcome: dict, *,
            action: str, lookup: dict | None = None) -> bool:
        state = outcome["state"]
        operation_id = outcome.get("operation_id")
        if state == "committed":
            result = "released" if action == "release" else "committed"
            return self._complete_control_audit(
                intent, result=result, operation_id=operation_id,
                possibly_committed=True, outcome=outcome, lookup=lookup)
        if state == "rejected":
            return self._complete_control_audit(
                intent, result="rejected", operation_id=operation_id,
                possibly_committed=False, outcome=outcome, lookup=lookup)
        return self._complete_control_audit(
            intent, unknown_reason="downstream_uncertain",
            operation_id=operation_id, possibly_committed=True,
            outcome=outcome, lookup=lookup)

    @property
    def gpio_control_configured(self) -> bool:
        return self.server.gpio_control_configured  # type: ignore[attr-defined]

    @property
    def gpio_control_operational(self) -> bool:
        return self._control_audit_ready() and \
            self._gpio_backend_operational()

    def _gpio_backend_operational(self) -> bool:
        if not self.gpio_control_configured or not isinstance(
                self.control_leases, DaemonBoundControlLeaseManager):
            return False
        state_lock = self.server.gpio_control_state_lock  # type: ignore[attr-defined]
        with state_lock:
            revision = self.server.gpio_control_state_revision  # type: ignore[attr-defined]
            admitted = self.server.gpio_control_admitted_daemon_id  # type: ignore[attr-defined]
            operational = self.server.gpio_control_operational  # type: ignore[attr-defined]
        if not operational or not isinstance(admitted, str):
            return False
        try:
            matches = self.control_leases.matches_current_daemon(
                admitted, deadline=self.request_deadline)
        except RequestDeadlineExceeded:
            # 请求自身预算不足不是 daemon 换代证据；本次失败关闭，但不污染
            # 其他请求共享的 operational 证明或活动租约。
            return False
        except (ControlLeaseError, ValueError):
            matches = False
        if not matches:
            self._clear_gpio_control_operational(
                expected_instance_id=admitted,
                expected_revision=revision)
            return False
        # 身份读取期间另一个请求可能已完成新实例准入；本次只证明旧快照，
        # 因此必须再次核对状态，不能把旧证明套用到新实例。
        with state_lock:
            return bool(
                self.server.gpio_control_operational and  # type: ignore[attr-defined]
                self.server.gpio_control_admitted_daemon_id == admitted and  # type: ignore[attr-defined]
                self.server.gpio_control_state_revision == revision)  # type: ignore[attr-defined]

    def _gpio_control_revision(self) -> int:
        state_lock = self.server.gpio_control_state_lock  # type: ignore[attr-defined]
        with state_lock:
            return self.server.gpio_control_state_revision  # type: ignore[attr-defined]

    def _admit_gpio_control(
            self, daemon_instance_id: str,
            expected_revision: int) -> bool:
        current_daemon_id = getattr(
            self.control_leases, "daemon_instance_id", None)
        if current_daemon_id != daemon_instance_id:
            return False
        state_lock = self.server.gpio_control_state_lock  # type: ignore[attr-defined]
        with state_lock:
            if self.server.gpio_control_state_revision != expected_revision:  # type: ignore[attr-defined]
                return False
            self.server.gpio_control_admitted_daemon_id = daemon_instance_id  # type: ignore[attr-defined]
            self.server.gpio_control_operational = True  # type: ignore[attr-defined]
            self.server.gpio_control_state_revision += 1  # type: ignore[attr-defined]
            return True

    def _clear_gpio_control_operational(
            self, expected_instance_id: str | None = None,
            expected_revision: int | None = None) -> bool:
        state_lock = self.server.gpio_control_state_lock  # type: ignore[attr-defined]
        with state_lock:
            admitted = self.server.gpio_control_admitted_daemon_id  # type: ignore[attr-defined]
            if expected_instance_id is not None and \
                    admitted != expected_instance_id:
                return False
            if expected_revision is not None and \
                    self.server.gpio_control_state_revision != expected_revision:  # type: ignore[attr-defined]
                return False
            self.server.gpio_control_operational = False  # type: ignore[attr-defined]
            self.server.gpio_control_admitted_daemon_id = None  # type: ignore[attr-defined]
            self.server.gpio_control_state_revision += 1  # type: ignore[attr-defined]
            return True

    def _begin_request_audit(self) -> None:
        # BaseHTTPRequestHandler 只有在请求行和全部头部解析完成后才分派到
        # do_*；此时停止头部总期限，后续请求体由自身总期限负责。
        self._header_complete.set()
        self._audit_request_id = secrets.token_hex(16)
        self._audit_key_id: str | None = None
        self._audit_path_category = self._classify_path(self.path)
        self._audit_emitted = False

    def _structured_provider_error(
            self, error: RuntimeProviderOperationError) -> None:
        status, public_code = {
            "deadline_exceeded": (
                HTTPStatus.GATEWAY_TIMEOUT, "control_deadline_exceeded"),
            "backend_unavailable": (
                HTTPStatus.SERVICE_UNAVAILABLE, "control_backend_unavailable"),
            "protocol_incompatible": (
                HTTPStatus.BAD_GATEWAY, "control_protocol_incompatible"),
            "target_rejected": (
                HTTPStatus.CONFLICT, "control_target_rejected"),
        }[error.code]
        self._error(
            status, public_code, "控制操作未获得可验证的下游成功结果",
            details={
                "category": error.category,
                "retryable": error.retryable,
                "possibly_committed": error.possibly_committed,
            })

    @staticmethod
    def _public_operation(outcome: dict) -> dict:
        recovery = outcome["recovery"]
        error_code = outcome["error_code"]
        error = None
        if error_code is not None:
            error = {
                "code": error_code,
                "category": {
                    "rejected": "operation",
                    "deadline": "timeout",
                    "backend": "backend",
                    "persistence": "persistence",
                    "history_expired": "history",
                }[error_code],
                # ledger 终态原样重放不会再次执行；这里的 retryable
                # 只能表示“原请求可直接重放”，因此一律为 false。
                "retryable": False,
                "possibly_committed": outcome["state"] in {
                    "unknown", "expired_unknown"},
            }
        return {
            "operation_schema_version": 1,
            "operation_id": outcome["operation_id"],
            "lease_id": outcome["lease_id"],
            "scope": (None if outcome["expected_node_uuid"] is None else {
                "expected_node_uuid": outcome["expected_node_uuid"],
                "resource_id": outcome["resource_id"],
            }),
            "operation_kind": outcome["kind"],
            "state": outcome["state"],
            "recovery": recovery,
            "scope_blocked": recovery in {
                "scope_blocked", "awaiting_reboot"},
            # ledger 终态不会因原请求重放而重新执行；unknown 更不能盲重试。
            "safe_to_retry": False,
            "replayed": outcome["replayed"],
            "result": ({
                "object_id": outcome["object_id"],
                "value": outcome["value"],
            } if outcome["state"] == "committed" and
                outcome["kind"] == "gpio_write" else ({
                    "released": True,
                } if outcome["state"] == "committed" and
                    outcome["kind"] == "control_release" else None)),
            "error": error,
        }

    def _reconcile_operation(
            self, outcome: dict,
            principal: AuthenticatedPrincipal | None = None) -> None:
        node_uuid = outcome.get("expected_node_uuid")
        resource_id = outcome.get("resource_id")
        scope = None
        if isinstance(node_uuid, str) and type(resource_id) is int:
            scope = ("node-" + node_uuid, f"resource-{resource_id:08x}")
        should_block = outcome["recovery"] in {
            "scope_blocked", "awaiting_reboot"} or (
                outcome["kind"] == "control_release" and
                outcome["state"] == "rejected" and
                outcome["recovery"] == "not_sent")
        safe = outcome["recovery"] in {
            "safe_closed", "node_reboot_confirmed"}
        if scope is not None:
            lock = self.server.operation_scope_lock  # type: ignore[attr-defined]
            with lock:
                blocked = self.server.blocked_operation_scopes  # type: ignore[attr-defined]
                if safe:
                    blocked.discard(scope)
                elif should_block and len(blocked) < \
                        self.server.operation_scope_capacity:  # type: ignore[attr-defined]
                    blocked.add(scope)
        if principal is None or not safe or \
                outcome["kind"] != "control_release" or \
                not isinstance(outcome.get("lease_id"), str):
            return
        try:
            call_with_deadline(
                self.control_leases.release, outcome["lease_id"],
                requester_key_id=principal.key_id,
                allow_foreign=(CONTROL_LEASE_REVOKE_PERMISSION in
                               principal.permissions),
                deadline=self.request_deadline)
        except (ControlLeaseNotFound, ControlLeaseOwnershipError,
                RequestDeadlineExceeded):
            pass

    def _send_operation(self, outcome: dict, *,
                        audit_result: str,
                        principal: AuthenticatedPrincipal | None = None) -> None:
        self._reconcile_operation(outcome, principal)
        operation = self._public_operation(outcome)
        location = (f"/api/{API_VERSION}/control/operations/" +
                    outcome["operation_id"])
        headers = {"Location": location}
        state = outcome["state"]
        if state == "pending":
            headers["Retry-After"] = "1"
            self._send_json({
                "api_version": API_VERSION, "ok": True,
                "data": {"operation": operation},
            }, HTTPStatus.ACCEPTED, headers=headers,
                audit_result=audit_result + "_pending")
            return
        if state == "committed":
            self._send_json({
                "api_version": API_VERSION, "ok": True,
                "data": {"operation": operation},
            }, HTTPStatus.OK, headers=headers,
                audit_result=audit_result + "_committed")
            return
        if state in {"unknown", "expired_unknown"}:
            code = ("control_operation_expired_unknown" if
                    state == "expired_unknown" else
                    "control_operation_unknown")
            self._error(
                HTTPStatus.CONFLICT, code,
                "操作结果不能被安全重放；请依据恢复状态协调资源",
                headers=headers, details={"operation": operation})
            return
        error_code = outcome["error_code"]
        status, code = {
            "rejected": (HTTPStatus.CONFLICT,
                         "control_operation_rejected"),
            "deadline": (HTTPStatus.GATEWAY_TIMEOUT,
                         "control_operation_deadline"),
            "backend": (HTTPStatus.SERVICE_UNAVAILABLE,
                        "control_operation_backend_unavailable"),
            "persistence": (HTTPStatus.SERVICE_UNAVAILABLE,
                            "control_operation_persistence_failed"),
        }[error_code]
        self._error(status, code, "控制操作被持久账本确定拒绝",
                    headers=headers, details={"operation": operation})

    def _operation_uncertain(self, *, kind: str, lease_id: str,
                             idempotency_key: str) -> None:
        self._error(
            HTTPStatus.CONFLICT, "control_operation_lookup_required",
            "控制操作可能已提交，必须先查询持久账本，禁止盲目重试",
            details={
                "safe_to_retry": False,
                "lookup": {
                    "method": "POST",
                    "path": f"/api/{API_VERSION}/control/operation-lookups",
                    "body": {
                        "operation_kind": kind,
                        "lease_id": lease_id,
                        "idempotency_key": idempotency_key,
                    },
                },
            })

    @staticmethod
    def _classify_path(target: str) -> str:
        try:
            path = urlparse(target).path
            parts = [
                unquote(part, encoding="utf-8", errors="strict")
                for part in path.strip("/").split("/") if part
            ]
        except (UnicodeDecodeError, ValueError):
            return "unknown"
        if not parts or parts in (["dashboard"], ["dashboard.css"],
                                  ["dashboard.js"]):
            return "dashboard"
        if parts == ["api", API_VERSION]:
            return "root"
        if len(parts) >= 3 and parts[:2] == ["api", API_VERSION] and \
                parts[2] in {"health", "snapshot", "overview", "operations", "nodes", "resources",
                             "alerts", "events", "control-leases", "alert-rules"}:
            return "control_leases" if parts[2] == "control-leases" \
                else "alert_rules" if parts[2] == "alert-rules" else parts[2]
        if parts == ["api", API_VERSION, "control", "gpio", "write"]:
            return "gpio_control"
        if len(parts) >= 3 and parts[:3] == [
                "api", API_VERSION, "control"] and len(parts) >= 4 and \
                parts[3] in {"operations", "operation-lookups"}:
            return "control_operations"
        return "unknown"

    def _emit_audit(self, result: str) -> None:
        if getattr(self, "_audit_emitted", False):
            return
        self._audit_emitted = True
        method_category = {
            "GET": "read", "HEAD": "read", "POST": "write",
            "PUT": "write", "PATCH": "write", "DELETE": "write",
            "OPTIONS": "options",
        }.get(self.command, "other")
        record = new_audit_record(
            request_id=self._audit_request_id,
            key_id=self._audit_key_id,
            method_category=method_category,
            path_category=self._audit_path_category,
            result=result)
        try:
            self.audit_sink.emit(record)
        except Exception:
            # 外部审计输出端故障不能改变 API 响应或阻塞后续请求。
            pass

    def _send_json(self, value: object, status: HTTPStatus = HTTPStatus.OK,
                   *, headers: dict[str, str] | None = None,
                   audit_result: str = "allowed") -> None:
        encoded = json.dumps(value, ensure_ascii=False,
                             separators=(",", ":")).encode("utf-8")
        self._emit_audit(audit_result)
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Request-ID", self._audit_request_id)
        for key, header_value in (headers or {}).items():
            self.send_header(key, header_value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(encoded)

    def _error(self, status: HTTPStatus, code: str, message: str,
               *, headers: dict[str, str] | None = None,
               details: dict | None = None) -> None:
        if int(status) >= 500:
            self.server.operational_state.record_error(code)  # type: ignore[attr-defined]
        error_payload = {"code": code, "message": message}
        if details is not None:
            error_payload["details"] = details
        self._send_json({
            "api_version": API_VERSION,
            "ok": False,
            "error": error_payload,
        }, status, headers=headers, audit_result=code)

    def _success(self, data: object, status: HTTPStatus = HTTPStatus.OK,
                 *, read: SnapshotRead | None = None,
                 audit_result: str = "allowed") -> None:
        payload = {
            "api_version": API_VERSION,
            "ok": True,
            "data": data,
        }
        headers = None
        if read is not None:
            freshness = {
                "cache_status": read.cache_status,
                "age_ms": read.age_ms,
                "cache_ttl_ms": read.cache_ttl_ms,
            }
            payload["meta"] = {"snapshot_freshness": freshness}
            headers = {
                "X-RemoteBSP-Snapshot-Cache": read.cache_status,
                "X-RemoteBSP-Snapshot-Age-Ms": (
                    "unknown" if read.age_ms is None else str(read.age_ms)),
            }
        self._send_json(payload, status, headers=headers,
                        audit_result=audit_result)

    def _send_empty(self, status: HTTPStatus, *,
                    headers: dict[str, str] | None = None,
                    audit_result: str = "allowed") -> None:
        self._emit_audit(audit_result)
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Request-ID", self._audit_request_id)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()

    def _send_dashboard_asset(self, path: str) -> bool:
        asset = _DASHBOARD_ASSETS.get(path)
        if asset is None:
            return False
        filename, content_type = asset
        try:
            encoded = (Path(__file__).resolve().parent / "static" /
                       filename).read_bytes()
        except OSError:
            self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                        "dashboard_unavailable", "仪表盘资源暂不可用")
            return True
        self._emit_audit("allowed")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy",
                         "default-src 'none'; script-src 'self'; "
                         "style-src 'self'; connect-src 'self'; "
                         "img-src 'self'; base-uri 'none'; "
                         "form-action 'none'; frame-ancestors 'none'")
        self.send_header("X-Request-ID", self._audit_request_id)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(encoded)
        return True

    def _overview_stream_event(self) -> bytes:
        """生成一条独立快照事件；绝不跨身份缓存编码后的响应。"""
        deadline = MonotonicDeadline.after_seconds(
            self.server.request_io_timeout_seconds)  # type: ignore[attr-defined]
        read = call_with_deadline(self.provider.read_snapshot,
                                  deadline=deadline)
        self.event_log.observe(read.snapshot)
        health_reader = getattr(self.provider, "health_snapshot", None)
        health = {"available": False, "snapshot": None,
                  "reason": "unsupported"}
        if callable(health_reader):
            try:
                health = {"available": True, "snapshot": call_with_deadline(
                    health_reader, deadline=deadline), "reason": None}
            except RuntimeProviderError:
                health = {"available": False, "snapshot": None,
                          "reason": "temporarily_unavailable"}
            except RequestDeadlineExceeded:
                health = {"available": False, "snapshot": None,
                          "reason": "deadline_exceeded"}
        overview = self.server.runtime_dashboard.observe(  # type: ignore[attr-defined]
            read.snapshot, health)
        encoded = json.dumps({"api_version": API_VERSION, "ok": True,
                              "data": overview}, ensure_ascii=False,
                             separators=(",", ":")).encode("utf-8")
        if len(encoded) > MAXIMUM_OVERVIEW_STREAM_EVENT_BYTES:
            raise RuntimeProviderError("overview事件超过大小上限")
        return b"event: overview\ndata: " + encoded + b"\n\n"

    def _handle_overview_stream(self) -> None:
        slots = self.server.overview_stream_slots  # type: ignore[attr-defined]
        if not slots.acquire(blocking=False):
            self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                        "overview_stream_capacity",
                        "主动推送连接已达到上限",
                        headers={"Retry-After": "2"})
            return
        self.server.operational_state.stream_opened()  # type: ignore[attr-defined]
        stop = threading.Event()
        updates: queue.Queue[bytes | None] = queue.Queue(
            maxsize=OVERVIEW_STREAM_QUEUE_CAPACITY)

        def produce() -> None:
            interval = self.server.overview_stream_interval_seconds  # type: ignore[attr-defined]
            try:
                while not stop.is_set():
                    try:
                        event = self._overview_stream_event()
                    except (RuntimeProviderError, RequestDeadlineExceeded):
                        event = None
                    if event is not None:
                        # 慢客户端只保留最新状态，生产者永不等待写端。
                        try:
                            updates.put_nowait(event)
                        except queue.Full:
                            try:
                                updates.get_nowait()
                            except queue.Empty:
                                pass
                            try:
                                updates.put_nowait(event)
                            except queue.Full:
                                pass
                    if stop.wait(interval):
                        break
            finally:
                try:
                    updates.put_nowait(None)
                except queue.Full:
                    pass

        try:
            self._emit_audit("allowed")
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-store, no-transform")
            self.send_header("Connection", "close")
            self.send_header("X-Accel-Buffering", "no")
            self.send_header("Vary", "Authorization, X-API-Key")
            self.send_header("X-Request-ID", self._audit_request_id)
            self.end_headers()
            # 普通请求的头部期限不应误杀已经认证的长连接。
            self._header_complete.set()
            producer = threading.Thread(target=produce, daemon=True,
                                        name="runtime-overview-sse")
            producer.start()
            self.close_connection = True
            while not stop.is_set():
                try:
                    event = updates.get(timeout=OVERVIEW_STREAM_HEARTBEAT_SECONDS)
                except queue.Empty:
                    event = b": keepalive\n\n"
                if event is None:
                    break
                self.wfile.write(event)
                self.wfile.flush()
        finally:
            stop.set()
            producer_thread = locals().get("producer")
            if producer_thread is not None:
                producer_thread.join(timeout=
                    self.server.request_io_timeout_seconds)  # type: ignore[attr-defined]
            slots.release()
            self.server.operational_state.stream_closed()  # type: ignore[attr-defined]

    def _parse_request_target(self, *,
                              allow_event_query: bool = False
                              ) -> ParseResult | None:
        try:
            parsed = urlparse(self.path)
        except ValueError:
            self._error(HTTPStatus.BAD_REQUEST, "invalid_path", "请求路径无效")
            return None
        self._query_pairs: list[tuple[str, str]] = []
        if parsed.query:
            try:
                self._query_pairs = parse_qsl(
                    parsed.query, keep_blank_values=True, max_num_fields=64)
                names = {name.lower() for name, _ in self._query_pairs}
            except ValueError:
                self._error(HTTPStatus.BAD_REQUEST, "invalid_query",
                            "查询参数编码无效")
                return None
            if names & _QUERY_CREDENTIAL_NAMES:
                self._error(HTTPStatus.BAD_REQUEST, "credential_in_query",
                            "认证信息不得放入URL查询参数")
            elif allow_event_query and \
                    parsed.path == f"/api/{API_VERSION}/events":
                return parsed
            else:
                self._error(HTTPStatus.BAD_REQUEST, "query_not_supported",
                            "当前端点不接受查询参数")
            return None
        if parsed.fragment:
            self._error(HTTPStatus.BAD_REQUEST, "query_not_supported",
                        "当前端点不接受查询参数")
            return None
        return parsed

    def _event_parameters(self) -> tuple[str | None, int] | None:
        values: dict[str, str] = {}
        for name, value in self._query_pairs:
            if name not in {"cursor", "limit"} or name in values:
                self._error(HTTPStatus.BAD_REQUEST, "event_query_invalid",
                            "事件查询只允许各一个cursor和limit参数")
                return None
            values[name] = value
        cursor = values.get("cursor")
        if cursor == "":
            self._error(HTTPStatus.BAD_REQUEST, "event_cursor_invalid",
                        "事件游标不能为空")
            return None
        if cursor is not None:
            try:
                validate_event_cursor(cursor)
            except InvalidEventCursor as error:
                self._error(HTTPStatus.BAD_REQUEST, "event_cursor_invalid",
                            str(error))
                return None
        limit_text = values.get("limit")
        if limit_text is None:
            limit = DEFAULT_EVENT_PAGE_LIMIT
        elif len(limit_text) > 3 or not limit_text.isascii() or \
                not limit_text.isdigit() or \
                (len(limit_text) > 1 and limit_text.startswith("0")):
            self._error(HTTPStatus.BAD_REQUEST, "event_limit_invalid",
                        "事件页大小必须是规范十进制整数")
            return None
        else:
            limit = int(limit_text)
            if limit < 1 or limit > MAXIMUM_EVENT_PAGE_LIMIT:
                self._error(
                    HTTPStatus.BAD_REQUEST, "event_limit_invalid",
                    f"事件页大小必须位于1～{MAXIMUM_EVENT_PAGE_LIMIT}")
                return None
        return cursor, limit

    def _path_parts(self, path: str) -> list[str] | None:
        try:
            return [
                unquote(part, encoding="utf-8", errors="strict")
                for part in path.strip("/").split("/") if part
            ]
        except UnicodeDecodeError:
            self._error(HTTPStatus.BAD_REQUEST, "invalid_path", "路径编码无效")
            return None

    def _authorize(self, path: str, *, public_health: bool,
                   required_permission: str | None
                   ) -> AuthenticatedPrincipal | str | None:
        if self.authenticator is None:
            return "disabled_loopback"

        authorization = self.headers.get_all("Authorization", [])
        api_keys = self.headers.get_all("X-API-Key", [])
        if not authorization and not api_keys and public_health and \
                path == f"/api/{API_VERSION}/health":
            return "public_health"
        if not authorization and not api_keys:
            self._error(
                HTTPStatus.UNAUTHORIZED, "authentication_required",
                "需要有效的API密钥",
                headers={"WWW-Authenticate":
                         'Bearer realm="RemoteBSP Runtime"'})
            return None
        if len(authorization) + len(api_keys) != 1:
            self._error(HTTPStatus.BAD_REQUEST, "invalid_auth_header",
                        "必须且只能使用一种认证请求头")
            return None

        if authorization:
            pieces = authorization[0].split(" ")
            if len(pieces) != 2 or pieces[0].lower() != "bearer" or \
                    not pieces[1]:
                self._error(HTTPStatus.BAD_REQUEST, "invalid_auth_header",
                            "Authorization必须使用Bearer格式")
                return None
            candidate = pieces[1]
        else:
            candidate = api_keys[0]
            if not candidate or candidate.strip() != candidate:
                self._error(HTTPStatus.BAD_REQUEST, "invalid_auth_header",
                            "X-API-Key格式无效")
                return None
        principal = None if len(candidate) > MAXIMUM_API_KEY_BYTES else \
            self.authenticator.authenticate(candidate)
        if principal is None:
            self._error(
                HTTPStatus.UNAUTHORIZED, "authentication_required",
                "需要有效的API密钥",
                headers={"WWW-Authenticate":
                         'Bearer realm="RemoteBSP Runtime"'})
            return None
        self._audit_key_id = principal.key_id
        if required_permission is not None and \
                required_permission not in principal.permissions:
            self._error(HTTPStatus.FORBIDDEN, "permission_denied",
                        "当前API密钥没有所需权限")
            return None
        return principal

    def _read_control_json(self) -> dict | None:
        if self.headers.get_all("Transfer-Encoding", []):
            self._error(HTTPStatus.BAD_REQUEST, "transfer_encoding_unsupported",
                        "控制请求不接受Transfer-Encoding")
            return None
        content_types = self.headers.get_all("Content-Type", [])
        lengths = self.headers.get_all("Content-Length", [])
        if len(content_types) != 1 or \
                content_types[0].split(";", 1)[0].strip().lower() != \
                "application/json":
            self._error(HTTPStatus.UNSUPPORTED_MEDIA_TYPE,
                        "content_type_invalid", "控制请求必须使用application/json")
            return None
        if len(lengths) != 1 or not lengths[0].isascii() or \
                not lengths[0].isdigit() or \
                (len(lengths[0]) > 1 and lengths[0].startswith("0")):
            self._error(HTTPStatus.BAD_REQUEST, "content_length_invalid",
                        "控制请求必须携带一个规范Content-Length")
            return None
        length = int(lengths[0])
        if length < 2 or length > MAXIMUM_CONTROL_REQUEST_BYTES:
            self._error(HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                        "request_body_too_large",
                        f"控制请求体必须位于2～{MAXIMUM_CONTROL_REQUEST_BYTES}字节")
            return None
        try:
            chunks: list[bytes] = []
            received = 0
            while received < length:
                remaining_seconds = self.request_deadline.remaining_seconds()
                self.connection.settimeout(remaining_seconds)
                maximum = min(length - received, MAXIMUM_CONTROL_REQUEST_BYTES)
                read1 = getattr(self.rfile, "read1", None)
                chunk = read1(maximum) if callable(read1) \
                    else self.rfile.read(maximum)
                if not chunk:
                    raise ValueError("请求体提前结束")
                chunks.append(chunk)
                received += len(chunk)
            raw = b"".join(chunks)
            value = json.loads(raw.decode("utf-8"),
                               object_pairs_hook=_strict_json_object)
        except (RequestDeadlineExceeded, TimeoutError, socket.timeout):
            self._error(HTTPStatus.REQUEST_TIMEOUT, "request_body_timeout",
                        "读取控制请求体超时")
            return None
        except OSError:
            self._error(HTTPStatus.BAD_REQUEST, "request_body_invalid",
                        "无法完整读取控制请求体")
            return None
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError,
                RecursionError):
            self._error(HTTPStatus.BAD_REQUEST, "request_body_invalid",
                        "控制请求体不是合法UTF-8 JSON")
            return None
        finally:
            try:
                self.connection.settimeout(
                    self.request_deadline.remaining_seconds())
            except RequestDeadlineExceeded:
                pass
            except OSError:
                pass
        if not isinstance(value, dict):
            self._error(HTTPStatus.BAD_REQUEST, "request_body_invalid",
                        "控制请求体根值必须是对象")
            return None
        return value

    def _current_operation_daemon(self) -> str:
        # 查询不要求目标租约仍存活，但必须在同一总期限内确认当前 daemon，
        # 防止把请求发送给身份已经变化的本地进程。
        call_with_deadline(
            self.control_leases.active_count, deadline=self.request_deadline)
        daemon_id = getattr(
            self.control_leases, "daemon_instance_id", None)
        if not isinstance(daemon_id, str):
            raise ControlLeaseError("toolbusd实例身份尚未绑定")
        return daemon_id

    def _stable_operation_query(
            self, query: Callable[[str], dict]) -> dict:
        """daemon 换代时仅重查一次；结果绝不依赖旧租约是否仍活动。"""
        daemon_id = self._current_operation_daemon()
        outcome = query(daemon_id)
        if call_with_deadline(
                self.control_leases.matches_current_daemon,
                daemon_id, deadline=self.request_deadline):
            return outcome
        replacement = getattr(
            self.control_leases, "daemon_instance_id", None)
        if not isinstance(replacement, str) or replacement == daemon_id:
            raise ControlLeaseError("toolbusd实例身份无法稳定确认")
        outcome = query(replacement)
        if not call_with_deadline(
                self.control_leases.matches_current_daemon,
                replacement, deadline=self.request_deadline):
            raise ControlLeaseError("toolbusd实例身份在重查期间再次变化")
        return outcome

    def _try_operation_lookup(
            self, owner_key_id: str, *, kind: str,
            lease_id: str, idempotency_key: str) -> dict | None:
        """可能提交后仅做只读恢复；失败时由调用者继续返回冻结locator。"""
        try:
            return self._stable_operation_query(
                lambda daemon_id: call_with_deadline(
                    self.provider.operation_lookup,  # type: ignore[attr-defined]
                    daemon_id, owner_key_id, kind, lease_id,
                    idempotency_key, deadline=self.request_deadline))
        except (RequestDeadlineExceeded, ControlLeaseError,
                RuntimeProviderError):
            return None

    def _finish_local_release_if_safe(
            self, outcome: dict, lease_id: str,
            principal: AuthenticatedPrincipal, allow_foreign: bool) -> None:
        safely_terminal = outcome["state"] == "committed" or (
            outcome["state"] in {"rejected", "unknown"} and
            outcome["recovery"] in {
                "safe_closed", "node_reboot_confirmed"})
        if not safely_terminal:
            return
        try:
            call_with_deadline(
                self.control_leases.release, lease_id,
                requester_key_id=principal.key_id,
                allow_foreign=allow_foreign,
                deadline=self.request_deadline)
        except ControlLeaseNotFound:
            # TTL 后账本查询仍是权威；本地占位已自然消失。
            pass

    def _handle_operation_status(
            self, principal: AuthenticatedPrincipal,
            operation_id: str) -> None:
        revision = self._gpio_control_revision()
        try:
            if _OPERATION_ID_PATTERN.fullmatch(operation_id) is None or \
                    operation_id == "0" * 64:
                raise ValueError("operation_id必须是非零64位小写十六进制")
            owner_key_id = self.server.foreign_operation_recovery.owner_for_operation(  # type: ignore[attr-defined]
                principal.key_id, operation_id)
            outcome = self._stable_operation_query(
                lambda daemon_id: call_with_deadline(
                    self.provider.operation_status,  # type: ignore[attr-defined]
                    daemon_id, owner_key_id or principal.key_id, operation_id,
                    deadline=self.request_deadline))
        except ValueError as error:
            self._error(HTTPStatus.BAD_REQUEST,
                        "control_operation_id_invalid", str(error))
            return
        except RequestDeadlineExceeded:
            self._error(HTTPStatus.GATEWAY_TIMEOUT,
                        "control_operation_query_deadline",
                        "操作查询未在统一处理期限内完成")
            return
        except (ControlLeaseError, RuntimeProviderError):
            self._clear_gpio_control_operational(expected_revision=revision)
            self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                        "control_operation_ledger_unavailable",
                        "无法取得可信的操作账本结果")
            return
        self._send_operation(
            outcome, audit_result="control_operation_query",
            principal=principal)

    def _handle_operation_lookup(
            self, principal: AuthenticatedPrincipal) -> None:
        value = self._read_control_json()
        if value is None:
            return
        expected = {"operation_kind", "lease_id", "idempotency_key"}
        if set(value) != expected:
            self._error(
                HTTPStatus.BAD_REQUEST, "request_body_invalid",
                "操作定位字段必须且只能包含operation_kind、lease_id和idempotency_key")
            return
        revision = self._gpio_control_revision()
        try:
            kind = value["operation_kind"]
            if not isinstance(kind, str) or kind not in _OPERATION_KINDS:
                raise ValueError("operation_kind无效")
            lease_id = validate_lease_id(value["lease_id"])
            idempotency_key = validate_idempotency_key(
                value["idempotency_key"])
            if kind == "control_release" and \
                    idempotency_key != "release:v1":
                raise ValueError("control_release定位必须使用release:v1")
            owner_key_id = self.server.foreign_operation_recovery.owner_for_selector(  # type: ignore[attr-defined]
                principal.key_id, kind=kind, lease_id=lease_id,
                idempotency_key=idempotency_key)
            outcome = self._stable_operation_query(
                lambda daemon_id: call_with_deadline(
                    self.provider.operation_lookup,  # type: ignore[attr-defined]
                    daemon_id, owner_key_id or principal.key_id, kind, lease_id,
                    idempotency_key, deadline=self.request_deadline))
        except ValueError as error:
            self._error(HTTPStatus.BAD_REQUEST,
                        "control_operation_lookup_invalid", str(error))
            return
        except RequestDeadlineExceeded:
            self._error(HTTPStatus.GATEWAY_TIMEOUT,
                        "control_operation_query_deadline",
                        "操作定位查询未在统一处理期限内完成")
            return
        except (ControlLeaseError, RuntimeProviderError):
            self._clear_gpio_control_operational(expected_revision=revision)
            self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                        "control_operation_ledger_unavailable",
                        "无法取得可信的操作账本结果")
            return
        if owner_key_id is not None:
            self.server.foreign_operation_recovery.bind_operation(  # type: ignore[attr-defined]
                principal.key_id, outcome["operation_id"], kind=kind,
                lease_id=lease_id, idempotency_key=idempotency_key)
        self._send_operation(
            outcome, audit_result="control_operation_lookup",
            principal=principal)

    def _handle_control_lease_acquire(self, principal: AuthenticatedPrincipal
                                      ) -> None:
        value = self._read_control_json()
        if value is None:
            return
        expected = {"node_id", "resource_id", "command_group", "ttl_ms",
                    "idempotency_key"}
        if set(value) != expected:
            self._error(HTTPStatus.BAD_REQUEST, "request_body_invalid",
                        "控制租约字段必须且只能包含node_id、resource_id、"
                        "command_group、ttl_ms和idempotency_key")
            return
        lease = None
        replayed = False
        downstream_registered = False
        downstream_started = False
        daemon_id = None
        audit_intent = None
        operational_revision = self._gpio_control_revision()
        try:
            node_id = validate_control_id(value["node_id"], "node_id")
            resource_id = validate_control_id(
                value["resource_id"], "resource_id")
            command_group = validate_control_id(
                value["command_group"], "command_group")
            ttl_ms = validate_ttl_ms(value["ttl_ms"])
            idempotency_key = validate_idempotency_key(
                value["idempotency_key"])
            with self.server.operation_scope_lock:  # type: ignore[attr-defined]
                if (node_id, resource_id) in \
                        self.server.blocked_operation_scopes:  # type: ignore[attr-defined]
                    self._error(
                        HTTPStatus.CONFLICT, "control_scope_blocked",
                        "目标资源存在提交状态未知的历史操作，必须先完成安全协调",
                        details={"safe_to_retry": False})
                    return
            if command_group == GPIO_WRITE_COMMAND_GROUP and \
                    GPIO_WRITE_PERMISSION not in principal.permissions:
                self._error(HTTPStatus.FORBIDDEN, "permission_denied",
                            "当前API密钥没有GPIO写权限")
                return
            if command_group == GPIO_WRITE_COMMAND_GROUP and \
                    not self.gpio_control_configured:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "gpio_control_unavailable",
                            "GPIO写控制后端不可用")
                return
            audit_intent = self._begin_control_audit(
                principal, action="acquire", fields={
                    "node_id": node_id,
                    "resource_id": resource_id,
                    "command_group": command_group,
                    "ttl_ms": ttl_ms,
                    "idempotency_key": idempotency_key,
                })
            if audit_intent is None:
                return
            lease, replayed = call_with_deadline(
                self.control_leases.acquire,
                owner_key_id=principal.key_id, node_id=node_id,
                resource_id=resource_id, command_group=command_group,
                ttl_ms=ttl_ms, idempotency_key=idempotency_key,
                deadline=self.request_deadline)
            if command_group == GPIO_WRITE_COMMAND_GROUP:
                daemon_id = getattr(
                    self.control_leases, "daemon_instance_id", None)
                if not isinstance(daemon_id, str):
                    raise ControlLeaseError("toolbusd实例身份尚未绑定")
                # replay 也必须重新经过下游幂等登记，不能只凭 Runtime
                # 进程内历史恢复 operational 证明。
                downstream_started = True
                call_with_deadline(
                    self.provider.gpio_control_acquire,  # type: ignore[attr-defined]
                    daemon_id, lease.lease_id, principal.key_id,
                    node_id, resource_id,
                    lambda: call_with_deadline(
                        self.control_leases.remaining_ttl_ms,
                        lease.lease_id,
                        requester_key_id=principal.key_id,
                        deadline=self.request_deadline),
                    deadline=self.request_deadline)
                downstream_registered = True
                # 远端登记可能接近 IPC 总期限；成功响应前再次按单调时钟
                # 确认本地租约仍活动，绝不返回一个已经过期的租约。
                call_with_deadline(
                    self.control_leases.authorize, lease.lease_id,
                    requester_key_id=principal.key_id,
                    deadline=self.request_deadline)
                self.request_deadline.check()
                self._admit_gpio_control(
                    daemon_id, operational_revision)
        except ValueError as error:
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="rejected",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.BAD_REQUEST, "request_body_invalid",
                        str(error))
            return
        except ControlLeaseConflict as error:
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="rejected",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.CONFLICT, "control_lease_conflict",
                        str(error))
            return
        except ControlLeaseCapacityExceeded as error:
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="failed",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                        "control_lease_capacity_exceeded", str(error))
            return
        except RequestDeadlineExceeded:
            # 下游调用一旦开始，deadline 只能说明结果未知，不能证明未提交；
            # 保留本地租约直至短 TTL 到期，避免错误释放后产生双重所有者。
            if lease is not None and not replayed and not downstream_started:
                self.control_leases.rollback_acquire(
                    lease.lease_id, owner_key_id=lease.owner_key_id)
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent,
                    unknown_reason=("deadline_exceeded" if downstream_started
                                    else None),
                    result=(None if downstream_started else "failed"),
                    possibly_committed=downstream_started,
                    lease=lease):
                return
            if downstream_started:
                self._structured_provider_error(RuntimeProviderOperationError(
                    "deadline_exceeded", category="timeout",
                    retryable=False, possibly_committed=True))
            else:
                self._error(HTTPStatus.GATEWAY_TIMEOUT,
                            "control_deadline_exceeded",
                            "控制请求未在统一处理期限内完成")
            return
        except RuntimeProviderOperationError as error:
            if lease is not None and not replayed and \
                    not error.possibly_committed:
                self.control_leases.rollback_acquire(
                    lease.lease_id, owner_key_id=lease.owner_key_id)
            if error.invalidates_global_operational:
                self._clear_gpio_control_operational(
                    expected_instance_id=(daemon_id if isinstance(
                        daemon_id, str) else None),
                    expected_revision=operational_revision)
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent,
                        unknown_reason=("provider_unavailable" if
                                        error.possibly_committed else None),
                        result=(None if error.possibly_committed else "failed"),
                        possibly_committed=error.possibly_committed,
                        lease=lease):
                    return
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                             "control_operation_ledger_unavailable",
                             "操作账本不可用，Runtime写能力已失败关闭")
            else:
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent,
                        unknown_reason=("downstream_uncertain" if
                                        error.possibly_committed else None),
                        result=(None if error.possibly_committed else
                                ("rejected" if error.code == "target_rejected"
                                 else "failed")),
                        possibly_committed=error.possibly_committed,
                        lease=lease):
                    return
                self._structured_provider_error(error)
            return
        except (ControlLeaseError, RuntimeProviderError) as error:
            if downstream_registered and isinstance(daemon_id, str) and \
                    lease is not None:
                try:
                    call_with_deadline(
                        self.provider.gpio_control_release,  # type: ignore[attr-defined]
                        daemon_id, lease.lease_id, lease.owner_key_id,
                        deadline=self.request_deadline)
                except (RequestDeadlineExceeded, RuntimeProviderError):
                    pass
            if lease is not None and not replayed and not downstream_started:
                self.control_leases.rollback_acquire(
                    lease.lease_id, owner_key_id=lease.owner_key_id)
            try:
                self.request_deadline.check()
            except RequestDeadlineExceeded:
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent,
                        unknown_reason=("deadline_exceeded" if
                                        downstream_started else None),
                        result=(None if downstream_started else "failed"),
                        possibly_committed=downstream_started,
                        lease=lease):
                    return
                if downstream_started:
                    self._structured_provider_error(
                        RuntimeProviderOperationError(
                            "deadline_exceeded", category="timeout",
                            retryable=False, possibly_committed=True))
                else:
                    self._error(HTTPStatus.GATEWAY_TIMEOUT,
                                "control_deadline_exceeded",
                                "控制请求未在统一处理期限内完成")
                return
            if isinstance(error, ControlLeaseError):
                self._clear_gpio_control_operational(
                    expected_revision=operational_revision)
            # IPC v2 尚无结构化业务/传输错误码。RuntimeProviderError 可能只是
            # 错误节点、资源或合同拒绝，不能由任意请求污染全局 operational；
            # 下一次 GET 会独立核验 daemon 身份，失败或换代时再降级。
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent,
                    unknown_reason=("provider_unavailable" if
                                    downstream_started else None),
                    result=(None if downstream_started else "failed"),
                    possibly_committed=downstream_started,
                    lease=lease):
                return
            self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                        "control_lease_unavailable",
                        "无法安全完成控制租约登记")
            return
        if audit_intent is None:
            raise RuntimeError("控制租约成功但缺少持久审计intent")
        if not self._complete_control_audit(
                audit_intent, result="committed",
                possibly_committed=True, lease=lease):
            return
        self._success({"lease": lease.to_dict(), "replayed": replayed},
                      HTTPStatus.OK if replayed else HTTPStatus.CREATED,
                      audit_result="control_lease_replayed" if replayed
                      else "control_lease_acquired")

    def _handle_control_lease_release(self, principal: AuthenticatedPrincipal,
                                      lease_id: str) -> None:
        operational_revision = self._gpio_control_revision()
        daemon_id_before = getattr(
            self.control_leases, "daemon_instance_id", None)
        daemon_id = None
        downstream_started = False
        outcome = None
        operation_owner_key_id = principal.key_id
        foreign_recovery_token: str | None = None
        audit_intent = None

        def bind_foreign_recovery(recovered: dict) -> None:
            if foreign_recovery_token is not None:
                self.server.foreign_operation_recovery.bind_operation(  # type: ignore[attr-defined]
                    principal.key_id, recovered["operation_id"],
                    kind="control_release", lease_id=lease_id,
                    idempotency_key="release:v1",
                    reservation_token=foreign_recovery_token)

        def forget_foreign_recovery() -> None:
            if foreign_recovery_token is not None:
                self.server.foreign_operation_recovery.forget(  # type: ignore[attr-defined]
                    principal.key_id, kind="control_release",
                    lease_id=lease_id, idempotency_key="release:v1",
                    reservation_token=foreign_recovery_token)

        def retain_foreign_recovery() -> None:
            if foreign_recovery_token is not None:
                self.server.foreign_operation_recovery.retain_for_lookup(  # type: ignore[attr-defined]
                    principal.key_id, kind="control_release",
                    lease_id=lease_id, idempotency_key="release:v1",
                    reservation_token=foreign_recovery_token)

        try:
            lease_id = validate_lease_id(lease_id)
            allow_foreign = CONTROL_LEASE_REVOKE_PERMISSION in \
                principal.permissions
            if CONTROL_LEASE_RELEASE_PERMISSION not in \
                    principal.permissions and not allow_foreign:
                self._error(HTTPStatus.FORBIDDEN, "permission_denied",
                            "当前API密钥没有释放控制租约的权限")
                return
            audit_intent = self._begin_control_audit(
                principal, action="release", lease_id=lease_id,
                fields={
                    "lease_id": lease_id,
                    "allow_foreign": allow_foreign,
                })
            if audit_intent is None:
                return
            lease = call_with_deadline(
                self.control_leases.authorize,
                lease_id, requester_key_id=principal.key_id,
                allow_foreign=allow_foreign,
                deadline=self.request_deadline)
            # 管理员可以代为撤销，但账本身份始终属于原租约所有者。
            # 可能提交后的恢复查询必须沿用实际提交时的 owner，不能用
            # 管理员身份派生出另一个 locator。
            operation_owner_key_id = lease.owner_key_id
            if lease.command_group == GPIO_WRITE_COMMAND_GROUP:
                if not self.gpio_control_configured:
                    raise ControlLeaseError("GPIO写控制后端不可用")
                daemon_id = getattr(
                    self.control_leases, "daemon_instance_id", None)
                if not isinstance(daemon_id, str):
                    raise ControlLeaseError("toolbusd实例身份尚未绑定")
                if lease.owner_key_id != principal.key_id:
                    token, reserve_error = self.server.foreign_operation_recovery.reserve(  # type: ignore[attr-defined]
                        principal.key_id, lease.owner_key_id,
                        kind="control_release", lease_id=lease.lease_id,
                        idempotency_key="release:v1")
                    if reserve_error is not None:
                        status = (HTTPStatus.CONFLICT if
                                  reserve_error == "conflict" else
                                  HTTPStatus.SERVICE_UNAVAILABLE)
                        code = ("control_recovery_locator_conflict" if
                                reserve_error == "conflict" else
                                "control_recovery_locator_capacity_exceeded")
                        self._error(
                            status, code,
                            "无法在执行管理员代释放前安全保留恢复定位信息")
                        return
                    if token is None:
                        raise RuntimeError("管理员恢复定位预留未返回token")
                    foreign_recovery_token = token
                # 管理员撤销权限来自服务端认证配置；v1 IPC 不携带可伪造的
                # foreign/admin 位，而是以登记时的真实所有者释放。
                downstream_started = True
                outcome = call_with_deadline(
                    self.provider.gpio_control_release,  # type: ignore[attr-defined]
                    daemon_id, lease.lease_id, lease.owner_key_id,
                    deadline=self.request_deadline)
                bind_foreign_recovery(outcome)
            else:
                call_with_deadline(
                    self.control_leases.release,
                    lease_id, requester_key_id=principal.key_id,
                    allow_foreign=allow_foreign,
                    deadline=self.request_deadline)
        except ValueError as error:
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="rejected",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.BAD_REQUEST, "control_lease_id_invalid",
                        str(error))
            return
        except ControlLeaseNotFound as error:
            daemon_id_after = getattr(
                self.control_leases, "daemon_instance_id", None)
            # 普通的未知或过期租约是调用方状态，不说明 GPIO 后端失效；
            # 只有身份读取发现 daemon 已换代时才撤销既有 operational 证明。
            if daemon_id_before is not None and \
                    daemon_id_after != daemon_id_before:
                self._clear_gpio_control_operational(
                    expected_instance_id=daemon_id_before,
                    expected_revision=operational_revision)
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="rejected",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.NOT_FOUND, "control_lease_not_found",
                        str(error))
            return
        except ControlLeaseOwnershipError as error:
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="rejected",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.FORBIDDEN, "control_lease_not_owner",
                        str(error))
            return
        except RequestDeadlineExceeded:
            if downstream_started:
                recovered = self._try_operation_lookup(
                    operation_owner_key_id, kind="control_release",
                    lease_id=lease_id,
                    idempotency_key="release:v1")
                if recovered is not None:
                    bind_foreign_recovery(recovered)
                    if audit_intent is not None and not \
                            self._complete_operation_audit(
                                audit_intent, recovered, action="release"):
                        return
                    self._finish_local_release_if_safe(
                        recovered, lease_id, principal, allow_foreign)
                    self._send_operation(
                        recovered, audit_result="control_release_recovered",
                        principal=principal)
                else:
                    retain_foreign_recovery()
                    if audit_intent is not None and not \
                            self._complete_control_audit(
                                audit_intent,
                                unknown_reason="deadline_exceeded",
                                possibly_committed=True):
                        return
                    self._operation_uncertain(
                        kind="control_release", lease_id=lease_id,
                        idempotency_key="release:v1")
            else:
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent, result="failed",
                        possibly_committed=False):
                    return
                self._error(HTTPStatus.GATEWAY_TIMEOUT,
                            "control_deadline_exceeded",
                            "控制请求未在统一处理期限内完成")
            return
        except RuntimeProviderOperationError as error:
            if error.invalidates_global_operational:
                if error.possibly_committed:
                    retain_foreign_recovery()
                else:
                    forget_foreign_recovery()
                self._clear_gpio_control_operational(
                    expected_instance_id=(daemon_id if isinstance(
                        daemon_id, str) else daemon_id_before),
                    expected_revision=operational_revision)
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent,
                        unknown_reason=("provider_unavailable" if
                                        error.possibly_committed else None),
                        result=(None if error.possibly_committed else "failed"),
                        possibly_committed=error.possibly_committed):
                    return
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                             "control_operation_ledger_unavailable",
                            "操作账本不可用，Runtime写能力已失败关闭")
            elif error.possibly_committed:
                recovered = self._try_operation_lookup(
                    operation_owner_key_id, kind="control_release",
                    lease_id=lease_id,
                    idempotency_key="release:v1")
                if recovered is not None:
                    bind_foreign_recovery(recovered)
                    if audit_intent is not None and not \
                            self._complete_operation_audit(
                                audit_intent, recovered, action="release"):
                        return
                    self._finish_local_release_if_safe(
                        recovered, lease_id, principal, allow_foreign)
                    self._send_operation(
                        recovered, audit_result="control_release_recovered",
                        principal=principal)
                else:
                    retain_foreign_recovery()
                    if audit_intent is not None and not \
                            self._complete_control_audit(
                                audit_intent,
                                unknown_reason="downstream_uncertain",
                                possibly_committed=True):
                        return
                    self._operation_uncertain(
                        kind="control_release", lease_id=lease_id,
                        idempotency_key="release:v1")
            else:
                forget_foreign_recovery()
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent,
                        result=("rejected" if error.code == "target_rejected"
                                else "failed"),
                        possibly_committed=False):
                    return
                self._structured_provider_error(error)
            return
        except (ControlLeaseError, RuntimeProviderError):
            try:
                self.request_deadline.check()
            except RequestDeadlineExceeded:
                if downstream_started:
                    recovered = self._try_operation_lookup(
                        operation_owner_key_id, kind="control_release",
                        lease_id=lease_id,
                        idempotency_key="release:v1")
                    if recovered is not None:
                        bind_foreign_recovery(recovered)
                        if audit_intent is not None and not \
                                self._complete_operation_audit(
                                    audit_intent, recovered,
                                    action="release"):
                            return
                        self._finish_local_release_if_safe(
                            recovered, lease_id, principal, allow_foreign)
                        self._send_operation(
                            recovered,
                            audit_result="control_release_recovered",
                            principal=principal)
                    else:
                        retain_foreign_recovery()
                        if audit_intent is not None and not \
                                self._complete_control_audit(
                                    audit_intent,
                                    unknown_reason="deadline_exceeded",
                                    possibly_committed=True):
                            return
                        self._operation_uncertain(
                            kind="control_release", lease_id=lease_id,
                            idempotency_key="release:v1")
                else:
                    forget_foreign_recovery()
                    if audit_intent is not None and not \
                            self._complete_control_audit(
                                audit_intent, result="failed",
                                possibly_committed=False):
                        return
                    self._error(HTTPStatus.GATEWAY_TIMEOUT,
                                "control_deadline_exceeded",
                                "控制请求未在统一处理期限内完成")
                return
            if downstream_started:
                recovered = self._try_operation_lookup(
                    operation_owner_key_id, kind="control_release",
                    lease_id=lease_id,
                    idempotency_key="release:v1")
                if recovered is not None:
                    bind_foreign_recovery(recovered)
                    if audit_intent is not None and not \
                            self._complete_operation_audit(
                                audit_intent, recovered, action="release"):
                        return
                    self._finish_local_release_if_safe(
                        recovered, lease_id, principal, allow_foreign)
                    self._send_operation(
                        recovered, audit_result="control_release_recovered",
                        principal=principal)
                else:
                    retain_foreign_recovery()
                    if audit_intent is not None and not \
                            self._complete_control_audit(
                                audit_intent,
                                unknown_reason="provider_unavailable",
                                possibly_committed=True):
                        return
                    self._operation_uncertain(
                        kind="control_release", lease_id=lease_id,
                        idempotency_key="release:v1")
            else:
                forget_foreign_recovery()
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent, result="failed",
                        possibly_committed=False):
                    return
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "control_lease_unavailable",
                            "无法安全完成控制租约释放")
            return
        if outcome is None:
            if audit_intent is None:
                raise RuntimeError("控制租约释放成功但缺少持久审计intent")
            if not self._complete_control_audit(
                    audit_intent, result="released",
                    possibly_committed=True):
                return
            self._send_empty(HTTPStatus.NO_CONTENT,
                             audit_result="control_lease_released")
        else:
            if audit_intent is None:
                raise RuntimeError("控制租约释放成功但缺少持久审计intent")
            if not self._complete_operation_audit(
                    audit_intent, outcome, action="release"):
                return
            self._finish_local_release_if_safe(
                outcome, lease_id, principal, allow_foreign)
            self._send_operation(
                outcome, audit_result="control_release", principal=principal)

    def _handle_gpio_write(self, principal: AuthenticatedPrincipal) -> None:
        value = self._read_control_json()
        if value is None:
            return
        expected = {"lease_id", "node_id", "resource_id",
                    "idempotency_key", "value"}
        if set(value) != expected:
            self._error(HTTPStatus.BAD_REQUEST, "request_body_invalid",
                        "GPIO写字段必须且只能包含lease_id、node_id、"
                        "resource_id、idempotency_key和value")
            return
        operational_revision = self._gpio_control_revision()
        daemon_id_before = getattr(
            self.control_leases, "daemon_instance_id", None)
        daemon_id = None
        downstream_started = False
        result = None
        audit_intent = None
        try:
            lease_id = validate_lease_id(value["lease_id"])
            node_id = validate_control_id(value["node_id"], "node_id")
            resource_id = validate_control_id(
                value["resource_id"], "resource_id")
            idempotency_key = validate_idempotency_key(
                value["idempotency_key"])
            desired = value["value"]
            if type(desired) is not bool:
                raise ValueError("value必须是布尔值")
            audit_intent = self._begin_control_audit(
                principal, action="gpio_write", lease_id=lease_id,
                fields={
                    "lease_id": lease_id,
                    "node_id": node_id,
                    "resource_id": resource_id,
                    "idempotency_key": idempotency_key,
                    "value": desired,
                })
            if audit_intent is None:
                return
            lease = call_with_deadline(
                self.control_leases.authorize, lease_id,
                requester_key_id=principal.key_id,
                deadline=self.request_deadline)
            if lease.command_group != GPIO_WRITE_COMMAND_GROUP or \
                    lease.node_id != node_id or \
                    lease.resource_id != resource_id:
                raise ControlLeaseConflict("控制租约范围与GPIO写请求不匹配")
            daemon_id = getattr(
                self.control_leases, "daemon_instance_id", None)
            if not isinstance(daemon_id, str):
                raise ControlLeaseError("toolbusd实例身份尚未绑定")
            downstream_started = True
            result = call_with_deadline(
                self.provider.gpio_control_write,  # type: ignore[attr-defined]
                daemon_id, lease_id, principal.key_id, node_id,
                resource_id, idempotency_key, desired,
                deadline=self.request_deadline)
        except ValueError as error:
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="rejected",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.BAD_REQUEST, "request_body_invalid",
                        str(error))
            return
        except ControlLeaseNotFound as error:
            daemon_id_after = getattr(
                self.control_leases, "daemon_instance_id", None)
            if daemon_id_before is not None and \
                    daemon_id_after != daemon_id_before:
                self._clear_gpio_control_operational(
                    expected_instance_id=daemon_id_before,
                    expected_revision=operational_revision)
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="rejected",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.NOT_FOUND, "control_lease_not_found",
                        str(error))
            return
        except ControlLeaseOwnershipError as error:
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="rejected",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.FORBIDDEN, "control_lease_not_owner",
                        str(error))
            return
        except ControlLeaseConflict as error:
            if audit_intent is not None and not self._complete_control_audit(
                    audit_intent, result="rejected",
                    possibly_committed=False):
                return
            self._error(HTTPStatus.CONFLICT, "control_lease_conflict",
                        str(error))
            return
        except RequestDeadlineExceeded:
            if downstream_started:
                recovered = self._try_operation_lookup(
                    principal.key_id, kind="gpio_write", lease_id=lease_id,
                    idempotency_key=idempotency_key)
                if recovered is not None:
                    if audit_intent is not None and not \
                            self._complete_operation_audit(
                                audit_intent, recovered,
                                action="gpio_write"):
                        return
                    self._send_operation(
                        recovered, audit_result="gpio_write_recovered",
                        principal=principal)
                else:
                    if audit_intent is not None and not \
                            self._complete_control_audit(
                                audit_intent,
                                unknown_reason="deadline_exceeded",
                                possibly_committed=True):
                        return
                    self._operation_uncertain(
                        kind="gpio_write", lease_id=lease_id,
                        idempotency_key=idempotency_key)
            else:
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent, result="failed",
                        possibly_committed=False):
                    return
                self._error(HTTPStatus.GATEWAY_TIMEOUT,
                            "control_deadline_exceeded",
                            "控制请求未在统一处理期限内完成")
            return
        except RuntimeProviderOperationError as error:
            if error.invalidates_global_operational:
                self._clear_gpio_control_operational(
                    expected_instance_id=(daemon_id if isinstance(
                        daemon_id, str) else daemon_id_before),
                    expected_revision=operational_revision)
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent,
                        unknown_reason=("provider_unavailable" if
                                        error.possibly_committed else None),
                        result=(None if error.possibly_committed else "failed"),
                        possibly_committed=error.possibly_committed):
                    return
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                             "control_operation_ledger_unavailable",
                            "操作账本不可用，Runtime写能力已失败关闭")
            elif error.possibly_committed:
                recovered = self._try_operation_lookup(
                    principal.key_id, kind="gpio_write", lease_id=lease_id,
                    idempotency_key=idempotency_key)
                if recovered is not None:
                    if audit_intent is not None and not \
                            self._complete_operation_audit(
                                audit_intent, recovered,
                                action="gpio_write"):
                        return
                    self._send_operation(
                        recovered, audit_result="gpio_write_recovered",
                        principal=principal)
                else:
                    if audit_intent is not None and not \
                            self._complete_control_audit(
                                audit_intent,
                                unknown_reason="downstream_uncertain",
                                possibly_committed=True):
                        return
                    self._operation_uncertain(
                        kind="gpio_write", lease_id=lease_id,
                        idempotency_key=idempotency_key)
            else:
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent,
                        result=("rejected" if error.code == "target_rejected"
                                else "failed"),
                        possibly_committed=False):
                    return
                self._structured_provider_error(error)
            return
        except (ControlLeaseError, RuntimeProviderError):
            try:
                self.request_deadline.check()
            except RequestDeadlineExceeded:
                if downstream_started:
                    recovered = self._try_operation_lookup(
                        principal.key_id, kind="gpio_write",
                        lease_id=lease_id,
                        idempotency_key=idempotency_key)
                    if recovered is not None:
                        if audit_intent is not None and not \
                                self._complete_operation_audit(
                                    audit_intent, recovered,
                                    action="gpio_write"):
                            return
                        self._send_operation(
                            recovered, audit_result="gpio_write_recovered",
                            principal=principal)
                    else:
                        if audit_intent is not None and not \
                                self._complete_control_audit(
                                    audit_intent,
                                    unknown_reason="deadline_exceeded",
                                    possibly_committed=True):
                            return
                        self._operation_uncertain(
                            kind="gpio_write", lease_id=lease_id,
                            idempotency_key=idempotency_key)
                else:
                    if audit_intent is not None and not \
                            self._complete_control_audit(
                                audit_intent, result="failed",
                                possibly_committed=False):
                        return
                    self._error(HTTPStatus.GATEWAY_TIMEOUT,
                                "control_deadline_exceeded",
                                "控制请求未在统一处理期限内完成")
                return
            if downstream_started:
                recovered = self._try_operation_lookup(
                    principal.key_id, kind="gpio_write", lease_id=lease_id,
                    idempotency_key=idempotency_key)
                if recovered is not None:
                    if audit_intent is not None and not \
                            self._complete_operation_audit(
                                audit_intent, recovered,
                                action="gpio_write"):
                        return
                    self._send_operation(
                        recovered, audit_result="gpio_write_recovered",
                        principal=principal)
                else:
                    if audit_intent is not None and not \
                            self._complete_control_audit(
                                audit_intent,
                                unknown_reason="provider_unavailable",
                                possibly_committed=True):
                        return
                    self._operation_uncertain(
                        kind="gpio_write", lease_id=lease_id,
                        idempotency_key=idempotency_key)
            else:
                if audit_intent is not None and not self._complete_control_audit(
                        audit_intent, result="failed",
                        possibly_committed=False):
                    return
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "gpio_control_unavailable",
                            "GPIO写入未获得可验证的下游成功结果")
            return
        if audit_intent is None:
            raise RuntimeError("GPIO写成功但缺少持久审计intent")
        if not self._complete_operation_audit(
                audit_intent, result, action="gpio_write"):
            return
        self._send_operation(
            result, audit_result="gpio_write", principal=principal)

    def _snapshot(self) -> SnapshotRead | None:
        try:
            read = call_with_deadline(
                self.provider.read_snapshot, deadline=self.request_deadline)
        except RequestDeadlineExceeded:
            self._error(HTTPStatus.GATEWAY_TIMEOUT,
                        "request_deadline_exceeded",
                        "Runtime读取未在统一处理期限内完成")
            return None
        except RuntimeProviderError:
            self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                        "provider_unavailable",
                        "Runtime数据源暂时不可用")
            return None
        self.event_log.observe(read.snapshot)
        return read

    def _runtime_capabilities(self) -> dict:
        return self.provider.runtime_capabilities()

    def _toolbusd_health(self) -> dict:
        reader = getattr(self.provider, "health_snapshot", None)
        if not callable(reader):
            return {"available": False, "snapshot": None,
                    "reason": "unsupported"}
        try:
            snapshot = call_with_deadline(
                reader, deadline=self.request_deadline)
        except RequestDeadlineExceeded:
            raise
        except RuntimeProviderError:
            # 健康遥测失败与 Runtime 资源快照隔离，且不向 HTTP 泄漏后端细节。
            return {"available": False, "snapshot": None,
                    "reason": "temporarily_unavailable"}
        return {"available": True, "snapshot": snapshot, "reason": None}

    @staticmethod
    def _node_summary(node: dict, alerts: list[dict]) -> dict:
        return {
            key: node[key] for key in (
                "node_id", "board_type", "display_name", "state",
                "last_seen_ms", "links")
        } | {
            "resource_count": len(node["resources"]),
            "active_alert_count": sum(
                alert["active"] and alert["node_id"] == node["node_id"]
                for alert in alerts),
        }

    def _handle_read(self) -> None:
        parsed = self._parse_request_target(allow_event_query=True)
        if parsed is None:
            return
        parts = self._path_parts(parsed.path)
        if parts is None:
            return

        if self._send_dashboard_asset(parsed.path):
            return

        api_path = len(parts) >= 2 and parts[:2] == ["api", API_VERSION]
        if len(parts) == 5 and parts[:4] == [
                "api", API_VERSION, "control", "operations"]:
            if self.authenticator is None:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "control_authentication_disabled",
                            "操作查询要求启用API密钥认证")
                return
            principal = self._authorize(
                parsed.path, public_health=False,
                required_permission=CONTROL_OPERATION_READ_PERMISSION)
            if not isinstance(principal, AuthenticatedPrincipal):
                return
            self._handle_operation_status(principal, parts[4])
            return
        if api_path:
            auth_context = self._authorize(
                parsed.path, public_health=self.command in ("GET", "HEAD"),
                required_permission=RUNTIME_READ_PERMISSION)
            if auth_context is None:
                return
            if auth_context == "public_health":
                self._success({"status": "ok", "scope": "liveness"},
                              audit_result="public_probe")
                return

        if parts == ["api", API_VERSION, "overview", "stream"]:
            if self.command == "HEAD":
                self._send_empty(HTTPStatus.METHOD_NOT_ALLOWED,
                                 headers={"Allow": "GET"},
                                 audit_result="method_not_allowed")
                return
            self._handle_overview_stream()
            return

        if parts == ["api", API_VERSION, "alert-rules"]:
            self._success(self.alert_rules.snapshot())
            return

        if parts == ["api", API_VERSION, "operations"]:
            reader = getattr(self.provider, "operational_status", None)
            toolbusd = {"availability": "unknown", "connection": "unknown"}
            if callable(reader):
                try:
                    toolbusd = call_with_deadline(
                        reader, deadline=self.request_deadline)
                except (RuntimeProviderError, RequestDeadlineExceeded):
                    toolbusd = {"availability": "unavailable",
                                "connection": "unavailable"}
            self._success(self.server.operational_state.snapshot(  # type: ignore[attr-defined]
                stream_limit=self.server.overview_stream_maximum_connections,  # type: ignore[attr-defined]
                trend=self.server.runtime_dashboard.storage_status(),  # type: ignore[attr-defined]
                toolbusd=toolbusd))
            return

        if parts == ["api", API_VERSION]:
            gpio_backend_operational = self._gpio_backend_operational()
            control_audit = self._control_audit_health()
            gpio_control_operational = bool(
                gpio_backend_operational and control_audit["operational"])
            self._success({
                "snapshot_schema_version": RUNTIME_SNAPSHOT_SCHEMA_VERSION,
                "capabilities": {
                    "read_only": not self.control_mutation_available,
                    "write_commands": (
                        gpio_control_operational and
                        self.control_mutation_available),
                    "control_audit": control_audit,
                    "gpio_write": {
                        "configured": self.gpio_control_configured,
                        "operational": gpio_control_operational,
                    },
                    "operation_ledger": {
                        "configured": self.gpio_control_configured,
                        "operational": gpio_backend_operational,
                        "schema_version": 1,
                    },
                    "control_leases": {
                        "available": self.control_leases_available,
                        "mutation_available":
                            self.control_mutation_available,
                        "schema_version": CONTROL_LEASE_SCHEMA_VERSION,
                        "maximum_active": self.control_leases.capacity,
                        "backend_binding":
                            "toolbusd_instance" if isinstance(
                                self.control_leases,
                                DaemonBoundControlLeaseManager)
                            else "runtime_process",
                        "downstream_commands": gpio_control_operational,
                        "loopback_only": True,
                    },
                    "authentication": self.authenticator is not None,
                    "authentication_mode": (
                        "api_key" if self.authenticator is not None
                        else "disabled_loopback"),
                    "event_stream": True,
                    "overview_stream": {
                        "available": True,
                        "transport": "sse",
                        "path": f"/api/{API_VERSION}/overview/stream",
                        "maximum_connections": self.server.overview_stream_maximum_connections,  # type: ignore[attr-defined]
                        "queue_capacity_per_connection":
                            OVERVIEW_STREAM_QUEUE_CAPACITY,
                    },
                    "incremental_events": {
                        "available": True,
                        "schema_version": EVENT_SCHEMA_VERSION,
                        "transport": "short_poll",
                        "maximum_page_size": MAXIMUM_EVENT_PAGE_LIMIT,
                        "capacity": self.event_log.capacity,
                    },
                    **self._runtime_capabilities(),
                },
                "endpoints": ["health", "snapshot", "overview", "overview/stream", "operations", "nodes", "resources",
                              "alerts", "events", "control-leases",
                              "control/operations/{operation_id}",
                              "control/operation-lookups"] +
                             (["control/gpio/write"]
                              if self.gpio_control_configured else []),
            })
            return
        if parts == ["api", API_VERSION, "health"]:
            read = self._snapshot()
            if read is not None:
                snapshot = read.snapshot
                try:
                    toolbusd_health = self._toolbusd_health()
                except RequestDeadlineExceeded:
                    self._error(HTTPStatus.GATEWAY_TIMEOUT,
                                "request_deadline_exceeded",
                                "健康读取未在统一处理期限内完成")
                    return
                self._success({"status": "ok",
                               "snapshot_id": snapshot["snapshot_id"],
                               "capabilities":
                                   self._runtime_capabilities(),
                               "toolbusd_health": toolbusd_health,
                               "runtime_control_audit":
                                   self._control_audit_health()},
                              read=read)
            return
        if parts == ["api", API_VERSION, "events"]:
            parameters = self._event_parameters()
            if parameters is None:
                return
            cursor, limit = parameters
            if cursor is not None:
                try:
                    self.event_log.validate_cursor_incarnation(cursor)
                except ExpiredEventCursor as error:
                    self._error(
                        HTTPStatus.CONFLICT, "event_cursor_expired",
                        str(error),
                        details={"reset_cursor": error.reset_cursor})
                    return
            read = self._snapshot()
            if read is None:
                return
            try:
                page = self.event_log.read_page(cursor, limit)
            except ExpiredEventCursor as error:
                self._error(
                    HTTPStatus.CONFLICT, "event_cursor_expired", str(error),
                    details={"reset_cursor": error.reset_cursor})
                return
            except InvalidEventCursor as error:
                self._error(HTTPStatus.BAD_REQUEST, "event_cursor_invalid",
                            str(error))
                return
            except EventLogError as error:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "event_log_unavailable", str(error))
                return
            self._success({
                "event_schema_version": EVENT_SCHEMA_VERSION,
                "events": [event.to_dict() for event in page.events],
                "next_cursor": page.next_cursor,
                "has_more": page.has_more,
            }, read=read)
            return
        if len(parts) < 3 or parts[:2] != ["api", API_VERSION]:
            self._error(HTTPStatus.NOT_FOUND, "not_found", "API路径不存在")
            return

        read = self._snapshot()
        if read is None:
            return
        snapshot = read.snapshot
        endpoint = parts[2]
        if parts == ["api", API_VERSION, "snapshot"]:
            self._success(snapshot, read=read)
            return
        if parts == ["api", API_VERSION, "overview"]:
            try:
                toolbusd_health = self._toolbusd_health()
            except RequestDeadlineExceeded:
                toolbusd_health = {"available": False, "snapshot": None,
                                   "reason": "deadline_exceeded"}
            dashboard = self.server.runtime_dashboard.observe(  # type: ignore[attr-defined]
                snapshot, toolbusd_health)
            self._success(dashboard, read=read)
            return
        if parts == ["api", API_VERSION, "nodes"]:
            self._success([
                self._node_summary(node, snapshot["alerts"])
                for node in snapshot["nodes"]], read=read)
            return
        if parts == ["api", API_VERSION, "resources"]:
            self._success([
                {"node_id": node["node_id"], **resource}
                for node in snapshot["nodes"]
                for resource in node["resources"]], read=read)
            return
        if parts == ["api", API_VERSION, "alerts"]:
            self._success(snapshot["alerts"], read=read)
            return
        if endpoint == "nodes" and len(parts) in (4, 5):
            node_id = parts[3]
            node = next((item for item in snapshot["nodes"]
                         if item["node_id"] == node_id), None)
            if node is None:
                self._error(HTTPStatus.NOT_FOUND, "node_not_found",
                            f"节点不存在：{node_id}")
                return
            if len(parts) == 4:
                self._success(node, read=read)
                return
            if parts[4] == "resources":
                self._success(node["resources"], read=read)
                return
            if parts[4] == "alerts":
                self._success([alert for alert in snapshot["alerts"]
                               if alert["node_id"] == node_id], read=read)
                return
        self._error(HTTPStatus.NOT_FOUND, "not_found", "API路径不存在")

    def do_GET(self) -> None:  # noqa: N802
        self._begin_request_audit()
        self._handle_read()

    def do_HEAD(self) -> None:  # noqa: N802
        self._begin_request_audit()
        self._handle_read()

    def do_POST(self) -> None:  # noqa: N802
        self._begin_request_audit()
        parsed = self._parse_request_target()
        if parsed is None:
            return
        parts = self._path_parts(parsed.path)
        if parts is None:
            return
        if parts in (["api", API_VERSION, "alert-rules", "preflight"],
                     ["api", API_VERSION, "alert-rules", "apply"]):
            if self.authenticator is None:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "alert_rule_authentication_disabled",
                            "告警规则更新要求启用API密钥认证")
                return
            principal = self._authorize(
                parsed.path, public_health=False,
                required_permission=ALERT_RULE_WRITE_PERMISSION)
            if not isinstance(principal, AuthenticatedPrincipal):
                return
            body = self._read_control_json()
            if body is None:
                return
            try:
                if parts[-1] == "preflight":
                    if set(body) != {"expected_revision", "rules"}:
                        raise AlertRuleError("预检请求字段不合法")
                    result = self.alert_rules.preflight(
                        body["expected_revision"], body["rules"])
                else:
                    if set(body) != {"confirmation_token", "confirmation"}:
                        raise AlertRuleError("应用请求字段不合法")
                    result = self.alert_rules.apply(
                        body["confirmation_token"], body["confirmation"])
            except AlertRuleStorageError:
                # 存储异常可能包含本地路径、文件名或系统错误细节。
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "alert_rule_storage_unavailable",
                            "告警规则存储暂不可用；规则未生效")
                return
            except AlertRuleError as error:
                self._error(HTTPStatus.CONFLICT, "alert_rule_update_rejected",
                            str(error))
                return
            self._success(result)
            return
        if self.command == "POST" and \
                parts == ["api", API_VERSION, "control-leases"]:
            if self.authenticator is None:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "control_authentication_disabled",
                            "控制租约要求启用API密钥认证")
                return
            if not self.control_leases_available:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "control_transport_insecure",
                            "控制租约只允许在回环监听上使用")
                return
            principal = self._authorize(
                parsed.path, public_health=False,
                required_permission=CONTROL_LEASE_ACQUIRE_PERMISSION)
            if not isinstance(principal, AuthenticatedPrincipal):
                return
            self._handle_control_lease_acquire(principal)
            return
        if self.command == "POST" and parts == [
                "api", API_VERSION, "control", "gpio", "write"]:
            if self.authenticator is None:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "control_authentication_disabled",
                            "GPIO写控制要求启用API密钥认证")
                return
            principal = self._authorize(
                parsed.path, public_health=False,
                required_permission=GPIO_WRITE_PERMISSION)
            if not isinstance(principal, AuthenticatedPrincipal):
                return
            if not self.gpio_control_configured:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "gpio_control_unavailable",
                            "GPIO写控制仅在认证回环与toolbusd安全IPC可用时开放")
                return
            self._handle_gpio_write(principal)
            return
        if self.command == "POST" and parts == [
                "api", API_VERSION, "control", "operation-lookups"]:
            if self.authenticator is None:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "control_authentication_disabled",
                            "操作定位查询要求启用API密钥认证")
                return
            principal = self._authorize(
                parsed.path, public_health=False,
                required_permission=CONTROL_OPERATION_READ_PERMISSION)
            if not isinstance(principal, AuthenticatedPrincipal):
                return
            self._handle_operation_lookup(principal)
            return
        if len(parts) >= 2 and parts[:2] == ["api", API_VERSION]:
            if self._authorize(parsed.path, public_health=False,
                               required_permission=None) is None:
                return
        self._error(HTTPStatus.METHOD_NOT_ALLOWED, "read_only",
                    "该路径不接受写请求",
                    headers={"Allow": "GET, HEAD, OPTIONS"})

    do_PUT = do_POST  # type: ignore[assignment]
    do_PATCH = do_POST  # type: ignore[assignment]

    def do_DELETE(self) -> None:  # noqa: N802
        self._begin_request_audit()
        parsed = self._parse_request_target()
        if parsed is None:
            return
        parts = self._path_parts(parsed.path)
        if parts is None:
            return
        if len(parts) == 4 and parts[:3] == [
                "api", API_VERSION, "control-leases"]:
            if self.authenticator is None:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "control_authentication_disabled",
                            "控制租约要求启用API密钥认证")
                return
            if not self.control_leases_available:
                self._error(HTTPStatus.SERVICE_UNAVAILABLE,
                            "control_transport_insecure",
                            "控制租约只允许在回环监听上使用")
                return
            principal = self._authorize(
                parsed.path, public_health=False, required_permission=None)
            if not isinstance(principal, AuthenticatedPrincipal):
                return
            self._handle_control_lease_release(principal, parts[3])
            return
        if len(parts) >= 2 and parts[:2] == ["api", API_VERSION]:
            if self._authorize(parsed.path, public_health=False,
                               required_permission=None) is None:
                return
        self._error(HTTPStatus.METHOD_NOT_ALLOWED, "read_only",
                    "该路径不接受写请求",
                    headers={"Allow": "GET, HEAD, OPTIONS"})

    def do_OPTIONS(self) -> None:  # noqa: N802
        self._begin_request_audit()
        parsed = self._parse_request_target()
        if parsed is None:
            return
        parts = self._path_parts(parsed.path)
        if parts is None:
            return
        if len(parts) >= 2 and parts[:2] == ["api", API_VERSION]:
            if parts == ["api", API_VERSION, "control-leases"]:
                self._send_empty(HTTPStatus.NO_CONTENT,
                                 headers={"Allow": "POST, OPTIONS"},
                                 audit_result="options")
                return
            if len(parts) == 4 and parts[:3] == [
                    "api", API_VERSION, "control-leases"]:
                self._send_empty(HTTPStatus.NO_CONTENT,
                                 headers={"Allow": "DELETE, OPTIONS"},
                                 audit_result="options")
                return
            if parts == ["api", API_VERSION, "control", "gpio", "write"]:
                self._send_empty(HTTPStatus.NO_CONTENT,
                                 headers={"Allow": "POST, OPTIONS"},
                                 audit_result="options")
                return
            if parts == ["api", API_VERSION, "control",
                         "operation-lookups"]:
                self._send_empty(HTTPStatus.NO_CONTENT,
                                 headers={"Allow": "POST, OPTIONS"},
                                 audit_result="options")
                return
            if len(parts) == 5 and parts[:4] == [
                    "api", API_VERSION, "control", "operations"]:
                self._send_empty(HTTPStatus.NO_CONTENT,
                                 headers={"Allow": "GET, HEAD, OPTIONS"},
                                 audit_result="options")
                return
            self._send_empty(HTTPStatus.NO_CONTENT,
                             headers={"Allow": "GET, HEAD, OPTIONS"},
                             audit_result="options")
            return
        self._error(HTTPStatus.NOT_FOUND, "not_found", "API路径不存在")

    def log_message(self, format: str, *args: object) -> None:
        if args and str(args[1]).startswith(("4", "5")):
            # 请求目标可能含被拒绝的查询参数，不能把密钥写入日志。
            sys.stderr.write(f"Runtime HTTP请求失败：{args[1]}\n")


def make_server(host: str, port: int,
                provider: RuntimeProvider, *,
                maximum_workers: int = 32,
                authenticator: ApiKeyAuthenticator | None = None,
                audit_capacity: int = DEFAULT_AUDIT_CAPACITY,
                audit_output: Callable[[dict], None] | None = None,
                event_capacity: int = DEFAULT_EVENT_CAPACITY,
                event_incarnation: str | None = None,
                control_lease_capacity: int = DEFAULT_CONTROL_LEASE_CAPACITY,
                control_lease_manager: ControlLeaseManager | None = None,
                control_audit_journal: ControlAuditJournal | None = None,
                request_io_timeout_seconds: float =
                DEFAULT_REQUEST_IO_TIMEOUT_SECONDS,
                overview_stream_connections: int =
                DEFAULT_OVERVIEW_STREAM_CONNECTIONS,
                overview_stream_interval_seconds: float =
                DEFAULT_OVERVIEW_STREAM_INTERVAL_SECONDS,
                runtime_dashboard: RuntimeDashboard | None = None,
                alert_rules: AlertRuleManager | None = None,
                ) -> ThreadingHTTPServer:
    try:
        loopback = ipaddress.ip_address(host).is_loopback
    except ValueError as error:
        raise ValueError("--host必须是数字IP地址；不能使用主机名") from error
    if not loopback and authenticator is None:
        raise ValueError("非回环监听必须配置API密钥认证")
    if control_lease_manager is not None and \
            control_lease_capacity != DEFAULT_CONTROL_LEASE_CAPACITY:
        raise ValueError("不能同时注入控制租约管理器和非默认容量")
    if type(overview_stream_connections) is not int or not \
            1 <= overview_stream_connections <= \
            MAXIMUM_OVERVIEW_STREAM_CONNECTIONS:
        raise ValueError("主动推送连接数必须位于1～32")
    if isinstance(overview_stream_interval_seconds, bool) or not isinstance(
            overview_stream_interval_seconds, (int, float)) or not \
            MINIMUM_OVERVIEW_STREAM_INTERVAL_SECONDS <= \
            overview_stream_interval_seconds <= \
            MAXIMUM_OVERVIEW_STREAM_INTERVAL_SECONDS:
        raise ValueError("主动推送间隔必须位于0.1～30秒")
    resolved_control_leases = control_lease_manager or \
        ControlLeaseManager(control_lease_capacity)
    event_log = RuntimeEventLog(
        event_capacity, incarnation=event_incarnation)
    server_type = IPv6ThreadingHTTPServer if ":" in host \
        else BoundedThreadingHTTPServer
    server = server_type((host, port), RuntimeRequestHandler,
                         maximum_workers=maximum_workers,
                         request_io_timeout_seconds=
                         request_io_timeout_seconds)
    server.provider = provider  # type: ignore[attr-defined]
    server.authenticator = authenticator  # type: ignore[attr-defined]
    server.audit_sink = BoundedAuditSink(  # type: ignore[attr-defined]
        capacity=audit_capacity, output=audit_output)
    server.event_log = event_log  # type: ignore[attr-defined]
    resolved_alert_rules = alert_rules or AlertRuleManager()
    server.alert_rules = resolved_alert_rules  # type: ignore[attr-defined]
    server.runtime_dashboard = runtime_dashboard or RuntimeDashboard(  # type: ignore[attr-defined]
        alert_rules=resolved_alert_rules)
    server.operational_state = RuntimeOperationalState()  # type: ignore[attr-defined]
    server.overview_stream_maximum_connections = overview_stream_connections  # type: ignore[attr-defined]
    server.overview_stream_slots = threading.BoundedSemaphore(  # type: ignore[attr-defined]
        overview_stream_connections)
    server.overview_stream_interval_seconds = float(  # type: ignore[attr-defined]
        overview_stream_interval_seconds)
    server.control_leases = resolved_control_leases  # type: ignore[attr-defined]
    server.control_audit_journal = control_audit_journal  # type: ignore[attr-defined]
    server.control_audit_state_lock = threading.Lock()  # type: ignore[attr-defined]
    server.control_audit_failed = False  # type: ignore[attr-defined]
    server.control_leases_available = (  # type: ignore[attr-defined]
        loopback and authenticator is not None)
    server.gpio_control_configured = (  # type: ignore[attr-defined]
        loopback and authenticator is not None and
        isinstance(resolved_control_leases, DaemonBoundControlLeaseManager) and
        bool(getattr(provider, "gpio_control_available", False)))
    # operational 必须由一次成功的 daemon 最终准入证明；旧 daemon 或
    # 不完整 IPC 仅能处于 configured，不能在根能力中虚报可执行。
    server.gpio_control_state_lock = threading.Lock()  # type: ignore[attr-defined]
    server.gpio_control_state_revision = 0  # type: ignore[attr-defined]
    server.gpio_control_admitted_daemon_id = None  # type: ignore[attr-defined]
    server.gpio_control_operational = False  # type: ignore[attr-defined]
    # 仅作HTTP快速拒绝/展示缓存；持久、权威的scope阻断始终由toolbusd执行。
    server.operation_scope_lock = threading.Lock()  # type: ignore[attr-defined]
    server.blocked_operation_scopes = set()  # type: ignore[attr-defined]
    server.operation_scope_capacity = 256  # type: ignore[attr-defined]
    # 管理员代释放时，daemon账本仍以原租约owner鉴权。该索引只在服务端
    # 有界保留调用管理员到真实owner的关联，HTTP locator从不携带owner。
    server.foreign_operation_recovery = _ForeignOperationRecoveryIndex()  # type: ignore[attr-defined]
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1",
                        help="监听数字IP地址；非回环地址必须配置API密钥")
    parser.add_argument("--port", type=int, default=8780, help="监听端口")
    parser.add_argument("--api-key-file", type=Path,
                        help="版本化API密钥JSON文件；不支持命令行明文密钥")
    parser.add_argument(
        "--control-audit-dir", type=Path,
        help="持久控制审计目录；必须与--control-audit-key-file成对配置")
    parser.add_argument(
        "--control-audit-key-file", type=Path,
        help="32～64字节原始控制审计HMAC密钥文件；不接受命令行明文密钥")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--snapshot", type=Path,
                        help="Runtime v1 快照JSON；省略数据源时使用内置Mock")
    source.add_argument("--toolbusd-socket", type=Path,
                        help="通过remote-cli连接的toolbusd本地套接字")
    parser.add_argument("--remote-cli",
                        help="remote-cli可执行文件，默认从PATH查找")
    parser.add_argument("--ipc-timeout-ms", type=int, default=2000,
                        help="每次只读IPC调用超时，默认2000毫秒")
    parser.add_argument("--snapshot-cache-ms", type=int, default=250,
                        help="toolbusd快照短时缓存，默认250毫秒")
    parser.add_argument("--snapshot-refresh-wait-ms", type=int, default=5000,
                        help="等待同批快照刷新的上限，默认5000毫秒")
    parser.add_argument("--maximum-resource-queries", type=int, default=128,
                        help="单次快照资源状态查询上限，默认128项")
    parser.add_argument("--status-query-workers", type=int, default=8,
                        help="资源状态IPC并发上限，默认8路")
    parser.add_argument("--http-workers", type=int, default=32,
                        help="活动HTTP请求线程上限，默认32个")
    parser.add_argument("--http-request-timeout-ms", type=int, default=5000,
                        help="单连接HTTP I/O等待上限，默认5000毫秒")
    parser.add_argument("--event-capacity", type=int,
                        default=DEFAULT_EVENT_CAPACITY,
                        help="进程内增量事件保留条数，默认1024条")
    parser.add_argument("--trend-capacity", type=int,
                        default=60,
                        help="每个趋势序列的样本上限，默认60条")
    parser.add_argument("--trend-store-dir", type=Path,
                        help="显式启用趋势重启恢复的专用目录")
    parser.add_argument("--alert-rule-store-dir", type=Path,
                        help="显式启用受控告警规则更新及重启恢复的专用目录")
    parser.add_argument("--trend-store-maximum-bytes", type=int,
                        default=DEFAULT_TREND_STORE_MAXIMUM_BYTES,
                        help="趋势持久文件字节上限，默认1048576")
    parser.add_argument("--control-lease-capacity", type=int,
                        default=DEFAULT_CONTROL_LEASE_CAPACITY,
                        help="进程内活动控制租约上限，默认256条")
    parser.add_argument(
        "--clock-error-warning-ns", type=int, default=250_000,
        help="主机时钟模型估计误差上界告警阈值，默认250000纳秒")
    parser.add_argument(
        "--clock-sample-age-warning-ms", type=int, default=1_000,
        help="主机时钟模型样本年龄告警阈值，默认1000毫秒")
    parser.add_argument(
        "--toolbusd-legacy-text", action="store_true",
        help="显式兼容旧版remote-cli文本输出；不会自动回退")
    args = parser.parse_args()
    if args.port < 0 or args.port > 65535:
        parser.error("--port必须位于0～65535")
    if args.ipc_timeout_ms < 100 or args.ipc_timeout_ms > 10000:
        parser.error("--ipc-timeout-ms必须位于100～10000")
    if args.snapshot_cache_ms < 0 or args.snapshot_cache_ms > 60000:
        parser.error("--snapshot-cache-ms必须位于0～60000")
    if args.snapshot_refresh_wait_ms < 1 or \
            args.snapshot_refresh_wait_ms > 60000:
        parser.error("--snapshot-refresh-wait-ms必须位于1～60000")
    if args.maximum_resource_queries < 1 or \
            args.maximum_resource_queries > 4096:
        parser.error("--maximum-resource-queries必须位于1～4096")
    if args.status_query_workers < 1 or args.status_query_workers > 32:
        parser.error("--status-query-workers必须位于1～32")
    if args.http_workers < 1 or args.http_workers > 256:
        parser.error("--http-workers必须位于1～256")
    if args.http_request_timeout_ms < 100 or \
            args.http_request_timeout_ms > 30000:
        parser.error("--http-request-timeout-ms必须位于100～30000")
    if args.event_capacity < 1 or args.event_capacity > MAXIMUM_EVENT_CAPACITY:
        parser.error(f"--event-capacity必须位于1～{MAXIMUM_EVENT_CAPACITY}")
    if args.trend_capacity < 1 or args.trend_capacity > 600:
        parser.error("--trend-capacity必须位于1～600")
    if args.trend_store_maximum_bytes < 4096 or \
            args.trend_store_maximum_bytes > 16 * 1024 * 1024:
        parser.error("--trend-store-maximum-bytes必须位于4096～16777216")
    if args.trend_store_dir is None and \
            args.trend_store_maximum_bytes != DEFAULT_TREND_STORE_MAXIMUM_BYTES:
        parser.error("--trend-store-maximum-bytes必须与--trend-store-dir一起使用")
    if args.control_lease_capacity < 1 or \
            args.control_lease_capacity > MAXIMUM_CONTROL_LEASE_CAPACITY:
        parser.error(
            f"--control-lease-capacity必须位于1～"
            f"{MAXIMUM_CONTROL_LEASE_CAPACITY}")
    if args.clock_error_warning_ns < 1 or \
            args.clock_error_warning_ns > 1_000_000_000:
        parser.error("--clock-error-warning-ns必须位于1～1000000000")
    if args.clock_sample_age_warning_ms < 1 or \
            args.clock_sample_age_warning_ms > 60_000:
        parser.error("--clock-sample-age-warning-ms必须位于1～60000")
    if args.remote_cli and not args.toolbusd_socket:
        parser.error("--remote-cli必须与--toolbusd-socket一起使用")
    if args.toolbusd_legacy_text and not args.toolbusd_socket:
        parser.error("--toolbusd-legacy-text必须与--toolbusd-socket一起使用")
    if (args.control_audit_dir is None) != \
            (args.control_audit_key_file is None):
        parser.error(
            "--control-audit-dir与--control-audit-key-file必须成对配置")
    if not args.toolbusd_legacy_text and \
            args.maximum_resource_queries > 128:
        parser.error("结构化Runtime快照最多允许128项资源")
    authenticator = None
    if args.api_key_file:
        try:
            authenticator = load_api_key_authenticator(args.api_key_file)
        except AuthConfigurationError as error:
            parser.error(f"--api-key-file无效：{error}")
    try:
        loopback = ipaddress.ip_address(args.host).is_loopback
    except ValueError:
        parser.error("--host必须是数字IP地址；不能使用主机名")
    if not loopback and authenticator is None:
        parser.error("非回环监听必须配置--api-key-file")
    control_lease_manager = None
    if args.toolbusd_socket:
        ipc_client = RemoteCliIpcClient(
            args.toolbusd_socket, args.remote_cli or "remote-cli",
            timeout_seconds=args.ipc_timeout_ms / 1000.0,
            structured_output=not args.toolbusd_legacy_text)
        provider: RuntimeProvider = ToolbusdSnapshotProvider(
            ipc_client,
            maximum_resources_per_snapshot=args.maximum_resource_queries,
            cache_ttl_ms=args.snapshot_cache_ms,
            maximum_concurrent_status_queries=args.status_query_workers,
            refresh_wait_timeout_ms=args.snapshot_refresh_wait_ms,
            maximum_clock_error_bound_ns=args.clock_error_warning_ns,
            maximum_clock_sample_age_ms=
                args.clock_sample_age_warning_ms)
        control_lease_manager = DaemonBoundControlLeaseManager(
            ipc_client.daemon_identity,
            capacity=args.control_lease_capacity)
    elif args.snapshot:
        provider = FileSnapshotProvider(args.snapshot)
    else:
        provider = MockSnapshotProvider()
    control_audit_journal = None
    if args.control_audit_dir is not None:
        try:
            control_audit_journal = ControlAuditJournal(
                args.control_audit_dir, args.control_audit_key_file)
        except ControlAuditError as error:
            parser.error(f"控制审计配置无效：{error}")
    try:
        alert_rules = AlertRuleManager(AlertRuleStore(
            args.alert_rule_store_dir)) if args.alert_rule_store_dir else \
            AlertRuleManager()
        trend_store = RuntimeTrendStore(
            args.trend_store_dir, capacity=args.trend_capacity,
            maximum_bytes=args.trend_store_maximum_bytes
        ) if args.trend_store_dir is not None else None
        runtime_dashboard = RuntimeDashboard(
            args.trend_capacity, trend_store, alert_rules)
    except (TrendStoreError, AlertRuleError, ValueError) as error:
        parser.error(f"Runtime持久配置无效：{error}")
    try:
        server = make_server(
            args.host, args.port, provider,
            maximum_workers=args.http_workers,
            authenticator=authenticator,
            event_capacity=args.event_capacity,
            control_lease_manager=control_lease_manager,
            control_audit_journal=control_audit_journal,
            control_lease_capacity=(
                DEFAULT_CONTROL_LEASE_CAPACITY
                if control_lease_manager is not None
                else args.control_lease_capacity),
            request_io_timeout_seconds=
                args.http_request_timeout_ms / 1000.0,
            runtime_dashboard=runtime_dashboard,
            alert_rules=alert_rules)
    except BaseException:
        if control_audit_journal is not None:
            control_audit_journal.close()
        raise
    display_host = f"[{args.host}]" if ":" in args.host else args.host
    auth_mode = "API密钥认证" if authenticator is not None else "回环开发模式"
    print(f"RemoteBSP Runtime API已启动：http://{display_host}:"
          f"{server.server_port}/api/{API_VERSION}（{auth_mode}）")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

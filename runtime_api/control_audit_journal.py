"""Runtime 控制面的持久、完整性可验证审计日志。"""

from __future__ import annotations

import dataclasses
import errno
import hashlib
import hmac
import json
import os
import re
import stat
import threading
import time
from pathlib import Path
from typing import Callable

try:  # pragma: no cover - 正式运行环境为 Linux，保留可诊断的导入行为。
    import fcntl
except ImportError:  # pragma: no cover
    fcntl = None  # type: ignore[assignment]


CONTROL_AUDIT_SCHEMA_VERSION = 1
DEFAULT_MAXIMUM_RECORDS = 65_536
DEFAULT_MAXIMUM_TOTAL_BYTES = 64 * 1024 * 1024
DEFAULT_MAXIMUM_SEGMENT_BYTES = 4 * 1024 * 1024
MINIMUM_KEY_BYTES = 32
MAXIMUM_KEY_BYTES = 64

_DOMAIN = b"RemoteBSP/control-audit/v1\0"
_COMMIT_MARKER = bytes.fromhex("5242434155444954")  # RBCAUDIT
_ZERO_MAC = b"\0" * hashlib.sha256().digest_size
_MANIFEST_NAME = "manifest.json"
_LOCK_NAME = "journal.lock"
_SEGMENT_RE = re.compile(r"segment-([0-9]{16})\.rcaj")
_HEX_32_RE = re.compile(r"[0-9a-f]{32}")
_HEX_64_RE = re.compile(r"[0-9a-f]{64}")
_KEY_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
_ACTIONS = frozenset({"acquire", "gpio_write", "pwm_configure", "pwm_stop", "release"})
_TERMINAL_RESULTS = frozenset({"committed", "rejected", "released", "failed"})
_UNKNOWN_REASONS = frozenset({
    "audit_sync_failed",
    "deadline_exceeded",
    "downstream_uncertain",
    "process_recovery",
    "provider_unavailable",
})
_FRAME_FIXED_BYTES = 4 + hashlib.sha256().digest_size + len(_COMMIT_MARKER)
_MAXIMUM_PAYLOAD_BYTES = 2048
_MAXIMUM_DIGEST_INPUT_BYTES = 4096
_DIGEST_DOMAIN = b"RemoteBSP/control-audit/request-digest/v1\0"


class ControlAuditError(RuntimeError):
    """控制审计日志错误基类。"""


class ControlAuditConfigError(ControlAuditError):
    """目录、密钥或选项不满足安全合同。"""


class ControlAuditCorruptionError(ControlAuditError):
    """持久数据的完整性、序列或状态转换无效。"""


class ControlAuditCapacityError(ControlAuditError):
    """日志达到配置的硬容量上限。"""


class ControlAuditIoError(ControlAuditError):
    """同步持久化失败；调用方必须失败关闭控制写入口。"""


@dataclasses.dataclass(frozen=True)
class ControlAuditOptions:
    maximum_records: int = DEFAULT_MAXIMUM_RECORDS
    maximum_total_bytes: int = DEFAULT_MAXIMUM_TOTAL_BYTES
    maximum_segment_bytes: int = DEFAULT_MAXIMUM_SEGMENT_BYTES

    def validate(self) -> None:
        if not 1 <= self.maximum_records <= DEFAULT_MAXIMUM_RECORDS:
            raise ControlAuditConfigError("控制审计记录上限必须位于1～65536")
        minimum_segment = _FRAME_FIXED_BYTES + 256
        if not minimum_segment <= self.maximum_segment_bytes <= \
                DEFAULT_MAXIMUM_SEGMENT_BYTES:
            raise ControlAuditConfigError("控制审计分段上限越界")
        if not self.maximum_segment_bytes <= self.maximum_total_bytes <= \
                DEFAULT_MAXIMUM_TOTAL_BYTES:
            raise ControlAuditConfigError("控制审计总字节上限越界")


@dataclasses.dataclass(frozen=True)
class ControlAuditRecord:
    schema_version: int
    sequence: int
    occurred_at_ms: int
    state: str
    intent_sequence: int | None
    request_id: str | None
    key_id: str | None
    action: str | None
    lease_id: str | None
    node_id: int | None
    resource_id: int | None
    request_digest: str | None
    result: str | None
    reason: str | None
    operation_id: str | None

    def to_dict(self) -> dict[str, object]:
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class ControlAuditHealth:
    operational: bool
    records: int
    segments: int
    bytes: int
    last_sequence: int
    dangling_intents: int


class ControlAuditJournal:
    """同步、追加式 HMAC 链审计日志。

    构造成功表示现有历史已经完整重放并取得独占进程锁。append 方法只有在
    记录、签名 manifest 和目录元数据全部同步后才返回。
    """

    def __init__(self, directory: str | os.PathLike[str],
                 key_file: str | os.PathLike[str], *,
                 options: ControlAuditOptions | None = None,
                 io_hook: Callable[[str], None] | None = None,
                 clock_ms: Callable[[], int] | None = None):
        self._options = options or ControlAuditOptions()
        self._options.validate()
        self._io_hook = io_hook
        self._clock_ms = clock_ms or (lambda: time.time_ns() // 1_000_000)
        self._mutex = threading.RLock()
        self._closed = False
        self._operational = False
        self._directory = Path(directory)
        self._dir_fd = -1
        self._lock_fd = -1
        self._key = b""
        self._records: list[ControlAuditRecord] = []
        self._intents: dict[int, ControlAuditRecord] = {}
        self._finalized: set[int] = set()
        self._active_segment = 1
        self._active_size = 0
        self._total_bytes = 0
        self._last_sequence = 0
        self._last_mac = _ZERO_MAC
        try:
            self._open_directory()
            self._key = self._load_key(Path(key_file))
            self._acquire_lock()
            self._load_or_initialize()
            self._operational = True
            self._recover_dangling_intents()
        except Exception:
            self._close_fds()
            raise

    def __enter__(self) -> "ControlAuditJournal":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def close(self) -> None:
        """幂等释放进程锁；关闭后不得继续追加。"""
        with self._mutex:
            if self._closed:
                return
            self._operational = False
            self._closed = True
            self._close_fds()
            self._key = b""

    @property
    def operational(self) -> bool:
        with self._mutex:
            return self._operational and not self._closed

    def health_snapshot(self) -> ControlAuditHealth:
        with self._mutex:
            return ControlAuditHealth(
                operational=self._operational and not self._closed,
                records=len(self._records),
                segments=self._active_segment,
                bytes=self._total_bytes,
                last_sequence=self._last_sequence,
                dangling_intents=len(set(self._intents) - self._finalized))

    def snapshot(self) -> tuple[ControlAuditRecord, ...]:
        with self._mutex:
            return tuple(self._records)

    def request_digest(self, value: bytes) -> str:
        """为受控规范请求生成不可离线枚举的域分隔摘要。

        value 应由调用方对允许进入审计身份的字段做规范编码；日志既不接收也不
        持久化该原文。使用独立域的 HMAC 避免低熵 GPIO 值和幂等标识被字典关联。
        """
        if type(value) is not bytes or not 1 <= len(value) <= \
                _MAXIMUM_DIGEST_INPUT_BYTES:
            raise ValueError("控制审计摘要输入必须是1～4096字节的bytes")
        with self._mutex:
            self._ensure_operational()
            return hmac.new(
                self._key, _DIGEST_DOMAIN + value,
                hashlib.sha256).hexdigest()

    def append_intent(self, *, request_id: str, key_id: str,
                      action: str, request_digest: str,
                      lease_id: str | None = None,
                      node_id: int | None = None,
                      resource_id: int | None = None) -> ControlAuditRecord:
        """在任何下游控制副作用之前同步记录脱敏意图。"""
        if _HEX_32_RE.fullmatch(request_id) is None:
            raise ValueError("request_id必须是32位小写十六进制")
        if _KEY_ID_RE.fullmatch(key_id) is None:
            raise ValueError("key_id格式无效")
        if action not in _ACTIONS:
            raise ValueError("控制审计action无效")
        if _HEX_64_RE.fullmatch(request_digest) is None:
            raise ValueError("request_digest必须是64位小写十六进制")
        if lease_id is not None and _HEX_32_RE.fullmatch(lease_id) is None:
            raise ValueError("lease_id必须是32位小写十六进制")
        self._validate_optional_u32("node_id", node_id)
        self._validate_optional_u32("resource_id", resource_id)
        with self._mutex:
            record = self._new_record(
                state="intent", intent_sequence=None, request_id=request_id,
                key_id=key_id, action=action, lease_id=lease_id,
                node_id=node_id, resource_id=resource_id,
                request_digest=request_digest, result=None, reason=None,
                operation_id=None)
            self._append(record)
            return record

    def append_terminal(self, intent_sequence: int, result: str, *,
                        operation_id: str | None = None) -> ControlAuditRecord:
        """同步记录确定终态；同一意图只允许一个终态或 unknown。"""
        if result not in _TERMINAL_RESULTS:
            raise ValueError("控制审计terminal result无效")
        self._validate_operation_id(operation_id)
        with self._mutex:
            self._validate_completion(intent_sequence)
            record = self._new_record(
                state="terminal", intent_sequence=intent_sequence,
                request_id=None, key_id=None, action=None, lease_id=None,
                node_id=None, resource_id=None, request_digest=None,
                result=result, reason=None, operation_id=operation_id)
            self._append(record)
            return record

    def append_unknown(self, intent_sequence: int, reason: str, *,
                       operation_id: str | None = None) -> ControlAuditRecord:
        """同步记录可能已提交的未知结果，禁止调用方将其盲目重试。"""
        if reason not in _UNKNOWN_REASONS:
            raise ValueError("控制审计unknown reason无效")
        self._validate_operation_id(operation_id)
        with self._mutex:
            self._validate_completion(intent_sequence)
            record = self._new_record(
                state="unknown", intent_sequence=intent_sequence,
                request_id=None, key_id=None, action=None, lease_id=None,
                node_id=None, resource_id=None, request_digest=None,
                result=None, reason=reason, operation_id=operation_id)
            self._append(record)
            return record

    def _recover_dangling_intents(self) -> None:
        """开放控制入口前，把上次进程遗留的 durable intent 固化为 unknown。"""
        dangling = sorted(set(self._intents) - self._finalized)
        for intent_sequence in dangling:
            self.append_unknown(intent_sequence, "process_recovery")

    @staticmethod
    def _validate_optional_u32(name: str, value: int | None) -> None:
        if value is not None and (type(value) is not int or
                                  not 0 <= value <= 0xffff_ffff):
            raise ValueError(f"{name}必须是u32或null")

    @staticmethod
    def _validate_operation_id(value: str | None) -> None:
        if value is not None and _HEX_64_RE.fullmatch(value) is None:
            raise ValueError("operation_id必须是64位小写十六进制或null")

    def _new_record(self, **fields: object) -> ControlAuditRecord:
        occurred = self._clock_ms()
        if type(occurred) is not int or occurred < 0:
            raise ControlAuditConfigError("审计时钟必须返回非负整数毫秒")
        return ControlAuditRecord(
            schema_version=CONTROL_AUDIT_SCHEMA_VERSION,
            sequence=self._last_sequence + 1,
            occurred_at_ms=occurred,
            **fields)  # type: ignore[arg-type]

    def _validate_completion(self, intent_sequence: int) -> None:
        if type(intent_sequence) is not int or intent_sequence < 1:
            raise ValueError("intent_sequence必须是正整数")
        if intent_sequence not in self._intents:
            raise ValueError("引用的控制审计intent不存在")
        if intent_sequence in self._finalized:
            raise ValueError("控制审计intent已经具有最终结果")

    def _open_directory(self) -> None:
        if fcntl is None:
            raise ControlAuditConfigError("控制审计日志仅支持具备flock的Linux环境")
        try:
            os.mkdir(self._directory, 0o700)
        except FileExistsError:
            pass
        except OSError as exc:
            raise ControlAuditConfigError("创建控制审计目录失败") from exc
        try:
            status = os.lstat(self._directory)
            if not stat.S_ISDIR(status.st_mode) or \
                    status.st_uid != os.geteuid() or status.st_mode & 0o077:
                raise ControlAuditConfigError(
                    "控制审计目录必须是真实目录、归当前用户所有且禁止组/其他访问")
            flags = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC
            if hasattr(os, "O_NOFOLLOW"):
                flags |= os.O_NOFOLLOW
            self._dir_fd = os.open(self._directory, flags)
            opened = os.fstat(self._dir_fd)
            if (opened.st_dev, opened.st_ino) != (status.st_dev, status.st_ino) or \
                    not stat.S_ISDIR(opened.st_mode) or \
                    opened.st_uid != os.geteuid() or opened.st_mode & 0o077:
                raise ControlAuditConfigError("控制审计目录在打开期间被替换")
        except ControlAuditError:
            raise
        except OSError as exc:
            raise ControlAuditConfigError("安全打开控制审计目录失败") from exc

    def _load_key(self, path: Path) -> bytes:
        try:
            before = os.lstat(path)
            if not stat.S_ISREG(before.st_mode) or before.st_uid != os.geteuid() \
                    or before.st_mode & 0o077 or before.st_nlink != 1:
                raise ControlAuditConfigError(
                    "控制审计密钥必须是当前用户独占的0600普通单链接文件")
            flags = os.O_RDONLY | os.O_CLOEXEC
            if hasattr(os, "O_NOFOLLOW"):
                flags |= os.O_NOFOLLOW
            fd = os.open(path, flags)
            try:
                after = os.fstat(fd)
                if (after.st_dev, after.st_ino) != (before.st_dev, before.st_ino):
                    raise ControlAuditConfigError("控制审计密钥在打开期间被替换")
                key = self._read_all(fd, MAXIMUM_KEY_BYTES + 1)
            finally:
                os.close(fd)
        except ControlAuditError:
            raise
        except OSError as exc:
            raise ControlAuditConfigError("读取控制审计密钥失败") from exc
        if not MINIMUM_KEY_BYTES <= len(key) <= MAXIMUM_KEY_BYTES:
            raise ControlAuditConfigError("控制审计密钥长度必须位于32～64字节")
        return key

    def _acquire_lock(self) -> None:
        flags = os.O_RDWR | os.O_CREAT | os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        try:
            self._lock_fd = os.open(
                _LOCK_NAME, flags, 0o600, dir_fd=self._dir_fd)
            self._validate_open_file(self._lock_fd, _LOCK_NAME)
            fcntl.flock(self._lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise ControlAuditConfigError(
                "控制审计目录已被另一个Runtime实例锁定") from exc
        except ControlAuditError:
            raise
        except OSError as exc:
            raise ControlAuditConfigError("取得控制审计进程锁失败") from exc

    def _load_or_initialize(self) -> None:
        names = set(os.listdir(self._dir_fd))
        if _MANIFEST_NAME not in names:
            remnants = names - {_LOCK_NAME}
            if remnants:
                raise ControlAuditCorruptionError(
                    "manifest缺失但存在控制审计遗留文件")
            self._create_segment(1)
            self._write_manifest()
            return
        self._load_manifest()
        self._scan_segments(names)

    def _manifest_body(self) -> dict[str, object]:
        return {
            "active_segment": self._active_segment,
            "active_size": self._active_size,
            "format": CONTROL_AUDIT_SCHEMA_VERSION,
            "last_mac": self._last_mac.hex(),
            "last_sequence": self._last_sequence,
            # append 在更新内存索引前先提交 manifest，因此这里以已经准备提交的
            # 单调序号为准；启动扫描仍会核对实际记录数与该锚点一致。
            "record_count": self._last_sequence,
            "total_bytes": self._total_bytes,
        }

    def _write_manifest(self) -> None:
        body = self._manifest_body()
        signature = hmac.new(
            self._key, b"manifest\0" + self._canonical(body),
            hashlib.sha256).hexdigest()
        document = dict(body)
        document["hmac"] = signature
        data = self._canonical(document) + b"\n"
        temporary = f"manifest.tmp-{os.getpid()}-{threading.get_ident()}"
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        fd = -1
        try:
            self._hook("manifest_write")
            fd = os.open(temporary, flags, 0o600, dir_fd=self._dir_fd)
            self._write_all(fd, data)
            self._hook("manifest_sync")
            os.fsync(fd)
            os.close(fd)
            fd = -1
            os.replace(temporary, _MANIFEST_NAME,
                       src_dir_fd=self._dir_fd, dst_dir_fd=self._dir_fd)
            self._hook("directory_sync")
            os.fsync(self._dir_fd)
        except OSError as exc:
            if fd >= 0:
                os.close(fd)
            try:
                os.unlink(temporary, dir_fd=self._dir_fd)
            except OSError:
                pass
            raise ControlAuditIoError(
                f"同步控制审计manifest失败: {exc.strerror or exc}") from exc

    def _load_manifest(self) -> None:
        try:
            data = self._read_secure_named_file(_MANIFEST_NAME, 4096)
            document = json.loads(data)
        except ControlAuditError:
            raise
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise ControlAuditCorruptionError("控制审计manifest无法解析") from exc
        expected_keys = {
            "active_segment", "active_size", "format", "hmac", "last_mac",
            "last_sequence", "record_count", "total_bytes"}
        if type(document) is not dict or set(document) != expected_keys:
            raise ControlAuditCorruptionError("控制审计manifest字段集合无效")
        signature = document.pop("hmac")
        if not isinstance(signature, str) or _HEX_64_RE.fullmatch(signature) is None:
            raise ControlAuditCorruptionError("控制审计manifest签名格式无效")
        actual = hmac.new(
            self._key, b"manifest\0" + self._canonical(document),
            hashlib.sha256).hexdigest()
        if not hmac.compare_digest(signature, actual):
            raise ControlAuditCorruptionError("控制审计manifest签名不匹配或密钥错误")
        integers = ("active_segment", "active_size", "last_sequence",
                    "record_count", "total_bytes")
        if document["format"] != CONTROL_AUDIT_SCHEMA_VERSION or any(
                type(document[name]) is not int or document[name] < 0
                for name in integers):
            raise ControlAuditCorruptionError("控制审计manifest数值无效")
        if not 1 <= document["active_segment"] <= self._options.maximum_records:
            raise ControlAuditCorruptionError("控制审计manifest分段号无效")
        if not 0 <= document["active_size"] <= self._options.maximum_segment_bytes \
                or not 0 <= document["total_bytes"] <= self._options.maximum_total_bytes \
                or not 0 <= document["record_count"] <= self._options.maximum_records \
                or document["last_sequence"] != document["record_count"] \
                or _HEX_64_RE.fullmatch(document["last_mac"]) is None:
            raise ControlAuditCorruptionError("控制审计manifest边界或锚点无效")
        self._active_segment = document["active_segment"]
        self._active_size = document["active_size"]
        self._total_bytes = document["total_bytes"]
        self._last_sequence = document["last_sequence"]
        self._last_mac = bytes.fromhex(document["last_mac"])

    def _scan_segments(self, names: set[str]) -> None:
        temporary_names = {
            name for name in names
            if re.fullmatch(r"manifest\.tmp-[0-9]+-[0-9]+", name)}
        for name in sorted(temporary_names):
            self._remove_uncommitted_file(name)
        names -= temporary_names
        segment_numbers: list[int] = []
        for name in names - {_LOCK_NAME, _MANIFEST_NAME}:
            match = _SEGMENT_RE.fullmatch(name)
            if match is None:
                raise ControlAuditCorruptionError(
                    f"控制审计目录包含未知文件: {name}")
            segment_numbers.append(int(match.group(1)))
        expected = list(range(1, self._active_segment + 1))
        extras = sorted(set(segment_numbers) - set(expected))
        if extras:
            if extras == [self._active_segment + 1]:
                self._remove_uncommitted_segment(extras[0])
                segment_numbers.remove(extras[0])
            else:
                raise ControlAuditCorruptionError("控制审计分段集合不连续")
        if sorted(segment_numbers) != expected:
            raise ControlAuditCorruptionError("控制审计分段缺失或重复")
        previous_mac = _ZERO_MAC
        expected_sequence = 1
        observed_total = 0
        for number in expected:
            name = self._segment_name(number)
            fd = self._open_existing_segment(name, os.O_RDWR)
            try:
                status = os.fstat(fd)
                size = status.st_size
                logical = self._active_size if number == self._active_segment else size
                if size < logical or size > self._options.maximum_segment_bytes or \
                        logical > self._options.maximum_segment_bytes:
                    raise ControlAuditCorruptionError("控制审计分段长度与manifest不一致")
                if number == self._active_segment and size > logical:
                    os.ftruncate(fd, logical)
                    os.fdatasync(fd)
                    os.fsync(self._dir_fd)
                data = self._read_all(fd, logical + 1)
                if len(data) != logical:
                    raise ControlAuditCorruptionError("控制审计分段出现短读")
            finally:
                os.close(fd)
            offset = 0
            while offset < len(data):
                record, record_mac, frame_size = self._decode_frame(
                    data, offset, previous_mac, expected_sequence)
                self._apply_record(record)
                previous_mac = record_mac
                expected_sequence += 1
                offset += frame_size
            observed_total += logical
        if observed_total != self._total_bytes or \
                expected_sequence - 1 != self._last_sequence or \
                len(self._records) != self._last_sequence or \
                not hmac.compare_digest(previous_mac, self._last_mac):
            raise ControlAuditCorruptionError("控制审计历史与manifest最终锚点不一致")

    def _append(self, record: ControlAuditRecord) -> None:
        self._ensure_operational()
        payload = self._canonical(record.to_dict())
        if len(payload) > _MAXIMUM_PAYLOAD_BYTES:
            raise ValueError("控制审计记录超过固定载荷上限")
        record_mac = hmac.new(
            self._key, _DOMAIN + self._last_mac + payload,
            hashlib.sha256).digest()
        frame = (len(payload).to_bytes(4, "little") + payload + record_mac +
                 _COMMIT_MARKER)
        if len(frame) > self._options.maximum_segment_bytes or \
                len(self._records) >= self._options.maximum_records or \
                self._total_bytes + len(frame) > self._options.maximum_total_bytes:
            raise ControlAuditCapacityError("控制审计日志容量已满")
        next_segment = self._active_segment
        next_size = self._active_size
        if next_size and next_size + len(frame) > self._options.maximum_segment_bytes:
            next_segment += 1
            self._create_segment(next_segment)
            next_size = 0
        name = self._segment_name(next_segment)
        fd = self._open_existing_segment(name, os.O_WRONLY | os.O_APPEND)
        try:
            self._hook("record_write")
            self._write_all(fd, frame)
            self._hook("record_sync")
            os.fdatasync(fd)
        except OSError as exc:
            self._operational = False
            raise ControlAuditIoError(
                f"同步控制审计记录失败: {exc.strerror or exc}") from exc
        finally:
            os.close(fd)
        old = (self._active_segment, self._active_size, self._total_bytes,
               self._last_sequence, self._last_mac)
        self._active_segment = next_segment
        self._active_size = next_size + len(frame)
        self._total_bytes += len(frame)
        self._last_sequence = record.sequence
        self._last_mac = record_mac
        try:
            self._write_manifest()
        except ControlAuditError:
            (self._active_segment, self._active_size, self._total_bytes,
             self._last_sequence, self._last_mac) = old
            self._operational = False
            raise
        self._apply_record(record)

    def _decode_frame(self, data: bytes, offset: int, previous_mac: bytes,
                      expected_sequence: int) -> tuple[ControlAuditRecord, bytes, int]:
        if len(data) - offset < _FRAME_FIXED_BYTES:
            raise ControlAuditCorruptionError("已提交的控制审计记录头截断")
        payload_size = int.from_bytes(data[offset:offset + 4], "little")
        frame_size = _FRAME_FIXED_BYTES + payload_size
        if payload_size < 2 or payload_size > _MAXIMUM_PAYLOAD_BYTES or \
                frame_size > len(data) - offset:
            raise ControlAuditCorruptionError("已提交的控制审计记录长度无效")
        payload_start = offset + 4
        payload_end = payload_start + payload_size
        record_mac = data[payload_end:payload_end + 32]
        marker = data[payload_end + 32:payload_end + 40]
        expected_mac = hmac.new(
            self._key, _DOMAIN + previous_mac + data[payload_start:payload_end],
            hashlib.sha256).digest()
        if marker != _COMMIT_MARKER or not hmac.compare_digest(
                record_mac, expected_mac):
            raise ControlAuditCorruptionError("控制审计记录HMAC或提交标记无效")
        try:
            raw = json.loads(data[payload_start:payload_end])
            record = self._record_from_dict(raw)
        except (UnicodeError, json.JSONDecodeError, TypeError, ValueError) as exc:
            raise ControlAuditCorruptionError("控制审计记录载荷无效") from exc
        if record.sequence != expected_sequence:
            raise ControlAuditCorruptionError("控制审计记录序号不连续")
        return record, record_mac, frame_size

    def _record_from_dict(self, raw: object) -> ControlAuditRecord:
        field_names = {field.name for field in dataclasses.fields(ControlAuditRecord)}
        if type(raw) is not dict or set(raw) != field_names:
            raise ValueError("字段集合无效")
        record = ControlAuditRecord(**raw)  # type: ignore[arg-type]
        if record.schema_version != CONTROL_AUDIT_SCHEMA_VERSION or \
                type(record.sequence) is not int or record.sequence < 1 or \
                type(record.occurred_at_ms) is not int or record.occurred_at_ms < 0:
            raise ValueError("版本、序号或时间无效")
        if record.state == "intent":
            if record.intent_sequence is not None or record.result is not None or \
                    record.reason is not None or record.operation_id is not None:
                raise ValueError("intent终态字段非空")
            if not isinstance(record.request_id, str) or \
                    _HEX_32_RE.fullmatch(record.request_id) is None or \
                    not isinstance(record.key_id, str) or \
                    _KEY_ID_RE.fullmatch(record.key_id) is None or \
                    record.action not in _ACTIONS or \
                    not isinstance(record.request_digest, str) or \
                    _HEX_64_RE.fullmatch(record.request_digest) is None:
                raise ValueError("intent身份字段无效")
            if record.lease_id is not None and \
                    (not isinstance(record.lease_id, str) or
                     _HEX_32_RE.fullmatch(record.lease_id) is None):
                raise ValueError("intent lease_id无效")
            self._validate_optional_u32("node_id", record.node_id)
            self._validate_optional_u32("resource_id", record.resource_id)
        elif record.state in {"terminal", "unknown"}:
            if any(value is not None for value in (
                    record.request_id, record.key_id, record.action,
                    record.lease_id, record.node_id, record.resource_id,
                    record.request_digest)):
                raise ValueError("完成记录包含原始意图字段")
            if type(record.intent_sequence) is not int or record.intent_sequence < 1:
                raise ValueError("完成记录引用序号无效")
            self._validate_operation_id(record.operation_id)
            if record.state == "terminal":
                if record.result not in _TERMINAL_RESULTS or record.reason is not None:
                    raise ValueError("terminal字段组合无效")
            elif record.reason not in _UNKNOWN_REASONS or record.result is not None:
                raise ValueError("unknown字段组合无效")
        else:
            raise ValueError("状态无效")
        return record

    def _apply_record(self, record: ControlAuditRecord) -> None:
        if record.state == "intent":
            self._intents[record.sequence] = record
        else:
            assert record.intent_sequence is not None
            if record.intent_sequence not in self._intents or \
                    record.intent_sequence in self._finalized:
                raise ControlAuditCorruptionError("控制审计状态转换矛盾")
            self._finalized.add(record.intent_sequence)
        self._records.append(record)

    def _create_segment(self, number: int) -> None:
        name = self._segment_name(number)
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        fd = -1
        try:
            fd = os.open(name, flags, 0o600, dir_fd=self._dir_fd)
            os.fsync(fd)
            os.close(fd)
            fd = -1
            os.fsync(self._dir_fd)
        except OSError as exc:
            if fd >= 0:
                os.close(fd)
            raise ControlAuditIoError("创建控制审计分段失败") from exc

    def _remove_uncommitted_segment(self, number: int) -> None:
        name = self._segment_name(number)
        self._remove_uncommitted_file(name)

    def _remove_uncommitted_file(self, name: str) -> None:
        fd = self._open_existing_segment(name, os.O_RDONLY)
        os.close(fd)
        try:
            os.unlink(name, dir_fd=self._dir_fd)
            os.fsync(self._dir_fd)
        except OSError as exc:
            raise ControlAuditIoError("清理未提交控制审计分段失败") from exc

    def _open_existing_segment(self, name: str, flags: int) -> int:
        open_flags = flags | os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            open_flags |= os.O_NOFOLLOW
        try:
            fd = os.open(name, open_flags, dir_fd=self._dir_fd)
            try:
                self._validate_open_file(fd, name)
                return fd
            except Exception:
                os.close(fd)
                raise
        except ControlAuditError:
            raise
        except OSError as exc:
            raise ControlAuditCorruptionError(
                f"控制审计分段无法安全打开: {name}") from exc

    def _read_secure_named_file(self, name: str, maximum: int) -> str:
        flags = os.O_RDONLY | os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        fd = os.open(name, flags, dir_fd=self._dir_fd)
        try:
            self._validate_open_file(fd, name)
            raw = self._read_all(fd, maximum + 1)
        finally:
            os.close(fd)
        if len(raw) > maximum:
            raise ControlAuditCorruptionError(f"控制审计文件过大: {name}")
        return raw.decode("utf-8")

    @staticmethod
    def _validate_open_file(fd: int, name: str) -> None:
        status = os.fstat(fd)
        if not stat.S_ISREG(status.st_mode) or status.st_uid != os.geteuid() \
                or status.st_mode & 0o077 or status.st_nlink != 1:
            raise ControlAuditConfigError(
                f"控制审计文件属主、权限、类型或链接数不安全: {name}")

    def _ensure_operational(self) -> None:
        if self._closed:
            raise ControlAuditIoError("控制审计日志已经关闭")
        if not self._operational:
            raise ControlAuditIoError("控制审计日志不可用，控制写入口必须失败关闭")

    def _hook(self, point: str) -> None:
        if self._io_hook is not None:
            self._io_hook(point)

    @staticmethod
    def _segment_name(number: int) -> str:
        return f"segment-{number:016d}.rcaj"

    @staticmethod
    def _canonical(value: object) -> bytes:
        return json.dumps(
            value, ensure_ascii=True, sort_keys=True,
            separators=(",", ":"), allow_nan=False).encode("utf-8")

    @staticmethod
    def _write_all(fd: int, data: bytes) -> None:
        view = memoryview(data)
        while view:
            try:
                written = os.write(fd, view)
            except InterruptedError:
                continue
            if written <= 0:
                raise OSError(errno.EIO, "控制审计写入没有取得进展")
            view = view[written:]

    @staticmethod
    def _read_all(fd: int, maximum: int) -> bytes:
        chunks: list[bytes] = []
        remaining = maximum
        while remaining > 0:
            try:
                chunk = os.read(fd, min(remaining, 64 * 1024))
            except InterruptedError:
                continue
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _close_fds(self) -> None:
        if self._lock_fd >= 0:
            try:
                if fcntl is not None:
                    fcntl.flock(self._lock_fd, fcntl.LOCK_UN)
            finally:
                os.close(self._lock_fd)
                self._lock_fd = -1
        if self._dir_fd >= 0:
            os.close(self._dir_fd)
            self._dir_fd = -1

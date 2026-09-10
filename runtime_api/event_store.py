"""Runtime 增量事件历史的版本化、有界、原子本地存储。"""

from __future__ import annotations

import json
import os
import secrets
import stat
from pathlib import Path


EVENT_STORE_SCHEMA_VERSION = 1
EVENT_STORE_KIND = "remotebsp-runtime-event-history"
EVENT_STORE_FILENAME = "event-history-v1.json"
DEFAULT_EVENT_STORE_MAXIMUM_BYTES = 8 * 1024 * 1024
MAXIMUM_EVENT_STORE_MAXIMUM_BYTES = 32 * 1024 * 1024


class EventStoreError(RuntimeError):
    """事件历史目录或持久文件不满足安全约束。"""


class RuntimeEventStore:
    """固定单文件事件存储；损坏文件隔离后以空历史启动。"""

    def __init__(self, directory: Path, *, maximum_bytes: int =
                 DEFAULT_EVENT_STORE_MAXIMUM_BYTES):
        if type(maximum_bytes) is not int or not 4096 <= maximum_bytes <= \
                MAXIMUM_EVENT_STORE_MAXIMUM_BYTES:
            raise EventStoreError("事件历史文件上限必须位于4096～33554432字节")
        self.directory = Path(directory)
        self.path = self.directory / EVENT_STORE_FILENAME
        self.maximum_bytes = maximum_bytes
        self.load_status = "empty"
        self._prepare_directory()

    def _prepare_directory(self) -> None:
        try:
            if self.directory.exists() and self.directory.is_symlink():
                raise EventStoreError("事件历史目录不能是符号链接")
            self.directory.mkdir(mode=0o700, parents=False, exist_ok=True)
            if not self.directory.is_dir() or self.path.is_symlink():
                raise EventStoreError("事件历史存储路径不安全")
            if os.name == "posix":
                info = self.directory.stat()
                if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) & 0o077:
                    raise EventStoreError("事件历史目录必须由服务用户独占")
        except EventStoreError:
            raise
        except OSError as error:
            raise EventStoreError(f"无法准备事件历史目录：{error}") from error

    def load(self) -> dict | None:
        if not self.path.exists():
            self.load_status = "empty"
            return None
        try:
            if self.path.is_symlink():
                raise EventStoreError("事件历史文件不能是符号链接")
            info = self.path.stat()
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or \
                    (os.name == "posix" and
                     (info.st_uid != os.geteuid() or
                      stat.S_IMODE(info.st_mode) & 0o077)):
                raise EventStoreError("事件历史文件必须是服务用户独占的单链接普通文件")
            if info.st_size > self.maximum_bytes:
                raise ValueError("事件历史文件超过容量上限")
            raw = json.loads(self.path.read_bytes().decode("utf-8"))
            if not isinstance(raw, dict) or set(raw) != {
                    "schema_version", "kind", "next_sequence",
                    "baseline_snapshot", "events"} or \
                    raw["schema_version"] != EVENT_STORE_SCHEMA_VERSION or \
                    raw["kind"] != EVENT_STORE_KIND:
                raise ValueError("事件历史文件头不合法")
            self.load_status = "recovered"
            return raw
        except EventStoreError:
            raise
        except (OSError, UnicodeDecodeError, json.JSONDecodeError,
                ValueError) as error:
            self.quarantine()
            return None

    def quarantine(self) -> None:
        """以不覆盖硬链接保存诊断副本，然后移除活动文件。"""
        try:
            for _ in range(8):
                quarantine = self.directory / (
                    f"event-history-v1.corrupt-{secrets.token_hex(8)}.json")
                try:
                    os.link(self.path, quarantine)
                    break
                except FileExistsError:
                    continue
            else:
                raise OSError("无法分配隔离文件名")
            self.path.unlink()
        except OSError as error:
            raise EventStoreError(f"事件历史损坏且无法隔离：{error}") from error
        self.load_status = "corrupt_isolated"

    def save(self, document: dict) -> None:
        encoded = (json.dumps(document, ensure_ascii=False, sort_keys=True,
                              separators=(",", ":"), allow_nan=False) +
                   "\n").encode("utf-8")
        if len(encoded) > self.maximum_bytes:
            raise EventStoreError("事件历史文件超过容量上限")
        temporary = self.directory / (
            f".{EVENT_STORE_FILENAME}.{secrets.token_hex(8)}.tmp")
        try:
            descriptor = os.open(
                temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(encoded)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)
            if os.name == "posix":
                directory_fd = os.open(self.directory, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
        except (OSError, TypeError, ValueError) as error:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
            raise EventStoreError(f"无法原子保存事件历史：{error}") from error

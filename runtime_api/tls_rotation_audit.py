"""TLS轮换的版本化、有界持久审计状态。"""

from __future__ import annotations

import hashlib
import json
import os
import stat
import tempfile
import threading
import time
from pathlib import Path


class TlsRotationAuditError(RuntimeError):
    pass


class TlsRotationAuditJournal:
    SCHEMA_VERSION = 1

    def __init__(self, path: Path, capacity: int = 64):
        self.path = Path(path)
        self.capacity = capacity
        self._lock = threading.Lock()
        self._records: list[dict] = []
        if self.path.exists():
            self._load()

    @staticmethod
    def _digest(payload: dict) -> str:
        raw = json.dumps(payload, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode()
        return hashlib.sha256(raw).hexdigest()

    def _load(self) -> None:
        info = self.path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or \
                stat.S_IMODE(info.st_mode) != 0o600:
            raise TlsRotationAuditError("TLS轮换审计文件类型或权限无效")
        try:
            document = json.loads(self.path.read_text(encoding="utf-8"))
            checksum = document.pop("checksum")
        except (OSError, KeyError, json.JSONDecodeError) as error:
            raise TlsRotationAuditError("TLS轮换审计文件损坏") from error
        if set(document) != {"schema_version", "records"} or \
                document["schema_version"] != self.SCHEMA_VERSION or \
                not isinstance(document["records"], list) or \
                checksum != self._digest(document):
            raise TlsRotationAuditError("TLS轮换审计文件完整性校验失败")
        self._records = document["records"][-self.capacity:]

    def append(self, *, generation: int, result: str,
               old_fingerprint: str | None,
               new_fingerprint: str | None) -> None:
        record = {
            "timestamp_ms": time.time_ns() // 1_000_000,
            "generation": generation,
            "result": result,
            "old_certificate_sha256": old_fingerprint,
            "new_certificate_sha256": new_fingerprint,
        }
        with self._lock:
            new_records = (self._records + [record])[-self.capacity:]
            payload = {"schema_version": self.SCHEMA_VERSION,
                       "records": new_records}
            document = {**payload, "checksum": self._digest(payload)}
            self.path.parent.mkdir(parents=True, exist_ok=True)
            fd, temporary = tempfile.mkstemp(
                prefix=f".{self.path.name}.", dir=self.path.parent)
            try:
                os.fchmod(fd, 0o600)
                with os.fdopen(fd, "w", encoding="utf-8") as stream:
                    json.dump(document, stream, ensure_ascii=False,
                              sort_keys=True, separators=(",", ":"))
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(temporary, self.path)
                directory_fd = os.open(self.path.parent, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
                self._records = new_records
            finally:
                try:
                    os.unlink(temporary)
                except FileNotFoundError:
                    pass

    def status(self) -> dict:
        with self._lock:
            return {"persistent": True, "records": len(self._records),
                    "last": self._records[-1] if self._records else None}

    def next_generation(self) -> int:
        with self._lock:
            if not self._records:
                return 1
            return int(self._records[-1]["generation"]) + 1

#!/usr/bin/env python3
"""Studio 本地生产批次历史：原子、有限、启动校验和显式隔离。"""

from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
import threading
from dataclasses import dataclass
from itertools import islice
from pathlib import Path

from production_batch import (
    MAX_BATCH_MANIFEST_BYTES,
    validate_production_batch_manifest,
)
from project_config import ProjectConfigError


DEFAULT_HISTORY_ROOT = Path(__file__).resolve().parent.parent / \
    "firmware" / "out" / "studio-history"
MAX_HISTORY_RECORDS = 256
MAX_HISTORY_TOTAL_BYTES = 16 * 1024 * 1024
MAX_HISTORY_QUERY_CHARS = 128
MAX_HISTORY_RESULTS = 50
MAX_HISTORY_DIRECTORY_ENTRIES = 1024
MAX_HISTORY_MARKER_BYTES = 64
_HASH_RE = re.compile(r"^[0-9a-f]{64}$")
_SEARCH_FIELDS = {
    "all", "project_sha256", "build_id", "board_id", "record_sha256",
}


@dataclass(frozen=True)
class StoredBatch:
    manifest: dict
    byte_count: int


def _json_bytes(value: object) -> bytes:
    try:
        return (json.dumps(value, ensure_ascii=False, allow_nan=False,
                           sort_keys=True, indent=2) + "\n").encode("utf-8")
    except (TypeError, ValueError, RecursionError) as error:
        raise ProjectConfigError(f"历史记录不是有效JSON：{error}") from error


def _summary(stored: StoredBatch, matched_fields: list[str] | None = None
             ) -> dict:
    manifest = stored.manifest
    validation = validate_production_batch_manifest(manifest)
    return {
        "manifest_sha256": manifest["manifest_sha256"],
        "batch_id": manifest["batch"]["batch_id"],
        "name": manifest["batch"]["name"],
        "note": manifest["batch"]["note"],
        "byte_count": stored.byte_count,
        "record_count": validation["summary"]["record_count"],
        "comparison_count": validation["summary"]["comparison_count"],
        "project_count": validation["summary"]["project_count"],
        "build_count": validation["summary"]["build_count"],
        "board_count": validation["summary"]["board_count"],
        "matched_fields": matched_fields or [],
    }


class ProductionHistoryStore:
    """只保存通过批次校验的不可变 JSON；用户输入从不成为路径。"""

    def __init__(self, root: Path, *, max_records: int = MAX_HISTORY_RECORDS,
                 max_total_bytes: int = MAX_HISTORY_TOTAL_BYTES,
                 max_record_bytes: int = MAX_BATCH_MANIFEST_BYTES):
        self.root = root.resolve()
        self.quarantine_root = self.root / "quarantine"
        self.max_records = max_records
        self.max_total_bytes = max_total_bytes
        self.max_record_bytes = max_record_bytes
        if max_records < 1 or max_total_bytes < 1 or max_record_bytes < 1:
            raise ProjectConfigError("本地历史容量参数必须为正数")
        self._lock = threading.RLock()
        self._records: dict[str, StoredBatch] = {}
        self._startup_issues: list[dict] = []
        self.root.mkdir(parents=True, exist_ok=True)
        self._marker = self.root / ".remotebsp-production-history-v1"
        self._ensure_owned_directory()
        self.quarantine_root.mkdir(parents=True, exist_ok=True)
        if self.quarantine_root.is_symlink() or \
                not self.quarantine_root.is_dir():
            raise ProjectConfigError("本地历史隔离区不是可信普通目录")
        self._audit_startup()

    def _ensure_owned_directory(self) -> None:
        marker_content = b"REMOTEBSP_PRODUCTION_HISTORY_V1\n"
        if self._marker.exists() or self._marker.is_symlink():
            if self._marker.is_symlink() or not self._marker.is_file():
                raise ProjectConfigError("本地历史目录标记损坏，拒绝审计")
            with self._marker.open("rb") as stream:
                marker_bytes = stream.read(MAX_HISTORY_MARKER_BYTES + 1)
            if marker_bytes != marker_content:
                raise ProjectConfigError("本地历史目录标记损坏，拒绝审计")
            return
        entries = list(islice(self.root.iterdir(), 2))
        if entries:
            raise ProjectConfigError(
                "本地历史目录非空且没有RemoteBSP标记，拒绝审计以保护原文件")
        try:
            with self._marker.open("xb") as stream:
                stream.write(marker_content)
                stream.flush()
                os.fsync(stream.fileno())
        except FileExistsError as error:
            raise ProjectConfigError("本地历史目录标记创建发生冲突") from error

    @staticmethod
    def _validate_key(manifest_sha256: object) -> str:
        if not isinstance(manifest_sha256, str) or not _HASH_RE.fullmatch(
                manifest_sha256):
            raise ProjectConfigError("历史记录ID必须是64位小写SHA-256")
        return manifest_sha256

    def _path(self, manifest_sha256: object) -> Path:
        key = self._validate_key(manifest_sha256)
        return self.root / f"{key}.json"

    def _quarantine(self, path: Path, reason: str) -> dict:
        try:
            content = path.name.encode() if path.is_symlink() else \
                path.read_bytes() if path.is_file() else path.name.encode()
        except OSError:
            content = path.name.encode()
        digest = hashlib.sha256(content + reason.encode("utf-8")).hexdigest()
        for counter in range(1000):
            suffix = "" if counter == 0 else f"-{counter}"
            destination = self.quarantine_root / \
                f"isolated-{digest[:24]}{suffix}.bad"
            if destination.exists():
                continue
            try:
                os.replace(path, destination)
                return {
                    "source_name": path.name,
                    "quarantine_name": destination.name,
                    "reason": reason,
                    "isolated": True,
                }
            except OSError as error:
                return {
                    "source_name": path.name,
                    "quarantine_name": None,
                    "reason": f"{reason}；隔离失败：{error}",
                    "isolated": False,
                }
        return {
            "source_name": path.name, "quarantine_name": None,
            "reason": f"{reason}；隔离区命名空间已满", "isolated": False,
        }

    def _read_verified(self, path: Path) -> StoredBatch:
        if path.is_symlink() or not path.is_file():
            raise ProjectConfigError("历史条目不是普通文件")
        with path.open("rb") as stream:
            content = stream.read(self.max_record_bytes + 1)
        if len(content) > self.max_record_bytes:
            raise ProjectConfigError(
                f"历史条目超过{self.max_record_bytes // 1024} KiB上限")
        try:
            manifest = json.loads(content.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ProjectConfigError(f"历史条目JSON损坏：{error}") from error
        validation = validate_production_batch_manifest(manifest)
        if not validation["valid"]:
            message = validation["issues"][0]["message"] \
                if validation["issues"] else "批次校验失败"
            raise ProjectConfigError(message)
        key = manifest["manifest_sha256"]
        if path.name != f"{key}.json":
            raise ProjectConfigError("历史文件名与清单SHA-256不一致")
        canonical = _json_bytes(manifest)
        if content != canonical:
            raise ProjectConfigError("历史文件不是规范JSON编码")
        return StoredBatch(manifest=manifest, byte_count=len(content))

    def _audit_startup(self) -> None:
        with self._lock:
            self._records.clear()
            self._startup_issues.clear()
            entries = list(islice(
                self.root.iterdir(), MAX_HISTORY_DIRECTORY_ENTRIES + 1))
            if len(entries) > MAX_HISTORY_DIRECTORY_ENTRIES:
                raise ProjectConfigError(
                    f"本地历史目录超过{MAX_HISTORY_DIRECTORY_ENTRIES}个条目审计上限")
            for path in sorted(entries, key=lambda item: item.name):
                if path in (self.quarantine_root, self._marker):
                    continue
                if path.name.startswith(".pending-") or path.suffix == ".tmp":
                    self._startup_issues.append(
                        self._quarantine(path, "发现未完成的原子写入临时文件"))
                    continue
                if path.suffix != ".json" and not path.is_symlink():
                    continue
                try:
                    stored = self._read_verified(path)
                    total_bytes = sum(
                        item.byte_count for item in self._records.values())
                    if len(self._records) >= self.max_records:
                        self._startup_issues.append(self._quarantine(
                            path, "有效历史条目超过配置的记录数量上限"))
                    elif total_bytes + stored.byte_count > self.max_total_bytes:
                        self._startup_issues.append(self._quarantine(
                            path, "有效历史条目超过配置的总字节容量上限"))
                    else:
                        self._records[
                            stored.manifest["manifest_sha256"]] = stored
                except (OSError, ProjectConfigError) as error:
                    self._startup_issues.append(
                        self._quarantine(path, str(error)))

    def _fsync_root(self) -> None:
        try:
            descriptor = os.open(self.root, os.O_RDONLY)
        except OSError:
            return
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def _atomic_write(self, destination: Path, content: bytes) -> None:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=".pending-", suffix=".tmp", dir=self.root)
        temporary = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            try:
                os.chmod(temporary, 0o600)
            except OSError:
                pass
            os.replace(temporary, destination)
            self._fsync_root()
        finally:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass

    def save(self, manifest: object) -> dict:
        validation = validate_production_batch_manifest(manifest)
        if not validation["valid"]:
            raise ProjectConfigError(
                f"批次清单校验失败：{validation['status_text']}")
        assert isinstance(manifest, dict)
        key = self._validate_key(manifest.get("manifest_sha256"))
        content = _json_bytes(manifest)
        if len(content) > self.max_record_bytes:
            raise ProjectConfigError(
                f"单条历史记录超过{self.max_record_bytes // 1024} KiB上限")
        destination = self._path(key)
        with self._lock:
            if key in self._records:
                stored = self.get(key)
                if _json_bytes(stored.manifest) != content:
                    raise ProjectConfigError("同一清单哈希对应不同内容")
                return {"stored": False, "already_exists": True,
                        "record": _summary(stored)}
            if destination.exists() or destination.is_symlink():
                issue = self._quarantine(destination,
                                         "目标文件已存在但未通过启动校验")
                self._startup_issues.append(issue)
                if not issue["isolated"]:
                    raise ProjectConfigError("冲突历史文件无法安全隔离")
            total_bytes = sum(item.byte_count for item in self._records.values())
            if len(self._records) >= self.max_records:
                raise ProjectConfigError(
                    f"本地历史已达到{self.max_records}条容量上限")
            if total_bytes + len(content) > self.max_total_bytes:
                raise ProjectConfigError(
                    f"本地历史已达到{self.max_total_bytes // 1024 // 1024} MiB容量上限")
            self._atomic_write(destination, content)
            stored = StoredBatch(manifest=json.loads(content),
                                 byte_count=len(content))
            self._records[key] = stored
            return {"stored": True, "already_exists": False,
                    "record": _summary(stored)}

    def get(self, manifest_sha256: object) -> StoredBatch:
        key = self._validate_key(manifest_sha256)
        with self._lock:
            if key not in self._records:
                raise ProjectConfigError("未找到该生产批次历史记录")
            path = self._path(key)
            try:
                stored = self._read_verified(path)
            except (OSError, ProjectConfigError) as error:
                issue = self._quarantine(path, f"读取时复核失败：{error}")
                self._startup_issues.append(issue)
                self._records.pop(key, None)
                raise ProjectConfigError(
                    "历史记录已损坏并从可检索索引中隔离") from error
            self._records[key] = stored
            return stored

    @staticmethod
    def _field_values(manifest: dict) -> dict[str, set[str]]:
        trace = manifest["trace_index"]
        return {
            "project_sha256": {item["key"] for item in
                               trace["by_project_sha256"]},
            "build_id": {item["key"] for item in trace["by_build_id"]},
            "board_id": {item["key"] for item in trace["by_board_id"]},
            "record_sha256": {item["key"] for item in
                              trace["by_record_sha256"]},
        }

    def search(self, query: object = "", field: object = "all",
               limit: object = MAX_HISTORY_RESULTS) -> dict:
        if not isinstance(query, str) or len(query) > MAX_HISTORY_QUERY_CHARS:
            raise ProjectConfigError(
                f"历史搜索词不能超过{MAX_HISTORY_QUERY_CHARS}个字符")
        if field not in _SEARCH_FIELDS:
            raise ProjectConfigError("历史搜索字段不受支持")
        if not isinstance(limit, int) or isinstance(limit, bool) or \
                not 1 <= limit <= MAX_HISTORY_RESULTS:
            raise ProjectConfigError(
                f"历史搜索条数必须位于1至{MAX_HISTORY_RESULTS}")
        needle = query.casefold()
        matches: list[dict] = []
        with self._lock:
            keys = sorted(self._records)
        for key in keys:
            try:
                stored = self.get(key)
            except ProjectConfigError:
                # get 已把启动后损坏的条目移出索引并记录隔离结果。
                continue
            values = self._field_values(stored.manifest)
            selected = values if field == "all" else {field: values[field]}
            matched = ["all"] if not needle else sorted(
                name for name, candidates in selected.items()
                if any(needle in value.casefold() for value in candidates))
            if matched:
                matches.append(_summary(stored, matched))
        matches.sort(key=lambda item: (item["batch_id"],
                                       item["manifest_sha256"]))
        return {
            "ok": True, "format": "PRODUCTION_HISTORY_SEARCH_V1",
            "query": query, "field": field,
            "total_matches": len(matches),
            "returned_count": min(len(matches), limit),
            "truncated": len(matches) > limit,
            "records": matches[:limit],
            "limits": {"max_query_chars": MAX_HISTORY_QUERY_CHARS,
                       "max_results": MAX_HISTORY_RESULTS},
        }

    def status(self) -> dict:
        with self._lock:
            quarantine_entries = list(islice(
                self.quarantine_root.iterdir(),
                MAX_HISTORY_DIRECTORY_ENTRIES + 1))
            quarantine_truncated = len(quarantine_entries) > \
                MAX_HISTORY_DIRECTORY_ENTRIES
            quarantined_count = sum(
                1 for item in quarantine_entries[:MAX_HISTORY_DIRECTORY_ENTRIES]
                if item.is_file() or item.is_symlink())
            total_bytes = sum(item.byte_count for item in self._records.values())
            visible_issues = self._startup_issues[:32]
            return {
                "ok": True, "format": "PRODUCTION_HISTORY_STATUS_V1",
                "status": "attention" if self._startup_issues else "ready",
                "status_text": (
                    f"有 {len(self._startup_issues)} 条损坏或异常记录已隔离"
                    if self._startup_issues else "本地生产历史已校验，可安全检索"),
                "valid_record_count": len(self._records),
                "total_bytes": total_bytes,
                "quarantined_count": quarantined_count,
                "quarantined_count_truncated": quarantine_truncated,
                "startup_issue_count": len(self._startup_issues),
                "startup_issues": visible_issues,
                "startup_issues_truncated":
                    len(self._startup_issues) > len(visible_issues),
                "limits": {
                    "max_records": self.max_records,
                    "max_total_bytes": self.max_total_bytes,
                    "max_record_bytes": self.max_record_bytes,
                    "max_query_chars": MAX_HISTORY_QUERY_CHARS,
                    "max_results": MAX_HISTORY_RESULTS,
                },
                "declaration": (
                    "本地历史只保存通过校验的软件批次清单；不执行构建、烧录、"
                    "硬件访问，也不把隔离记录伪装成成功记录。"),
            }

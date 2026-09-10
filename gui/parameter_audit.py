#!/usr/bin/env python3
"""设备参数变更的本地、脱敏、带密钥完整性审计。"""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import tempfile
import uuid
from datetime import datetime, timezone
from pathlib import Path

from device_parameters import DeviceParameterError


MAX_AUDIT_RECORDS = 4096
MAX_AUDIT_RECORD_BYTES = 32 * 1024
MAX_AUDIT_KEY_BYTES = 128
_OPERATION_ID = re.compile(r"^[0-9a-f]{32}$")


def _canonical(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, allow_nan=False,
                      sort_keys=True, separators=(",", ":")).encode("utf-8")


def _load_key(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise DeviceParameterError("参数审计密钥必须是普通文件")
    content = path.read_bytes()
    if len(content) > MAX_AUDIT_KEY_BYTES:
        raise DeviceParameterError("参数审计密钥超过128字节上限")
    stripped = content.strip()
    if re.fullmatch(rb"[0-9a-fA-F]{64}", stripped):
        key = bytes.fromhex(stripped.decode("ascii"))
    else:
        key = stripped
    if len(key) < 32:
        raise DeviceParameterError("参数审计密钥至少需要32字节")
    return key


class ParameterAuditStore:
    """每次变更一个原子JSON文件；未完成操作保留intent供恢复审计。"""

    MARKER = ".remotebsp-parameter-audit-v1"

    def __init__(self, root: Path, key_file: Path):
        self.root = root
        self.key = _load_key(key_file)
        self.key_id = hashlib.sha256(self.key).hexdigest()[:16]
        if root.exists():
            if root.is_symlink() or not root.is_dir():
                raise DeviceParameterError("参数审计路径必须是普通目录")
        else:
            if not root.parent.is_dir() or root.parent.is_symlink():
                raise DeviceParameterError("参数审计父目录无效")
            root.mkdir()
        marker = root / self.MARKER
        others = [item for item in root.iterdir() if item.name != self.MARKER]
        if marker.exists():
            if marker.is_symlink() or marker.read_text(encoding="ascii") != "1\n":
                raise DeviceParameterError("参数审计目录标记损坏")
        elif others:
            raise DeviceParameterError("参数审计目录非空且没有有效标记")
        else:
            self._atomic_write(marker, b"1\n", replace=False)
        records = self._record_paths()
        if len(records) > MAX_AUDIT_RECORDS:
            raise DeviceParameterError("参数审计记录数量超过4096项上限")
        for path in records:
            self._load(path)

    def _record_paths(self) -> list[Path]:
        return sorted(self.root.glob("audit-*.json"))

    @staticmethod
    def _atomic_write(path: Path, content: bytes, *, replace: bool) -> None:
        descriptor, name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
        temporary = Path(name)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            if replace:
                os.replace(temporary, path)
            else:
                os.link(temporary, path)
                temporary.unlink()
        finally:
            temporary.unlink(missing_ok=True)

    def _sign(self, event: dict) -> str:
        unsigned = dict(event)
        unsigned.pop("event_hmac_sha256", None)
        return hmac.new(self.key, _canonical(unsigned), hashlib.sha256).hexdigest()

    def _encode(self, record: dict) -> bytes:
        content = (json.dumps(record, ensure_ascii=False, allow_nan=False,
                              sort_keys=True, indent=2) + "\n").encode("utf-8")
        if len(content) > MAX_AUDIT_RECORD_BYTES:
            raise DeviceParameterError("参数审计记录超过32 KiB上限")
        return content

    def _load(self, path: Path) -> dict:
        if path.is_symlink() or not path.is_file() or \
                path.stat().st_size > MAX_AUDIT_RECORD_BYTES:
            raise DeviceParameterError("参数审计记录文件无效")
        try:
            record = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise DeviceParameterError("参数审计记录无法解析") from error
        if not isinstance(record, dict) or set(record) != {
                "schema_version", "format", "operation_id", "key_id",
                "events"} or record.get("schema_version") != 1 or \
                record.get("format") != "REMOTEBSP_PARAMETER_AUDIT_V1" or \
                not _OPERATION_ID.fullmatch(str(record.get("operation_id", ""))) or \
                record.get("key_id") != self.key_id or \
                not isinstance(record.get("events"), list) or \
                len(record["events"]) not in (1, 2):
            raise DeviceParameterError("参数审计记录结构或密钥标识无效")
        previous = None
        for index, event in enumerate(record["events"]):
            if not isinstance(event, dict) or event.get("phase") not in (
                    "intent", "terminal") or event.get("previous_hmac_sha256") != previous:
                raise DeviceParameterError("参数审计事件链无效")
            claimed = event.get("event_hmac_sha256")
            if not isinstance(claimed, str) or not hmac.compare_digest(
                    claimed, self._sign(event)):
                raise DeviceParameterError("参数审计HMAC不匹配，记录可能已被篡改")
            if index == 0 and event["phase"] != "intent" or \
                    index == 1 and event["phase"] != "terminal":
                raise DeviceParameterError("参数审计事件顺序无效")
            previous = claimed
        return record

    def begin(self, *, operation: str, node_id: int, node_uuid: str,
              expected_generation: int, parameters: list[dict],
              operator: dict | None = None) -> str:
        if len(self._record_paths()) >= MAX_AUDIT_RECORDS:
            raise DeviceParameterError("参数审计容量已满，拒绝执行未记录的写入")
        if operation not in ("write", "restore") or not parameters or \
                len(parameters) > 64:
            raise DeviceParameterError("参数审计意图无效")
        operation_id = uuid.uuid4().hex
        if operator is not None and (not isinstance(operator, dict) or
                set(operator) != {"identity", "role", "policy_sha256"} or
                operator.get("role") not in ("provisioner", "supervisor") or
                not isinstance(operator.get("identity"), str) or
                not re.fullmatch(r"[a-zA-Z0-9_.-]{1,64}", operator["identity"]) or
                not re.fullmatch(r"[0-9a-f]{64}", str(operator.get("policy_sha256", "")))):
            raise DeviceParameterError("参数审计生产操作员证据无效")
        event = {
            "phase": "intent", "operation": operation,
            "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
            "time_source": "host_system_clock_untrusted",
            "operator": ({"source": "production_policy",
                          "identity": operator["identity"],
                          "role": operator["role"],
                          "policy_sha256": operator["policy_sha256"],
                          "authorized_by_policy": True,
                          "authenticated": False} if operator is not None else
                         {"source": "local_cli", "identity": None,
                          "authenticated": False}),
            "target": {"node_id": node_id, "node_uuid": node_uuid,
                       "expected_generation": expected_generation},
            "parameters": parameters,
            "previous_hmac_sha256": None,
        }
        event["event_hmac_sha256"] = self._sign(event)
        record = {"schema_version": 1,
                  "format": "REMOTEBSP_PARAMETER_AUDIT_V1",
                  "operation_id": operation_id, "key_id": self.key_id,
                  "events": [event]}
        path = self.root / f"audit-{operation_id}.json"
        self._atomic_write(path, self._encode(record), replace=False)
        # 容量检查与创建之间可能有并发者；超界时只撤销本次尚未执行的意图。
        if len(self._record_paths()) > MAX_AUDIT_RECORDS:
            path.unlink(missing_ok=True)
            raise DeviceParameterError("参数审计容量已满，拒绝执行未记录的写入")
        return operation_id

    def finish(self, operation_id: str, *, outcome: str,
               generation_after: int | None, applied_count: int,
               error_type: str | None = None) -> dict:
        path = self.root / f"audit-{operation_id}.json"
        record = self._load(path)
        if len(record["events"]) != 1 or outcome not in (
                "success", "failure", "partial_failure"):
            raise DeviceParameterError("参数审计终态无效或已经写入")
        event = {
            "phase": "terminal", "outcome": outcome,
            "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
            "time_source": "host_system_clock_untrusted",
            "generation_after": generation_after,
            "applied_count": applied_count,
            "error_type": error_type,
            "previous_hmac_sha256": record["events"][0]["event_hmac_sha256"],
        }
        event["event_hmac_sha256"] = self._sign(event)
        record["events"].append(event)
        self._atomic_write(path, self._encode(record), replace=True)
        return record

    def get(self, operation_id: str) -> dict:
        if not _OPERATION_ID.fullmatch(operation_id):
            raise DeviceParameterError("参数审计操作ID无效")
        return self._load(self.root / f"audit-{operation_id}.json")


def parameter_evidence(parameter_id: int, value: bytes) -> dict:
    """生成不含值明文的参数证据。"""
    return {"id": parameter_id, "byte_count": len(value),
            "value_sha256": hashlib.sha256(value).hexdigest()}

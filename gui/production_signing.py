#!/usr/bin/env python3
"""生产证据的离线 Ed25519 脱离签名；不把本机时间冒充可信时间。"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

from project_config import ProjectConfigError

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey, Ed25519PublicKey)
except ImportError:  # pragma: no cover - 由调用入口转换为明确错误
    InvalidSignature = None
    Ed25519PrivateKey = Ed25519PublicKey = None
    serialization = None


SIGNATURE_SCHEMA_VERSION = 1
MAX_SIGNATURE_BYTES = 16 * 1024
_FORMATS = {
    "REMOTEBSP_PRODUCTION_BATCH_V1": "production_batch_manifest",
    "REMOTEBSP_DEPLOYMENT_RECORD_V1": "deployment_record",
}
_SIGNATURE_DOMAIN = b"REMOTEBSP_OFFLINE_EVIDENCE_SIGNATURE_V1\x00"


def _require_backend() -> None:
    if Ed25519PrivateKey is None:
        raise ProjectConfigError(
            "Ed25519签名需要cryptography依赖；拒绝降级为普通哈希或HMAC")


def canonical_evidence(value: object) -> tuple[str, bytes]:
    if not isinstance(value, dict) or value.get("format") not in _FORMATS:
        raise ProjectConfigError("只允许签名生产批次清单或部署记录")
    try:
        content = (json.dumps(value, ensure_ascii=False, allow_nan=False,
                              sort_keys=True, indent=2) + "\n").encode("utf-8")
    except (TypeError, ValueError, RecursionError) as error:
        raise ProjectConfigError(f"待签名证据不是有效JSON：{error}") from error
    return _FORMATS[value["format"]], content


def _load_private(path: Path):
    _require_backend()
    try:
        key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    except (OSError, ValueError, TypeError) as error:
        raise ProjectConfigError(f"Ed25519私钥无法读取：{error}") from error
    if not isinstance(key, Ed25519PrivateKey):
        raise ProjectConfigError("私钥不是Ed25519密钥")
    return key


def _load_public(path: Path):
    _require_backend()
    try:
        key = serialization.load_pem_public_key(path.read_bytes())
    except (OSError, ValueError, TypeError) as error:
        raise ProjectConfigError(f"Ed25519公钥无法读取：{error}") from error
    if not isinstance(key, Ed25519PublicKey):
        raise ProjectConfigError("公钥不是Ed25519密钥")
    return key


def public_key_id(key) -> str:
    raw = key.public_bytes(serialization.Encoding.Raw,
                           serialization.PublicFormat.Raw)
    return "ed25519:" + hashlib.sha256(raw).hexdigest()


def _signature_input(subject: dict, recorded_time: dict,
                     evidence_content: bytes) -> bytes:
    metadata = json.dumps(
        {"subject": subject, "recorded_time": recorded_time},
        ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":")).encode("utf-8")
    return (_SIGNATURE_DOMAIN + len(metadata).to_bytes(8, "big") + metadata +
            len(evidence_content).to_bytes(8, "big") + evidence_content)


def _validate_recorded_time(value: object) -> dict:
    if not isinstance(value, dict) or set(value) != {
            "value_utc", "source", "trusted", "note"} or \
            value.get("source") != "host_system_clock" or \
            value.get("trusted") is not False or \
            not isinstance(value.get("note"), str) or \
            not 0 < len(value["note"]) <= 256:
        raise ProjectConfigError("签名时间来源声明无效")
    timestamp_text = value.get("value_utc")
    if not isinstance(timestamp_text, str) or not timestamp_text.endswith("Z"):
        raise ProjectConfigError("签名时间必须是规范UTC格式")
    try:
        timestamp = datetime.fromisoformat(timestamp_text[:-1] + "+00:00")
    except ValueError as error:
        raise ProjectConfigError("签名时间必须是规范UTC格式") from error
    canonical = timestamp.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
    if timestamp.utcoffset() is None or timestamp.utcoffset().total_seconds() != 0 \
            or canonical != timestamp_text:
        raise ProjectConfigError("签名时间必须是规范UTC格式")
    return value


def generate_key_pair(private_path: Path, public_path: Path) -> dict:
    _require_backend()
    private_path = private_path.resolve()
    public_path = public_path.resolve()
    if private_path == public_path:
        raise ProjectConfigError("公钥与私钥输出不能是同一路径")
    private = Ed25519PrivateKey.generate()
    public = private.public_key()
    private_content = private.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption())
    public_content = public.public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo)
    temporary: list[Path] = []
    published: list[Path] = []
    try:
        for destination, content, mode in (
                (private_path, private_content, 0o600),
                (public_path, public_content, 0o644)):
            descriptor, name = tempfile.mkstemp(
                prefix=".remotebsp-key-", suffix=".tmp",
                dir=destination.parent)
            staging = Path(name)
            temporary.append(staging)
            with os.fdopen(descriptor, "wb") as stream:
                os.chmod(staging, mode)
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
        # 同目录硬链接是原子且拒绝已存在目标；避免 exists→write 的竞态覆盖。
        for staging, destination in zip(temporary, (private_path, public_path)):
            os.link(staging, destination)
            published.append(destination)
        for staging in temporary:
            staging.unlink()
    except FileExistsError as error:
        for destination in reversed(published):
            destination.unlink(missing_ok=True)
        for staging in temporary:
            staging.unlink(missing_ok=True)
        raise ProjectConfigError("密钥输出已存在，拒绝覆盖") from error
    except Exception as error:
        for destination in reversed(published):
            destination.unlink(missing_ok=True)
        for staging in temporary:
            staging.unlink(missing_ok=True)
        raise ProjectConfigError(f"密钥对原子写入失败：{error}") from error
    return {"key_id": public_key_id(public), "private_key": str(private_path),
            "public_key": str(public_path)}


def sign_evidence(value: object, private_path: Path, *,
                  clock: Callable[[], datetime] | None = None) -> dict:
    kind, content = canonical_evidence(value)
    private = _load_private(private_path)
    now = (clock or (lambda: datetime.now(timezone.utc)))()
    if not isinstance(now, datetime) or now.tzinfo is None:
        raise ProjectConfigError("签名记录时间必须包含时区")
    subject = {"kind": kind, "sha256": hashlib.sha256(content).hexdigest()}
    recorded_time = {
        "value_utc": now.astimezone(timezone.utc).isoformat().replace("+00:00", "Z"),
        "source": "host_system_clock", "trusted": False,
        "note": "本机时间仅用于排序；Ed25519证明签署内容和密钥身份，不证明签署时刻。",
    }
    signature = private.sign(_signature_input(subject, recorded_time, content))
    return {
        "schema_version": SIGNATURE_SCHEMA_VERSION,
        "format": "REMOTEBSP_OFFLINE_SIGNATURE_V1",
        "algorithm": "Ed25519",
        "key_id": public_key_id(private.public_key()),
        "subject": subject,
        "signature_base64": base64.b64encode(signature).decode("ascii"),
        "recorded_time": recorded_time,
    }


def verify_evidence(value: object, envelope: object, public_path: Path) -> dict:
    kind, content = canonical_evidence(value)
    if not isinstance(envelope, dict) or set(envelope) != {
            "schema_version", "format", "algorithm", "key_id", "subject",
            "signature_base64", "recorded_time"}:
        raise ProjectConfigError("签名信封字段集合无效")
    public = _load_public(public_path)
    expected_key = public_key_id(public)
    subject = envelope.get("subject")
    recorded = _validate_recorded_time(envelope.get("recorded_time"))
    if envelope.get("schema_version") != 1 or envelope.get("format") != \
            "REMOTEBSP_OFFLINE_SIGNATURE_V1" or envelope.get("algorithm") != "Ed25519" \
            or envelope.get("key_id") != expected_key:
        raise ProjectConfigError("签名版本、算法或公钥身份不匹配")
    if subject != {"kind": kind, "sha256": hashlib.sha256(content).hexdigest()}:
        raise ProjectConfigError("签名绑定的证据类型或SHA-256不匹配")
    try:
        signature = base64.b64decode(envelope["signature_base64"], validate=True)
        public.verify(signature, _signature_input(subject, recorded, content))
    except (ValueError, TypeError, InvalidSignature) as error:
        raise ProjectConfigError("Ed25519签名验证失败") from error
    return {"valid": True, "algorithm": "Ed25519", "key_id": expected_key,
            "subject": subject, "time_trusted": False}

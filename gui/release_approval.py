#!/usr/bin/env python3
"""发布审批与外部可信时间证明；本模块不自行签发可信时间。"""

from __future__ import annotations

import base64
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

from project_config import ProjectConfigError
from production_signing import (
    InvalidSignature, _load_private, _require_backend, canonical_evidence,
    public_key_id, serialization, verify_evidence_with_policy,
    Ed25519PublicKey)

POLICY_FORMAT = "REMOTEBSP_RELEASE_AUTHORITY_POLICY_V1"
APPROVAL_FORMAT = "REMOTEBSP_RELEASE_APPROVAL_V1"
TIME_FORMAT = "REMOTEBSP_EXTERNAL_TIME_ATTESTATION_V1"
_APPROVAL_DOMAIN = b"REMOTEBSP_RELEASE_APPROVAL_V1\0"
_TIME_DOMAIN = b"REMOTEBSP_EXTERNAL_TIME_ATTESTATION_V1\0"
MAX_AUTHORITY_POLICY_BYTES = 128 * 1024
MAX_PUBLIC_KEY_PEM_CHARS = 4096
MAX_NOTE_CHARS = 512
MAX_SIGNATURE_BASE64_CHARS = 256
MAX_TIME_CHARS = 40


def _canonical(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False,
                      sort_keys=True, separators=(",", ":")).encode()


def _digest(value):
    return hashlib.sha256(_canonical({k: v for k, v in value.items()
                                      if k not in ("sha256", "signature_base64")})).hexdigest()


def create_authority_policy(entries: list[dict]) -> dict:
    if not isinstance(entries, list) or not 1 <= len(entries) <= 32 or any(
            not isinstance(item, dict) or
            not isinstance(item.get("public_key_pem"), str) or
            len(item["public_key_pem"]) > MAX_PUBLIC_KEY_PEM_CHARS
            for item in entries):
        raise ProjectConfigError("发布权威策略输入数量、类型或PEM长度无效")
    policy = {"format": POLICY_FORMAT, "schema_version": 1, "authorities": entries}
    policy["sha256"] = hashlib.sha256(_canonical(policy)).hexdigest()
    return validate_authority_policy(policy)


def validate_authority_policy(value: object) -> dict:
    if not isinstance(value, dict):
        raise ProjectConfigError("发布权威策略必须是JSON对象")
    try:
        encoded = _canonical(value)
    except (TypeError, ValueError, RecursionError) as error:
        raise ProjectConfigError("发布权威策略不是有界标准JSON") from error
    if len(encoded) > MAX_AUTHORITY_POLICY_BYTES or \
            set(value) != {"format", "schema_version",
            "authorities", "sha256"} or value.get("format") != POLICY_FORMAT or \
            value.get("schema_version") != 1 or value.get("sha256") != \
            hashlib.sha256(_canonical({k: v for k, v in value.items() if k != "sha256"})).hexdigest():
        raise ProjectConfigError("发布权威策略格式或SHA-256无效")
    entries = value["authorities"]
    if not isinstance(entries, list) or not 1 <= len(entries) <= 32:
        raise ProjectConfigError("发布权威数量必须位于1～32")
    seen = set()
    for item in entries:
        if not isinstance(item, dict) or set(item) != {"key_id", "role", "status", "public_key_pem"} or \
                item.get("role") not in ("release_approver", "time_authority") or \
                item.get("status") not in ("active", "revoked") or item.get("key_id") in seen:
            raise ProjectConfigError("发布权威条目重复或字段无效")
        pem = item.get("public_key_pem")
        if not isinstance(pem, str) or not 1 <= len(pem) <= MAX_PUBLIC_KEY_PEM_CHARS:
            raise ProjectConfigError("发布权威公钥文本类型或长度无效")
        _require_backend()
        try: public = serialization.load_pem_public_key(pem.encode("ascii"))
        except Exception as error: raise ProjectConfigError("发布权威公钥无效") from error
        if not isinstance(public, Ed25519PublicKey) or \
                item["key_id"] != public_key_id(public):
            raise ProjectConfigError("发布权威公钥身份不匹配")
        seen.add(item["key_id"])
    return value


def _authority(policy, key_id, role):
    policy = validate_authority_policy(policy)
    matches = [x for x in policy["authorities"] if x["key_id"] == key_id and x["role"] == role]
    if len(matches) != 1 or matches[0]["status"] != "active":
        raise ProjectConfigError(f"{role}未授权或已撤销")
    return serialization.load_pem_public_key(matches[0]["public_key_pem"].encode("ascii"))


def approve_release(evidence: dict, evidence_signature: dict,
                    evidence_trust_policy: dict, private_key: Path,
                    authority_policy: dict,
                    *, decision: str, note: str) -> dict:
    verified = verify_evidence_with_policy(evidence, evidence_signature,
                                           evidence_trust_policy)
    if decision not in ("approved", "rejected") or not isinstance(note, str) or len(note) > MAX_NOTE_CHARS:
        raise ProjectConfigError("发布审批决定或备注无效")
    private = _load_private(private_key)
    _authority(authority_policy, public_key_id(private.public_key()),
               "release_approver")
    approval = {"format": APPROVAL_FORMAT, "schema_version": 1,
        "decision": decision, "note": note,
        "approver_key_id": public_key_id(private.public_key()),
        "evidence_subject": verified["subject"],
        "evidence_signature_sha256": hashlib.sha256(_canonical(evidence_signature)).hexdigest(),
        "recorded_time": {"source": "host_system_clock", "trusted": False}}
    approval["sha256"] = _digest(approval)
    approval["signature_base64"] = base64.b64encode(
        private.sign(_APPROVAL_DOMAIN + bytes.fromhex(approval["sha256"]))).decode()
    return approval


def verify_release_approval(evidence: dict, evidence_signature: dict,
                            evidence_trust_policy: dict, approval: object,
                            authority_policy: dict) -> dict:
    verified = verify_evidence_with_policy(evidence, evidence_signature,
                                           evidence_trust_policy)
    fields = {"format", "schema_version", "decision", "note", "approver_key_id",
              "evidence_subject", "evidence_signature_sha256", "recorded_time",
              "sha256", "signature_base64"}
    if not isinstance(approval, dict) or set(approval) != fields or \
            approval.get("format") != APPROVAL_FORMAT or approval.get("schema_version") != 1 or \
            not isinstance(approval.get("note"), str) or len(approval["note"]) > MAX_NOTE_CHARS or \
            not isinstance(approval.get("signature_base64"), str) or \
            len(approval["signature_base64"]) > MAX_SIGNATURE_BASE64_CHARS or \
            approval.get("sha256") != _digest(approval) or approval.get("evidence_subject") != verified["subject"] or \
            approval.get("evidence_signature_sha256") != hashlib.sha256(_canonical(evidence_signature)).hexdigest() or \
            approval.get("recorded_time") != {"source": "host_system_clock", "trusted": False}:
        raise ProjectConfigError("发布审批格式、绑定或自哈希无效")
    if approval.get("decision") not in ("approved", "rejected"):
        raise ProjectConfigError("发布审批决定或备注无效")
    public = _authority(authority_policy, approval.get("approver_key_id"), "release_approver")
    try:
        signature = base64.b64decode(approval["signature_base64"], validate=True)
        public.verify(signature, _APPROVAL_DOMAIN + bytes.fromhex(approval["sha256"]))
    except (ValueError, TypeError, InvalidSignature) as error:
        raise ProjectConfigError("发布审批Ed25519签名无效") from error
    return {"valid": True, "decision": approval["decision"],
            "approval_sha256": approval["sha256"], "time_trusted": False}


def verify_external_time_attestation(approval: dict, attestation: object,
                                     authority_policy: dict) -> dict:
    approval_fields = {"format", "schema_version", "decision", "note",
        "approver_key_id", "evidence_subject", "evidence_signature_sha256",
        "recorded_time", "sha256", "signature_base64"}
    if not isinstance(approval, dict) or set(approval) != approval_fields or \
            approval.get("format") != APPROVAL_FORMAT or \
            approval.get("schema_version") != 1 or \
            approval.get("decision") not in ("approved", "rejected") or \
            not isinstance(approval.get("note"), str) or len(approval["note"]) > MAX_NOTE_CHARS or \
            approval.get("recorded_time") != {"source": "host_system_clock", "trusted": False} or \
            not isinstance(approval.get("signature_base64"), str) or \
            len(approval["signature_base64"]) > MAX_SIGNATURE_BASE64_CHARS or \
            approval.get("sha256") != _digest(approval):
        raise ProjectConfigError("外部时间证明绑定的发布审批无效")
    approver = _authority(authority_policy, approval.get("approver_key_id"),
                          "release_approver")
    try:
        approver.verify(base64.b64decode(approval["signature_base64"], validate=True),
                        _APPROVAL_DOMAIN + bytes.fromhex(approval["sha256"]))
    except (ValueError, TypeError, InvalidSignature) as error:
        raise ProjectConfigError("外部时间证明绑定的发布审批签名无效") from error
    fields = {"format", "schema_version", "approval_sha256", "time_utc",
              "authority_key_id", "sha256", "signature_base64"}
    if not isinstance(attestation, dict) or set(attestation) != fields or \
            attestation.get("format") != TIME_FORMAT or attestation.get("schema_version") != 1 or \
            attestation.get("approval_sha256") != approval.get("sha256") or \
            not isinstance(attestation.get("signature_base64"), str) or \
            len(attestation["signature_base64"]) > MAX_SIGNATURE_BASE64_CHARS or \
            not isinstance(attestation.get("time_utc"), str) or \
            len(attestation["time_utc"]) > MAX_TIME_CHARS or \
            attestation.get("sha256") != _digest(attestation):
        raise ProjectConfigError("外部时间证明格式、绑定或自哈希无效")
    text = attestation.get("time_utc")
    try:
        parsed = (datetime.fromisoformat(text[:-1] + "+00:00")
                  if isinstance(text, str) and len(text) <= MAX_TIME_CHARS and
                  text.endswith("Z") else None)
    except ValueError as error: raise ProjectConfigError("外部可信时间格式无效") from error
    canonical = (parsed.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
                 if parsed is not None else None)
    if parsed is None or parsed.utcoffset() != timezone.utc.utcoffset(parsed) or canonical != text:
        raise ProjectConfigError("外部可信时间必须是规范UTC")
    public = _authority(authority_policy, attestation.get("authority_key_id"), "time_authority")
    try:
        public.verify(base64.b64decode(attestation["signature_base64"], validate=True),
                      _TIME_DOMAIN + bytes.fromhex(attestation["sha256"]))
    except (ValueError, TypeError, InvalidSignature) as error:
        raise ProjectConfigError("外部时间证明Ed25519签名无效") from error
    return {"valid": True, "time_trusted": True, "trusted_time_utc": text,
            "authority_key_id": attestation["authority_key_id"]}


def _sign_external_time_for_test(private_key: Path, approval_sha256: str,
                                 time_utc: str) -> dict:
    """仅供离线测试夹具模拟外部签发者；不暴露CLI。"""
    private = _load_private(private_key)
    item = {"format": TIME_FORMAT, "schema_version": 1,
            "approval_sha256": approval_sha256, "time_utc": time_utc,
            "authority_key_id": public_key_id(private.public_key())}
    item["sha256"] = _digest(item)
    item["signature_base64"] = base64.b64encode(private.sign(
        _TIME_DOMAIN + bytes.fromhex(item["sha256"]))).decode()
    return item

#!/usr/bin/env python3
"""已核验固件部署的版本化、可校验结果记录。"""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

from firmware_builder import FirmwareBuildError, resolve_artifact
from firmware_deployment import DeploymentResult, FirmwareDeploymentError


DEPLOYMENT_RECORD_SCHEMA_VERSION = 1
MAX_DEPLOYMENT_RECORD_BYTES = 32 * 1024
_HASH = re.compile(r"^[0-9a-f]{64}$")
_BUILD_ID = re.compile(r"^[a-z0-9-]{8,96}$")
_UUID = re.compile(r"^[0-9a-f]{32}$")
_BACKEND = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")


@dataclass(frozen=True)
class DeploymentRecordResult:
    record: dict
    content: bytes
    sha256: str
    filename: str


def _json_bytes(value: object) -> bytes:
    try:
        return (json.dumps(value, ensure_ascii=False, allow_nan=False,
                           sort_keys=True, indent=2) + "\n").encode("utf-8")
    except (TypeError, ValueError, RecursionError) as error:
        raise FirmwareDeploymentError(
            f"部署记录不是有效JSON：{error}") from error


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _strict_object(pairs: list[tuple[str, object]]) -> dict:
    result: dict = {}
    for key, value in pairs:
        if key in result:
            raise FirmwareDeploymentError(f"部署记录包含重复字段：{key}")
        result[key] = value
    return result


def _record_hash(record: dict) -> str:
    unsigned = dict(record)
    unsigned.pop("record_sha256", None)
    return hashlib.sha256(_json_bytes(unsigned)).hexdigest()


def create_deployment_record(
        result: DeploymentResult, *, output_root: Path,
        clock: Callable[[], datetime] | None = None) -> DeploymentRecordResult:
    """仅从完整核验成功的部署结果生成记录，并再次绑定受保护产物。"""
    if not isinstance(result, DeploymentResult) or result.verified is not True:
        raise FirmwareDeploymentError("只有完整身份核验成功的部署才能生成记录")
    if result.expected.board_id != result.observed.board_id or any(
            getattr(result.expected, field) != getattr(result.observed, field)
            for field in ("project_sha256", "config_sha256",
                          "firmware_identity_sha256")):
        raise FirmwareDeploymentError("部署结果身份不一致，拒绝生成成功记录")
    if not _UUID.fullmatch(result.observed.device_uuid):
        raise FirmwareDeploymentError("部署结果device_uuid必须是32位小写十六进制")
    if type(result.attempts) is not int or not 1 <= result.attempts <= 10000:
        raise FirmwareDeploymentError("部署结果attempts范围无效")
    try:
        build_record_path = resolve_artifact(
            result.build_id, "build-record.json", output_root)
        artifact_filename = ("firmware.bin" if result.backend in {
                             "can-katapult", "usb-katapult"}
                             else "firmware.elf")
        artifact_path = resolve_artifact(
            result.build_id, artifact_filename, output_root)
    except FirmwareBuildError as error:
        raise FirmwareDeploymentError(
            f"部署记录无法绑定构建产物：{error}") from error
    build_record_size = build_record_path.stat().st_size
    artifact_size = artifact_path.stat().st_size
    if build_record_size > 128 * 1024 or artifact_size <= 0:
        raise FirmwareDeploymentError("部署记录绑定的构建记录或固件大小无效")
    now = (clock or (lambda: datetime.now(timezone.utc)))()
    if not isinstance(now, datetime) or now.tzinfo is None:
        raise FirmwareDeploymentError("部署记录时间必须包含时区")
    recorded_at = now.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
    hashes = {
        "project_sha256": result.observed.project_sha256,
        "config_sha256": result.observed.config_sha256,
        "firmware_identity_sha256": result.observed.firmware_identity_sha256,
    }
    if any(not _HASH.fullmatch(value) for value in hashes.values()):
        raise FirmwareDeploymentError("部署结果包含非规范SHA-256")
    record = {
        "schema_version": DEPLOYMENT_RECORD_SCHEMA_VERSION,
        "format": "REMOTEBSP_DEPLOYMENT_RECORD_V1",
        "status": "firmware_flash_verified",
        "deployment": {
            "build_id": result.build_id,
            "board_id": result.observed.board_id,
            "device_uuid": result.observed.device_uuid,
            "backend": result.backend,
            "attempts": result.attempts,
            "verified": True,
        },
        "observed_identity": hashes,
        "source_evidence": {
            "build_record": {
                "filename": "build-record.json", "size": build_record_size,
                "sha256": _sha256(build_record_path),
            },
            "flashed_artifact": {
                "filename": artifact_filename, "size": artifact_size,
                "sha256": _sha256(artifact_path),
            },
        },
        "recorded_time": {
            "value_utc": recorded_at,
            "source": "host_system_clock",
            "trusted": False,
            "note": "主机系统时钟未经过可信时间源证明；仅用于排序，不作为审计时间戳。",
        },
        "declaration": (
            f"本记录证明一次{('CAN Katapult APP' if result.backend == 'can-katapult' else 'USB Katapult APP' if result.backend == 'usb-katapult' else 'ST-Link')}写入已完成且运行中设备通过四重身份核验；"
            "不证明外设功能或波形已经通过硬件测试。"),
    }
    record["record_sha256"] = _record_hash(record)
    validated = validate_deployment_record(record)
    content = _json_bytes(validated)
    if len(content) > MAX_DEPLOYMENT_RECORD_BYTES:
        raise FirmwareDeploymentError("部署记录超过32 KiB上限")
    return DeploymentRecordResult(
        validated, content, hashlib.sha256(content).hexdigest(),
        f"{result.build_id}-{result.observed.device_uuid}-部署记录-v1.json")


def validate_deployment_record(value: object) -> dict:
    """严格校验字段集合、自哈希、来源大小及成功语义。"""
    if not isinstance(value, dict):
        raise FirmwareDeploymentError("部署记录必须是JSON对象")
    if len(_json_bytes(value)) > MAX_DEPLOYMENT_RECORD_BYTES:
        raise FirmwareDeploymentError("部署记录超过32 KiB上限")
    if set(value) != {"schema_version", "format", "status", "deployment",
                      "observed_identity", "source_evidence", "recorded_time",
                      "declaration", "record_sha256"}:
        raise FirmwareDeploymentError("部署记录顶层字段集合与v1不一致")
    if value.get("schema_version") != 1 or value.get("format") != \
            "REMOTEBSP_DEPLOYMENT_RECORD_V1":
        raise FirmwareDeploymentError("部署记录格式或版本不受支持")
    if value.get("status") != "firmware_flash_verified":
        raise FirmwareDeploymentError("部署记录不是已核验成功状态")
    deployment = value.get("deployment")
    if not isinstance(deployment, dict) or set(deployment) != {
            "build_id", "board_id", "device_uuid", "backend", "attempts",
            "verified"}:
        raise FirmwareDeploymentError("部署记录deployment字段无效")
    if not _BUILD_ID.fullmatch(str(deployment.get("build_id", ""))) or \
            not isinstance(deployment.get("board_id"), str) or \
            not 0 < len(deployment["board_id"]) <= 96 or \
            not _UUID.fullmatch(str(deployment.get("device_uuid", ""))) or \
            not _BACKEND.fullmatch(str(deployment.get("backend", ""))) or \
            type(deployment.get("attempts")) is not int or \
            not 1 <= deployment["attempts"] <= 10000 or \
            deployment.get("verified") is not True:
        raise FirmwareDeploymentError("部署记录deployment值无效")
    identity = value.get("observed_identity")
    hash_fields = {"project_sha256", "config_sha256",
                   "firmware_identity_sha256"}
    if not isinstance(identity, dict) or set(identity) != hash_fields or \
            any(not _HASH.fullmatch(str(identity.get(field, "")))
                for field in hash_fields):
        raise FirmwareDeploymentError("部署记录observed_identity无效")
    evidence = value.get("source_evidence")
    if not isinstance(evidence, dict) or set(evidence) != {
            "build_record", "flashed_artifact"}:
        raise FirmwareDeploymentError("部署记录source_evidence无效")
    artifact_filename = ("firmware.bin" if deployment["backend"] in {
                         "can-katapult", "usb-katapult"} else "firmware.elf")
    for key, filename in (("build_record", "build-record.json"),
                          ("flashed_artifact", artifact_filename)):
        item = evidence.get(key)
        if not isinstance(item, dict) or set(item) != {
                "filename", "size", "sha256"} or \
                item.get("filename") != filename or \
                type(item.get("size")) is not int or item["size"] <= 0 or \
                not _HASH.fullmatch(str(item.get("sha256", ""))):
            raise FirmwareDeploymentError(f"部署记录{key}证据无效")
    recorded = value.get("recorded_time")
    if not isinstance(recorded, dict) or set(recorded) != {
            "value_utc", "source", "trusted", "note"} or \
            recorded.get("source") != "host_system_clock" or \
            recorded.get("trusted") is not False or \
            not isinstance(recorded.get("note"), str) or \
            not 0 < len(recorded["note"]) <= 256:
        raise FirmwareDeploymentError("部署记录时间来源声明无效")
    try:
        timestamp = datetime.fromisoformat(
            str(recorded.get("value_utc", "")).replace("Z", "+00:00"))
    except ValueError as error:
        raise FirmwareDeploymentError("部署记录UTC时间无效") from error
    if timestamp.tzinfo is None or timestamp.utcoffset().total_seconds() != 0:
        raise FirmwareDeploymentError("部署记录时间必须是UTC")
    if not isinstance(value.get("declaration"), str) or \
            not 0 < len(value["declaration"]) <= 512:
        raise FirmwareDeploymentError("部署记录声明无效")
    claimed = value.get("record_sha256")
    if not isinstance(claimed, str) or not _HASH.fullmatch(claimed):
        raise FirmwareDeploymentError("部署记录自哈希格式无效")
    if claimed != _record_hash(value):
        raise FirmwareDeploymentError("部署记录自哈希不匹配，内容可能已被篡改")
    return value


def load_deployment_record(path: Path) -> dict:
    if path.is_symlink() or not path.is_file():
        raise FirmwareDeploymentError("部署记录必须是普通文件")
    with path.open("rb") as stream:
        content = stream.read(MAX_DEPLOYMENT_RECORD_BYTES + 1)
    if len(content) > MAX_DEPLOYMENT_RECORD_BYTES:
        raise FirmwareDeploymentError("部署记录超过32 KiB上限")
    try:
        value = json.loads(content.decode("utf-8"),
                           object_pairs_hook=_strict_object,
                           parse_constant=lambda item: (_ for _ in ()).throw(
                               FirmwareDeploymentError(
                                   f"部署记录包含非标准数值：{item}")))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FirmwareDeploymentError(f"部署记录不是合法UTF-8 JSON：{error}") from error
    return validate_deployment_record(value)

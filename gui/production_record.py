#!/usr/bin/env python3
"""生成确定、版本化且不冒充烧录或实测的 Studio 生产记录。"""

from __future__ import annotations

import base64
import hashlib
import json
import math
import re
from dataclasses import dataclass

from project_artifacts import generate_project_reports
from project_config import ProjectConfigError


PRODUCTION_RECORD_SCHEMA_VERSION = 1
MAX_BUILD_RECORD_BYTES = 128 * 1024
MAX_BUILD_ARTIFACTS = 32
_HASH_PATTERN = re.compile(r"^[0-9a-f]{64}$")
_REQUIRED_ARTIFACT_NAMES = (
    "studio-project.json", "firmware.config", "build.log",
    "firmware.elf", "firmware.bin", "firmware.hex", "firmware.map",
)


@dataclass(frozen=True)
class ProductionRecordResult:
    record: dict
    content: bytes
    sha256: str
    filename: str


def _json_bytes(value: object) -> bytes:
    try:
        return (json.dumps(value, ensure_ascii=False, allow_nan=False,
                           sort_keys=True, indent=2) + "\n").encode("utf-8")
    except (TypeError, ValueError) as error:
        raise ProjectConfigError(f"生产记录输入不是有效JSON：{error}") from error


def _build_value(record: dict, path: str) -> object:
    value: object = record
    for part in path.split("."):
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    return value


def _text(value: object, maximum: int = 256) -> bool:
    return isinstance(value, str) and 0 < len(value) <= maximum


def _uint(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and \
        0 <= value <= 0x7FFF_FFFF_FFFF_FFFF


def _percent(value: object) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) \
        and math.isfinite(value) and 0 <= value <= 100


_REQUIRED_BUILD_FIELDS = (
    "schema_version", "build_id", "built_at_utc", "board_id",
    "firmware_target", "resource_count", "project_sha256",
    "config_sha256", "git_revision", "git_dirty", "parallel_jobs",
    "tool_versions.cmake", "tool_versions.ninja",
    "tool_versions.arm_none_eabi_gcc", "memory.ram.used_bytes",
    "memory.ram.capacity_bytes", "memory.ram.used_percent",
    "memory.flash.used_bytes", "memory.flash.capacity_bytes",
    "memory.flash.used_percent", "artifacts",
)


def _missing_build_fields(record: dict) -> list[dict]:
    missing: list[dict] = []
    for path in _REQUIRED_BUILD_FIELDS:
        value = _build_value(record, path)
        if value is None or value == "" or value == "unknown" or \
                path == "artifacts" and value == []:
            missing.append({
                "field": f"build_record.{path}",
                "reason": "字段缺失" if value is None or value == ""
                else "构建工具未能识别该值",
            })
    return missing


def _normalize_artifacts(value: object, missing: list[dict]) -> list[dict]:
    if not isinstance(value, list):
        return []
    if len(value) > MAX_BUILD_ARTIFACTS:
        raise ProjectConfigError(
            f"构建记录产物超过{MAX_BUILD_ARTIFACTS}项上限")
    artifacts: list[dict] = []
    filenames: set[str] = set()
    for index, item in enumerate(value):
        prefix = f"build_record.artifacts[{index}]"
        if not isinstance(item, dict):
            missing.append({"field": prefix, "reason": "产物条目不是对象"})
            continue
        filename = item.get("filename")
        size = item.get("size")
        sha256 = item.get("sha256")
        valid_name = isinstance(filename, str) and 0 < len(filename) <= 128 \
            and "/" not in filename and "\\" not in filename \
            and filename not in (".", "..")
        valid_size = _uint(size)
        valid_hash = isinstance(sha256, str) and bool(
            _HASH_PATTERN.fullmatch(sha256))
        for field, valid in (("filename", valid_name), ("size", valid_size),
                             ("sha256", valid_hash)):
            if not valid:
                missing.append({
                    "field": f"{prefix}.{field}",
                    "reason": "字段缺失或格式无效",
                })
        if valid_name and filename in filenames:
            missing.append({
                "field": f"{prefix}.filename",
                "reason": "产物名称重复",
            })
        elif valid_name:
            filenames.add(filename)
        artifacts.append({
            "filename": filename if valid_name else None,
            "size": size if valid_size else None,
            "sha256": sha256 if valid_hash else None,
        })
    return artifacts


def _build_evidence(build_record: object | None,
                    source_sha256: str | None) -> tuple[dict, list[dict], list[str]]:
    if build_record is None:
        missing = [{"field": f"build_record.{path}",
                    "reason": "尚未提供软件构建记录"}
                   for path in _REQUIRED_BUILD_FIELDS]
        return ({
            "status": "not_built",
            "source_record_sha256": None,
            "build_id": None,
            "built_at_utc": None,
            "firmware_target": None,
            "config_sha256": None,
            "git": {"revision": None, "dirty": None},
            "parallel_jobs": None,
            "tool_versions": {},
            "memory": {},
            "artifacts": [],
        }, missing, ["没有关联软件构建记录；本文件只是设计阶段记录。"])
    if not isinstance(build_record, dict):
        raise ProjectConfigError("构建记录必须是JSON对象")
    encoded = _json_bytes(build_record)
    if len(encoded) > MAX_BUILD_RECORD_BYTES:
        raise ProjectConfigError(
            f"构建记录超过{MAX_BUILD_RECORD_BYTES // 1024} KiB上限")
    version = build_record.get("schema_version")
    if isinstance(version, int) and not isinstance(version, bool) and \
            version > 1:
        raise ProjectConfigError(
            f"构建记录schema_version={version}高于当前支持的1")
    missing = _missing_build_fields(build_record)
    artifacts = _normalize_artifacts(build_record.get("artifacts"), missing)
    artifact_names = {item["filename"] for item in artifacts
                      if item["filename"]}
    for filename in _REQUIRED_ARTIFACT_NAMES:
        if filename not in artifact_names:
            missing.append({
                "field": f"build_record.artifacts[{filename}]",
                "reason": "构建记录未列出该标准产物",
            })
    invalid_fields = (
        ("schema_version", isinstance(build_record.get("schema_version"), int)
         and not isinstance(build_record.get("schema_version"), bool)
         and build_record.get("schema_version") == 1),
        ("project_sha256", isinstance(build_record.get("project_sha256"), str)
         and bool(_HASH_PATTERN.fullmatch(build_record["project_sha256"]))),
        ("config_sha256", isinstance(build_record.get("config_sha256"), str)
         and bool(_HASH_PATTERN.fullmatch(build_record["config_sha256"]))),
        ("git_dirty", isinstance(build_record.get("git_dirty"), bool)),
        ("build_id", _text(build_record.get("build_id"), 96)
         and bool(re.fullmatch(r"[a-z0-9-]+", build_record["build_id"]))),
        ("built_at_utc", _text(build_record.get("built_at_utc"), 64)),
        ("board_id", _text(build_record.get("board_id"), 96)),
        ("firmware_target", _text(build_record.get("firmware_target"), 96)),
        ("git_revision", _text(build_record.get("git_revision"), 64)
         and bool(re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}",
                               build_record["git_revision"]))),
        ("resource_count", _uint(build_record.get("resource_count"))),
        ("parallel_jobs", isinstance(build_record.get("parallel_jobs"), int)
         and not isinstance(build_record.get("parallel_jobs"), bool)
         and 1 <= build_record.get("parallel_jobs") <= 64),
        ("tool_versions.cmake",
         _text(_build_value(build_record, "tool_versions.cmake"))),
        ("tool_versions.ninja",
         _text(_build_value(build_record, "tool_versions.ninja"))),
        ("tool_versions.arm_none_eabi_gcc",
         _text(_build_value(
             build_record, "tool_versions.arm_none_eabi_gcc"))),
        ("memory.ram.used_bytes",
         _uint(_build_value(build_record, "memory.ram.used_bytes"))),
        ("memory.ram.capacity_bytes",
         _uint(_build_value(build_record, "memory.ram.capacity_bytes"))),
        ("memory.ram.used_percent",
         _percent(_build_value(build_record, "memory.ram.used_percent"))),
        ("memory.flash.used_bytes",
         _uint(_build_value(build_record, "memory.flash.used_bytes"))),
        ("memory.flash.capacity_bytes",
         _uint(_build_value(build_record, "memory.flash.capacity_bytes"))),
        ("memory.flash.used_percent",
         _percent(_build_value(build_record, "memory.flash.used_percent"))),
    )
    known_issues = {item["field"] for item in missing}
    for field, valid in invalid_fields:
        qualified = f"build_record.{field}"
        if not valid and qualified not in known_issues:
            missing.append({
                "field": qualified,
                "reason": "字段格式或范围无效",
            })
            known_issues.add(qualified)
    actual_source_sha256 = source_sha256 or hashlib.sha256(encoded).hexdigest()
    if not _HASH_PATTERN.fullmatch(actual_source_sha256):
        raise ProjectConfigError("构建记录源文件SHA-256格式无效")
    warnings: list[str] = []
    if build_record.get("git_dirty") is True:
        warnings.append("构建时 Git 工作区包含未提交改动，提交号不能单独复现全部源码。")
    if missing:
        warnings.append(f"软件构建记录有 {len(missing)} 个缺失或无效字段。")
    return ({
        "status": "incomplete" if missing else "recorded",
        "source_record_sha256": actual_source_sha256,
        "schema_version": build_record.get("schema_version"),
        "build_id": build_record.get("build_id"),
        "built_at_utc": build_record.get("built_at_utc"),
        "board_id": build_record.get("board_id"),
        "firmware_target": build_record.get("firmware_target"),
        "resource_count": build_record.get("resource_count"),
        "project_sha256": build_record.get("project_sha256"),
        "config_sha256": build_record.get("config_sha256"),
        "git": {
            "revision": build_record.get("git_revision"),
            "dirty": build_record.get("git_dirty"),
        },
        "parallel_jobs": build_record.get("parallel_jobs"),
        "tool_versions": build_record.get("tool_versions")
            if isinstance(build_record.get("tool_versions"), dict) else {},
        "memory": build_record.get("memory")
            if isinstance(build_record.get("memory"), dict) else {},
        "artifacts": artifacts,
    }, missing, warnings)


def generate_production_record(
        project: dict, catalog: dict, *, build_record: object | None = None,
        build_record_sha256: str | None = None) -> ProductionRecordResult:
    """生成只含软件证据的生产记录；不会构建、烧录或访问节点。"""
    reports = generate_project_reports(project, catalog)
    build, missing, warnings = _build_evidence(
        build_record, build_record_sha256)
    project_match = None if build_record is None or \
        build.get("project_sha256") is None else \
        build.get("project_sha256") == reports.project_sha256
    board_match = None if build_record is None or \
        build.get("board_id") is None else \
        build.get("board_id") == reports.board_id
    resource_count_match = None if build_record is None or \
        build.get("resource_count") is None else \
        build.get("resource_count") == reports.resource_count
    artifact_by_name = {
        item["filename"]: item for item in build.get("artifacts", [])
        if item.get("filename")
    }
    archived_project = artifact_by_name.get("studio-project.json")
    archived_config = artifact_by_name.get("firmware.config")
    archived_project_match = None if build_record is None or \
        archived_project is None or archived_project.get("sha256") is None \
        else archived_project["sha256"] == reports.project_sha256
    archived_config_match = None if build_record is None or \
        archived_config is None or archived_config.get("sha256") is None or \
        build.get("config_sha256") is None else \
        archived_config["sha256"] == build["config_sha256"]
    if project_match is False:
        warnings.append("软件构建记录对应的工程哈希与当前工程不一致。")
    if board_match is False:
        warnings.append("软件构建记录对应的板卡与当前工程不一致。")
    if resource_count_match is False:
        warnings.append("软件构建记录的资源数量与当前工程不一致。")
    if archived_project_match is False:
        warnings.append("归档的 Studio 工程产物哈希与当前规范工程不一致。")
    if archived_config_match is False:
        warnings.append("归档的固件配置产物哈希与构建记录不一致。")

    if build_record is None:
        status = "design_only"
        status_text = "仅设计记录：尚未执行软件构建、烧录或硬件实测"
    elif False in (project_match, board_match, resource_count_match,
                   archived_project_match, archived_config_match):
        status = "build_mismatch"
        status_text = "证据不匹配：构建记录不属于当前工程或板卡"
    elif missing:
        status = "build_incomplete"
        status_text = "构建证据不完整：缺失字段已逐项列出"
    else:
        status = "software_build_recorded"
        status_text = "已关联软件构建记录；尚未烧录，也未进行硬件实测"

    design_artifacts = [{
        "filename": artifact.filename,
        "sha256": artifact.sha256,
        "byte_count": len(artifact.content),
    } for artifact in reports.artifacts]
    record = {
        "schema_version": PRODUCTION_RECORD_SCHEMA_VERSION,
        "format": "RemoteBSP Studio生产记录",
        "status": status,
        "status_text": status_text,
        "design_evidence": {
            "board_id": reports.board_id,
            "project_schema_version": reports.project_schema_version,
            "project_original_schema_version":
                reports.original_schema_version,
            "project_migrations": list(reports.migrations),
            "project_sha256": reports.project_sha256,
            "resource_count": reports.resource_count,
            "resource_set_sha256": reports.resource_set_sha256,
            "project_reports_archive": {
                "filename": reports.archive_filename,
                "sha256": reports.archive_sha256,
                "byte_count": len(reports.archive),
                "artifacts": design_artifacts,
            },
        },
        "software_build_evidence": build,
        "evidence_checks": {
            "project_sha256_matches_build": project_match,
            "board_id_matches_build": board_match,
            "resource_count_matches_build": resource_count_match,
            "archived_project_matches_current": archived_project_match,
            "archived_config_matches_build": archived_config_match,
        },
        "execution_status": {
            "software_build": build["status"],
            "firmware_flash": "not_performed",
            "hardware_validation": "not_performed",
            "hardware_connected_by_this_operation": False,
        },
        "missing_or_invalid_fields": missing,
        "warnings": warnings,
        "declaration": (
            "本记录仅整理已有软件证据，不证明固件已经烧录，不证明板卡、"
            "接线、波形、时序或外设通过实测。"),
    }
    content = _json_bytes(record)
    return ProductionRecordResult(
        record=record, content=content,
        sha256=hashlib.sha256(content).hexdigest(),
        filename=f"{reports.board_id}-生产记录-v1.json")


def production_record_response(result: ProductionRecordResult) -> dict:
    """转换为本地 Studio API 的可下载响应。"""
    return {
        "ok": True,
        "format": "PRODUCTION_RECORD_V1",
        "status": result.record["status"],
        "status_text": result.record["status_text"],
        "project_sha256": result.record["design_evidence"]["project_sha256"],
        "resource_set_sha256":
            result.record["design_evidence"]["resource_set_sha256"],
        "reports_archive_sha256": result.record["design_evidence"]
            ["project_reports_archive"]["sha256"],
        "record_sha256": result.sha256,
        "filename": result.filename,
        "byte_count": len(result.content),
        "record_base64": base64.b64encode(result.content).decode("ascii"),
        "record": result.record,
    }

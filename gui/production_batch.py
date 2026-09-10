#!/usr/bin/env python3
"""Studio 确定性生产批次清单：纯软件、可移植且不冒充数据库。"""

from __future__ import annotations

import base64
import hashlib
import json
import re
from collections import Counter, defaultdict
from dataclasses import dataclass

from comparison_export import export_existing_project_comparison
from project_artifacts import ReportArtifact, deterministic_zip
from project_config import ProjectConfigError


PRODUCTION_BATCH_SCHEMA_VERSION = 1
MAX_BATCH_MANIFEST_BYTES = 128 * 1024
MAX_PRODUCTION_RECORD_BYTES = 64 * 1024
MAX_BATCH_RECORDS = 32
MAX_BATCH_COMPARISONS = 64
_HASH_RE = re.compile(r"^[0-9a-f]{64}$")
_BATCH_ID_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$")
_RECORD_STATUSES = {
    "design_only", "build_incomplete", "build_mismatch",
    "software_build_recorded", "firmware_deployed_verified",
}


@dataclass(frozen=True)
class ProductionBatchExport:
    manifest: dict
    validation: dict
    artifacts: tuple[ReportArtifact, ...]
    archive: bytes
    archive_filename: str
    archive_sha256: str


def _json_bytes(value: object, *, pretty: bool = False) -> bytes:
    try:
        options = {"indent": 2} if pretty else {"separators": (",", ":")}
        return (json.dumps(value, ensure_ascii=False, allow_nan=False,
                           sort_keys=True, **options) + "\n").encode("utf-8")
    except (TypeError, ValueError, RecursionError) as error:
        raise ProjectConfigError(f"生产批次输入不是有效JSON：{error}") from error


def _sha256(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _json_sha256(value: object) -> str:
    return _sha256(_json_bytes(value))


def _nested(value: object, path: str) -> object:
    current = value
    for part in path.split("."):
        if not isinstance(current, dict) or part not in current:
            return None
        current = current[part]
    return current


def _hash(value: object) -> bool:
    return isinstance(value, str) and bool(_HASH_RE.fullmatch(value))


def _text(value: object, maximum: int, *, optional: bool = False) -> bool:
    return value is None and optional or isinstance(value, str) and \
        0 < len(value) <= maximum


def _record_descriptor(value: object, index: int) -> dict:
    expected_sha256 = None
    if isinstance(value, dict) and value.get("format") == \
            "PRODUCTION_RECORD_V1" and isinstance(value.get("record"), dict):
        expected_sha256 = value.get("record_sha256")
        record = value["record"]
    elif isinstance(value, dict) and isinstance(value.get("record"), dict):
        expected_sha256 = value.get("record_sha256")
        record = value["record"]
    else:
        record = value
    if not isinstance(record, dict):
        raise ProjectConfigError(f"生产记录[{index}]必须是JSON对象")
    content = _json_bytes(record, pretty=True)
    if len(content) > MAX_PRODUCTION_RECORD_BYTES:
        raise ProjectConfigError(
            f"生产记录[{index}]超过{MAX_PRODUCTION_RECORD_BYTES // 1024} KiB上限")
    record_sha256 = _sha256(content)
    if expected_sha256 is not None and (not _hash(expected_sha256) or
                                        expected_sha256 != record_sha256):
        raise ProjectConfigError(
            f"生产记录[{index}]SHA-256不匹配，内容可能已被修改")
    if record.get("schema_version") != 1 or record.get("format") != \
            "RemoteBSP Studio生产记录":
        raise ProjectConfigError(f"生产记录[{index}]格式或版本不受支持")
    status = record.get("status")
    design = record.get("design_evidence")
    build = record.get("software_build_evidence")
    execution = record.get("execution_status")
    if status not in _RECORD_STATUSES or not isinstance(design, dict) or \
            not isinstance(build, dict) or not isinstance(execution, dict):
        raise ProjectConfigError(f"生产记录[{index}]缺少状态或软件证据")
    fields = {
        "board_id": design.get("board_id"),
        "project_sha256": design.get("project_sha256"),
        "resource_set_sha256": design.get("resource_set_sha256"),
        "reports_archive_sha256": _nested(
            design, "project_reports_archive.sha256"),
        "build_id": build.get("build_id"),
        "build_record_sha256": build.get("source_record_sha256"),
    }
    if not _text(fields["board_id"], 96) or any(
            not _hash(fields[name]) for name in (
                "project_sha256", "resource_set_sha256",
                "reports_archive_sha256")):
        raise ProjectConfigError(f"生产记录[{index}]设计证据引用缺失或格式无效")
    if not _text(fields["build_id"], 96, optional=True) or \
            not (fields["build_record_sha256"] is None or
                 _hash(fields["build_record_sha256"])):
        raise ProjectConfigError(f"生产记录[{index}]构建引用格式无效")
    if status == "design_only" and (fields["build_id"] is not None or
                                    fields["build_record_sha256"] is not None):
        raise ProjectConfigError(f"生产记录[{index}]仅设计状态却包含构建引用")
    if status != "design_only" and (fields["build_id"] is None or
                                    fields["build_record_sha256"] is None):
        raise ProjectConfigError(f"生产记录[{index}]缺少构建ID或构建记录引用")
    expected_flash = ("performed_and_verified" if status ==
                      "firmware_deployed_verified" else "not_performed")
    if execution.get("firmware_flash") != expected_flash or \
            execution.get("hardware_validation") != "not_performed" or \
            execution.get("hardware_connected_by_this_operation") is not False:
        raise ProjectConfigError(
            f"生产记录[{index}]执行状态与记录状态不一致")
    return {
        "record_schema_version": 1,
        "record_sha256": record_sha256,
        "status": status,
        **fields,
    }


def _comparison_descriptor(value: object, index: int) -> dict:
    expected_archive_sha256 = None
    if isinstance(value, dict) and isinstance(value.get("comparison"), dict):
        comparison = value["comparison"]
        expected_archive_sha256 = value.get("archive_sha256")
    else:
        comparison = value
    try:
        packaged = export_existing_project_comparison(comparison)
    except ProjectConfigError as error:
        raise ProjectConfigError(f"差异记录[{index}]无效：{error}") from error
    if expected_archive_sha256 is not None and (
            not _hash(expected_archive_sha256) or
            expected_archive_sha256 != packaged.archive_sha256):
        raise ProjectConfigError(
            f"差异记录[{index}]资料包SHA-256不匹配，内容可能已被修改")
    left = packaged.comparison["left"]
    right = packaged.comparison["right"]
    required = (left.get("project_sha256"), right.get("project_sha256"))
    if not all(_hash(item) for item in required) or not all(
            _text(item.get("board_id"), 96) for item in (left, right)):
        raise ProjectConfigError(f"差异记录[{index}]工程或板卡引用无效")
    return {
        "comparison_schema_version": packaged.comparison["schema_version"],
        "comparison_sha256": packaged.comparison["comparison_sha256"],
        "comparison_archive_sha256": packaged.archive_sha256,
        "left_project_sha256": left["project_sha256"],
        "right_project_sha256": right["project_sha256"],
        "left_board_id": left["board_id"],
        "right_board_id": right["board_id"],
        "truncated": packaged.comparison["summary"]["truncated"],
    }


def _trace_index(records: list[dict], comparisons: list[dict]) -> dict:
    projects: dict[str, dict[str, set[str]]] = defaultdict(
        lambda: {"records": set(), "comparisons": set()})
    builds: dict[str, set[str]] = defaultdict(set)
    boards: dict[str, set[str]] = defaultdict(set)
    record_rows: list[dict] = []
    for record in records:
        record_hash = record.get("record_sha256")
        project_hash = record.get("project_sha256")
        board_id = record.get("board_id")
        build_id = record.get("build_id")
        if _hash(record_hash) and _hash(project_hash):
            projects[project_hash]["records"].add(record_hash)
        if _hash(record_hash) and _text(board_id, 96):
            boards[board_id].add(record_hash)
        if _hash(record_hash) and _text(build_id, 96, optional=True) and build_id:
            builds[build_id].add(record_hash)
        if _hash(record_hash):
            record_rows.append({
                "key": record_hash, "project_sha256": project_hash,
                "build_id": build_id, "board_id": board_id,
            })
    for comparison in comparisons:
        comparison_hash = comparison.get("comparison_sha256")
        for side in ("left_project_sha256", "right_project_sha256"):
            project_hash = comparison.get(side)
            if _hash(project_hash) and _hash(comparison_hash):
                projects[project_hash]["comparisons"].add(comparison_hash)
    return {
        "by_project_sha256": [{
            "key": key,
            "record_sha256s": sorted(value["records"]),
            "comparison_sha256s": sorted(value["comparisons"]),
        } for key, value in sorted(projects.items())],
        "by_build_id": [{"key": key, "record_sha256s": sorted(value)}
                        for key, value in sorted(builds.items())],
        "by_board_id": [{"key": key, "record_sha256s": sorted(value)}
                        for key, value in sorted(boards.items())],
        "by_record_sha256": sorted(record_rows, key=lambda item: item["key"]),
    }


def _issue(code: str, path: str, message: str) -> dict:
    return {"code": code, "path": path, "message": message}


def _descriptor_issues(records: list, comparisons: list) -> list[dict]:
    issues: list[dict] = []
    record_fields = (
        ("record_schema_version", lambda value: value == 1),
        ("record_sha256", _hash),
        ("status", lambda value: value in _RECORD_STATUSES),
        ("board_id", lambda value: _text(value, 96)),
        ("project_sha256", _hash),
        ("resource_set_sha256", _hash),
        ("reports_archive_sha256", _hash),
        ("build_id", lambda value: _text(value, 96, optional=True)),
        ("build_record_sha256", lambda value: value is None or _hash(value)),
    )
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            issues.append(_issue("invalid_entry", f"records[{index}]",
                                 "生产记录索引项不是对象"))
            continue
        if set(record) != {field for field, _ in record_fields}:
            issues.append(_issue(
                "schema_mismatch", f"records[{index}]",
                "生产记录索引字段集合与v1 schema不一致"))
        for field, predicate in record_fields:
            if not predicate(record.get(field)):
                issues.append(_issue(
                    "invalid_reference", f"records[{index}].{field}",
                    "字段缺失或格式无效"))
        if record.get("status") != "design_only" and (
                not record.get("build_id") or
                not record.get("build_record_sha256")):
            issues.append(_issue(
                "missing_reference", f"records[{index}]",
                "非设计阶段记录缺少构建ID或构建记录哈希"))
        if record.get("status") == "design_only" and (
                record.get("build_id") is not None or
                record.get("build_record_sha256") is not None):
            issues.append(_issue(
                "reference_mismatch", f"records[{index}]",
                "仅设计记录不应包含构建引用"))
    comparison_fields = (
        ("comparison_schema_version", lambda value: value == 1),
        ("comparison_sha256", _hash),
        ("comparison_archive_sha256", _hash),
        ("left_project_sha256", _hash),
        ("right_project_sha256", _hash),
        ("left_board_id", lambda value: _text(value, 96)),
        ("right_board_id", lambda value: _text(value, 96)),
        ("truncated", lambda value: isinstance(value, bool)),
    )
    for index, comparison in enumerate(comparisons):
        if not isinstance(comparison, dict):
            issues.append(_issue("invalid_entry", f"comparisons[{index}]",
                                 "差异资料索引项不是对象"))
            continue
        if set(comparison) != {field for field, _ in comparison_fields}:
            issues.append(_issue(
                "schema_mismatch", f"comparisons[{index}]",
                "差异资料索引字段集合与v1 schema不一致"))
        for field, predicate in comparison_fields:
            if not predicate(comparison.get(field)):
                issues.append(_issue(
                    "invalid_reference", f"comparisons[{index}].{field}",
                    "字段缺失或格式无效"))
    return issues


def _duplicates(values: list[object]) -> list[str]:
    return sorted(str(value) for value, count in Counter(values).items()
                  if value is not None and count > 1)


def validate_production_batch_manifest(manifest: object) -> dict:
    """验证批次自哈希、引用、重复项和稳定索引，不访问外部文件。"""
    if not isinstance(manifest, dict):
        raise ProjectConfigError("生产批次清单必须是JSON对象")
    if len(_json_bytes(manifest)) > MAX_BATCH_MANIFEST_BYTES:
        raise ProjectConfigError(
            f"生产批次清单超过{MAX_BATCH_MANIFEST_BYTES // 1024} KiB上限")
    issues: list[dict] = []
    expected_top_fields = {
        "schema_version", "format", "batch", "records", "comparisons",
        "trace_index", "integrity", "limits", "declaration",
        "manifest_sha256",
    }
    if set(manifest) != expected_top_fields:
        issues.append(_issue("schema_mismatch", "$",
                             "清单字段集合与v1 schema不一致"))
    if manifest.get("format") != "REMOTEBSP_PRODUCTION_BATCH_V1":
        issues.append(_issue("unsupported_format", "format",
                             "不是受支持的生产批次清单"))
    if manifest.get("schema_version") != PRODUCTION_BATCH_SCHEMA_VERSION:
        issues.append(_issue("unsupported_schema", "schema_version",
                             "生产批次schema版本不受支持"))
    batch = manifest.get("batch")
    if not isinstance(batch, dict) or not _BATCH_ID_RE.fullmatch(
            str(batch.get("batch_id", ""))):
        issues.append(_issue("invalid_batch", "batch.batch_id",
                             "批次ID格式无效"))
    if not isinstance(batch, dict) or not _text(batch.get("name"), 128):
        issues.append(_issue("invalid_batch", "batch.name",
                             "批次名称格式无效"))
    if not isinstance(batch, dict) or not isinstance(batch.get("note"), str) \
            or len(batch.get("note", "")) > 512:
        issues.append(_issue("invalid_batch", "batch.note",
                             "批次说明格式无效"))
    if isinstance(batch, dict) and set(batch) != {"batch_id", "name", "note"}:
        issues.append(_issue("schema_mismatch", "batch",
                             "批次信息字段集合与v1 schema不一致"))
    records = manifest.get("records")
    comparisons = manifest.get("comparisons")
    if not isinstance(records, list):
        issues.append(_issue("invalid_entries", "records", "生产记录索引不是数组"))
        records = []
    if not isinstance(comparisons, list):
        issues.append(_issue("invalid_entries", "comparisons", "差异资料索引不是数组"))
        comparisons = []
    if not records:
        issues.append(_issue("missing_reference", "records", "批次至少需要一份生产记录"))
    if len(records) > MAX_BATCH_RECORDS:
        issues.append(_issue("capacity_exceeded", "records",
                             f"生产记录超过{MAX_BATCH_RECORDS}项上限"))
    if len(comparisons) > MAX_BATCH_COMPARISONS:
        issues.append(_issue("capacity_exceeded", "comparisons",
                             f"差异资料超过{MAX_BATCH_COMPARISONS}项上限"))
    issues.extend(_descriptor_issues(records, comparisons))
    for field, values in (
            ("record_sha256", [item.get("record_sha256") for item in records
                               if isinstance(item, dict)]),
            ("build_id", [item.get("build_id") for item in records
                          if isinstance(item, dict) and item.get("build_id")]),
            ("comparison_sha256", [item.get("comparison_sha256")
                                   for item in comparisons
                                   if isinstance(item, dict)]),
            ("comparison_archive_sha256", [
                item.get("comparison_archive_sha256") for item in comparisons
                if isinstance(item, dict)])):
        for duplicate in _duplicates(values):
            issues.append(_issue(
                "duplicate", field,
                f"发现重复引用：{duplicate}"))
    project_boards: dict[str, set[str]] = defaultdict(set)
    for item in records:
        if isinstance(item, dict) and _hash(item.get("project_sha256")) and \
                _text(item.get("board_id"), 96):
            project_boards[item["project_sha256"]].add(item["board_id"])
    for project, boards in sorted(project_boards.items()):
        if len(boards) > 1:
            issues.append(_issue(
                "reference_mismatch", "records.project_sha256",
                f"同一工程哈希关联了多个板卡：{project}"))
    projects = {project: next(iter(boards))
                for project, boards in project_boards.items()
                if len(boards) == 1}
    for index, comparison in enumerate(comparisons):
        if not isinstance(comparison, dict):
            continue
        for side in ("left", "right"):
            project = comparison.get(f"{side}_project_sha256")
            board = comparison.get(f"{side}_board_id")
            if _hash(project) and project not in projects:
                issues.append(_issue(
                    "missing_reference",
                    f"comparisons[{index}].{side}_project_sha256",
                    "差异资料引用的工程在本批次生产记录中不存在"))
            elif project in projects and projects[project] != board:
                issues.append(_issue(
                    "reference_mismatch",
                    f"comparisons[{index}].{side}_board_id",
                    "差异资料的板卡与生产记录不一致"))
    sorted_records = sorted(
        records, key=lambda item: item.get("record_sha256", "")
        if isinstance(item, dict) else "")
    sorted_comparisons = sorted(
        comparisons, key=lambda item: (
            item.get("comparison_sha256", ""),
            item.get("comparison_archive_sha256", ""))
        if isinstance(item, dict) else ("", ""))
    if records != sorted_records:
        issues.append(_issue("unstable_order", "records",
                             "生产记录未按记录哈希稳定排序"))
    if comparisons != sorted_comparisons:
        issues.append(_issue("unstable_order", "comparisons",
                             "差异资料未按比较哈希稳定排序"))
    integrity = manifest.get("integrity")
    if not isinstance(integrity, dict):
        integrity = {}
        issues.append(_issue("missing_integrity", "integrity", "缺少分组校验值"))
    for field, actual in (
            ("records_sha256", _json_sha256(records)),
            ("comparisons_sha256", _json_sha256(comparisons))):
        if integrity.get(field) != actual:
            issues.append(_issue("hash_mismatch", f"integrity.{field}",
                                 "分组SHA-256不匹配，内容可能已被修改"))
    expected_trace = _trace_index(records, comparisons)
    if manifest.get("trace_index") != expected_trace:
        issues.append(_issue("index_mismatch", "trace_index",
                             "追溯索引与记录内容不一致"))
    hash_input = dict(manifest)
    claimed_manifest_sha256 = hash_input.pop("manifest_sha256", None)
    actual_manifest_sha256 = _json_sha256(hash_input)
    if claimed_manifest_sha256 != actual_manifest_sha256:
        issues.append(_issue("hash_mismatch", "manifest_sha256",
                             "清单SHA-256不匹配，内容可能已被修改"))
    expected_limits = {
        "max_manifest_bytes": MAX_BATCH_MANIFEST_BYTES,
        "max_production_record_bytes": MAX_PRODUCTION_RECORD_BYTES,
        "max_records": MAX_BATCH_RECORDS,
        "max_comparisons": MAX_BATCH_COMPARISONS,
    }
    if manifest.get("limits") != expected_limits:
        issues.append(_issue("limit_mismatch", "limits",
                             "清单声明的容量边界与当前schema不一致"))
    issues.sort(key=lambda item: (item["path"], item["code"], item["message"]))
    valid = not issues
    return {
        "ok": True,
        "format": "PRODUCTION_BATCH_VALIDATION_V1",
        "valid": valid,
        "status": "valid" if valid else "invalid",
        "status_text": "批次清单完整且引用一致" if valid else
                       f"批次清单发现 {len(issues)} 个问题",
        "manifest_sha256": claimed_manifest_sha256,
        "actual_manifest_sha256": actual_manifest_sha256,
        "summary": {
            "record_count": len(records),
            "comparison_count": len(comparisons),
            "project_count": len(expected_trace["by_project_sha256"]),
            "build_count": len(expected_trace["by_build_id"]),
            "board_count": len(expected_trace["by_board_id"]),
            "issue_count": len(issues),
        },
        "trace_index": expected_trace,
        "issues": issues,
        "limits": {
            "max_manifest_bytes": MAX_BATCH_MANIFEST_BYTES,
            "max_production_record_bytes": MAX_PRODUCTION_RECORD_BYTES,
            "max_records": MAX_BATCH_RECORDS,
            "max_comparisons": MAX_BATCH_COMPARISONS,
        },
        "declaration": (
            "该校验只核对批次清单及其软件证据引用，不执行构建、烧录、"
            "硬件连接或实测；SHA-256不是数字签名。"),
    }


def _markdown(manifest: dict, validation: dict) -> bytes:
    batch = manifest["batch"]
    summary = validation["summary"]
    lines = [
        "# RemoteBSP Studio 生产批次摘要",
        "",
        f"- 批次 ID：`{batch['batch_id']}`",
        f"- 批次名称：{_markdown_text(batch['name'])}",
        f"- 批次说明：{_markdown_text(batch['note']) if batch['note'] else '无'}",
        f"- 清单 SHA-256：`{manifest['manifest_sha256']}`",
        f"- 校验结果：{validation['status_text']}",
        "",
        "## 可追溯内容",
        "",
        f"- 生产记录：{summary['record_count']} 份",
        f"- 差异资料：{summary['comparison_count']} 份",
        f"- 工程哈希：{summary['project_count']} 个",
        f"- 构建 ID：{summary['build_count']} 个",
        f"- 板卡：{summary['board_count']} 种",
        "",
        "## 生产记录",
        "",
    ]
    for record in manifest["records"]:
        build = _markdown_text(record["build_id"]) if record["build_id"] \
            else "未关联构建"
        lines.append(
            f"- {_markdown_text(record['board_id'])} · "
            f"{_markdown_text(record['status'])} · 构建 {build} · "
            f"工程 `{record['project_sha256']}` · 记录 `{record['record_sha256']}`")
    lines.extend(["", "## 工程差异资料", ""])
    if manifest["comparisons"]:
        for comparison in manifest["comparisons"]:
            suffix = "（详情有截断）" if comparison["truncated"] else ""
            lines.append(
                f"- `{comparison['left_project_sha256']}` → "
                f"`{comparison['right_project_sha256']}` · 比较 "
                f"`{comparison['comparison_sha256']}`{suffix}")
    else:
        lines.append("- 本批次未关联工程差异资料。")
    lines.extend([
        "", "## 完整性与边界", "",
        f"- 生产记录分组 SHA-256：`{manifest['integrity']['records_sha256']}`",
        f"- 差异资料分组 SHA-256：`{manifest['integrity']['comparisons_sha256']}`",
        f"- 容量上限：{MAX_BATCH_RECORDS} 份生产记录、"
        f"{MAX_BATCH_COMPARISONS} 份差异资料、"
        f"{MAX_BATCH_MANIFEST_BYTES // 1024} KiB 清单。",
        "- 清单按工程哈希、构建 ID、板卡和生产记录哈希提供稳定索引。",
        "- SHA-256用于内容一致性核对，不是签名或可信时间戳。",
        "- 本批次只整理软件证据；未执行构建、烧录、硬件连接或实测。",
        "",
    ])
    return "\n".join(lines).encode("utf-8")


def _markdown_text(value: object) -> str:
    text = str(value).replace("\r", " ").replace("\n", " ")
    # 批次证据来自外部 JSON。先转义反斜杠，再转义 Markdown 控制字符，
    # 避免名称或说明伪造标题、列表、代码和表格单元格。
    text = text.replace("\\", "\\\\")
    for character in ("`", "*", "_", "[", "]", "<", ">", "|",
                      "#", "+", "-", "!"):
        text = text.replace(character, "\\" + character)
    return text


def export_production_batch(*, batch_id: object, name: object, note: object,
                            production_records: object,
                            comparison_exports: object) -> ProductionBatchExport:
    """从现有软件证据生成最小可信批次清单和稳定资料包。"""
    if not isinstance(batch_id, str) or not _BATCH_ID_RE.fullmatch(batch_id):
        raise ProjectConfigError("批次ID须为1至64位小写字母、数字或连字符")
    if not _text(name, 128):
        raise ProjectConfigError("批次名称须为1至128个字符")
    if not isinstance(note, str) or len(note) > 512:
        raise ProjectConfigError("批次说明不能超过512个字符")
    if not isinstance(production_records, list) or not production_records:
        raise ProjectConfigError("至少需要一份生产记录")
    if not isinstance(comparison_exports, list):
        raise ProjectConfigError("差异资料必须是数组")
    if len(production_records) > MAX_BATCH_RECORDS:
        raise ProjectConfigError(f"生产记录超过{MAX_BATCH_RECORDS}项上限")
    if len(comparison_exports) > MAX_BATCH_COMPARISONS:
        raise ProjectConfigError(f"差异资料超过{MAX_BATCH_COMPARISONS}项上限")
    records = sorted(
        (_record_descriptor(item, index)
         for index, item in enumerate(production_records)),
        key=lambda item: item["record_sha256"])
    comparisons = sorted(
        (_comparison_descriptor(item, index)
         for index, item in enumerate(comparison_exports)),
        key=lambda item: (item["comparison_sha256"],
                          item["comparison_archive_sha256"]))
    manifest = {
        "schema_version": PRODUCTION_BATCH_SCHEMA_VERSION,
        "format": "REMOTEBSP_PRODUCTION_BATCH_V1",
        "batch": {"batch_id": batch_id, "name": name, "note": note},
        "records": records,
        "comparisons": comparisons,
        "trace_index": _trace_index(records, comparisons),
        "integrity": {
            "records_sha256": _json_sha256(records),
            "comparisons_sha256": _json_sha256(comparisons),
        },
        "limits": {
            "max_manifest_bytes": MAX_BATCH_MANIFEST_BYTES,
            "max_production_record_bytes": MAX_PRODUCTION_RECORD_BYTES,
            "max_records": MAX_BATCH_RECORDS,
            "max_comparisons": MAX_BATCH_COMPARISONS,
        },
        "declaration": (
            "这是可移植的软件生产批次索引，不是服务器数据库；不证明已构建、"
            "烧录、连接板卡或完成硬件实测。"),
    }
    manifest["manifest_sha256"] = _json_sha256(manifest)
    if len(_json_bytes(manifest)) > MAX_BATCH_MANIFEST_BYTES:
        raise ProjectConfigError(
            f"生成的生产批次清单超过{MAX_BATCH_MANIFEST_BYTES // 1024} KiB上限")
    validation = validate_production_batch_manifest(manifest)
    if not validation["valid"]:
        raise ProjectConfigError(validation["status_text"])
    json_content = _json_bytes(manifest, pretty=True)
    markdown_content = _markdown(manifest, validation)
    artifacts = (
        ReportArtifact(f"{batch_id}-生产批次清单-v1.json", "application/json",
                       json_content, _sha256(json_content)),
        ReportArtifact(f"{batch_id}-生产批次摘要-v1.md",
                       "text/markdown; charset=utf-8", markdown_content,
                       _sha256(markdown_content)),
    )
    checksums = "".join(f"{item.sha256}  {item.filename}\n"
                        for item in artifacts).encode("utf-8")
    artifacts += (ReportArtifact(
        "SHA256SUMS", "text/plain; charset=utf-8", checksums,
        _sha256(checksums)),)
    archive = deterministic_zip(artifacts)
    return ProductionBatchExport(
        manifest=manifest, validation=validation, artifacts=artifacts,
        archive=archive,
        archive_filename=f"{batch_id}-生产批次资料-v1.zip",
        archive_sha256=_sha256(archive))


def production_batch_response(result: ProductionBatchExport) -> dict:
    return {
        "ok": True,
        "format": "PRODUCTION_BATCH_EXPORT_V1",
        "schema_version": PRODUCTION_BATCH_SCHEMA_VERSION,
        "manifest_sha256": result.manifest["manifest_sha256"],
        "manifest": result.manifest,
        "validation": result.validation,
        "archive_filename": result.archive_filename,
        "archive_sha256": result.archive_sha256,
        "archive_byte_count": len(result.archive),
        "archive_base64": base64.b64encode(result.archive).decode("ascii"),
        "artifacts": [{
            "filename": item.filename, "content_type": item.content_type,
            "byte_count": len(item.content), "sha256": item.sha256,
        } for item in result.artifacts],
    }

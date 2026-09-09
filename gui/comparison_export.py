#!/usr/bin/env python3
"""把同一份 Studio 工程比较结果导出为稳定 JSON 与中文报告。"""

from __future__ import annotations

import base64
import hashlib
import json
from dataclasses import dataclass

from project_artifacts import ReportArtifact, deterministic_zip
from project_compare import compare_projects
from project_config import ProjectConfigError


COMPARISON_EXPORT_SCHEMA_VERSION = 1


@dataclass(frozen=True)
class ComparisonExportResult:
    comparison: dict
    artifacts: tuple[ReportArtifact, ...]
    archive: bytes
    archive_filename: str
    archive_sha256: str


def _json_bytes(value: object) -> bytes:
    try:
        return (json.dumps(value, ensure_ascii=False, allow_nan=False,
                           sort_keys=True, indent=2) + "\n").encode("utf-8")
    except (TypeError, ValueError) as error:
        raise ProjectConfigError(f"差异导出内容不是有效JSON：{error}") from error


def _sha256(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _markdown_text(value: object) -> str:
    # summary/headline/migration 都是比较结果的一部分。这里只压平换行，保留其余
    # 字节，确保 Markdown 源文与 JSON/页面展示的中文摘要可直接逐项核对。
    return str(value).replace("\r", " ").replace("\n", " ")


def _migration_text(project: dict) -> str:
    migrations = project.get("migrations", [])
    return "；".join(_markdown_text(item) for item in migrations) \
        if migrations else "无迁移"


def _markdown(comparison: dict) -> bytes:
    left = comparison["left"]
    right = comparison["right"]
    summary = comparison["summary"]
    lines = [
        "# RemoteBSP Studio 工程差异报告",
        "",
        "> 本报告与同包 JSON 使用同一份 `PROJECT_COMPARISON_V1` 比较结果，"
        "未执行构建、烧录或板卡访问。",
        "",
        f"- 比较结果：{_markdown_text(summary['headline'])}",
        f"- 比较结果 SHA-256：`{comparison['comparison_sha256']}`",
        f"- 版本 A：{_markdown_text(left['board_label'])}",
        f"- 版本 A 工程 SHA-256：`{left['project_sha256']}`",
        f"- 版本 A schema：原始 v{left['original_schema_version']} → "
        f"规范 v{left['schema_version']}；{_migration_text(left)}",
        f"- 版本 B：{_markdown_text(right['board_label'])}",
        f"- 版本 B 工程 SHA-256：`{right['project_sha256']}`",
        f"- 版本 B schema：原始 v{right['original_schema_version']} → "
        f"规范 v{right['schema_version']}；{_migration_text(right)}",
        "",
        "## 变化概览",
        "",
        "| 新增 | 删除 | 修改 | 未变 | 工程级变化 | 字段变化 |",
        "|---:|---:|---:|---:|---:|---:|",
        f"| {summary['added']} | {summary['removed']} | "
        f"{summary['modified']} | {summary['unchanged']} | "
        f"{summary['project_level_changes']} | {summary['field_changes']} |",
        "",
    ]
    if summary["truncated"]:
        lines.extend([
            "## 截断说明",
            "",
            f"字段变化总数为 {summary['field_changes']}，其中 "
            f"{summary['omitted_field_changes']} 条因有界输出限制未展开。"
            "完整输入未被写入服务器文件。",
            "",
        ])
    else:
        lines.extend(["- 差异详情未截断。", ""])
    lines.extend(["## 逐项变化", ""])
    if comparison["changes"]:
        for change in comparison["changes"]:
            lines.append(f"- {_markdown_text(change['summary'])}")
    else:
        lines.append("- 两份工程的板卡与资源配置一致。")
    limits = comparison["limits"]
    lines.extend([
        "", "## 比较边界", "",
        f"- 每份工程最多 {limits['max_project_bytes'] // 1024} KiB。",
        f"- 每份工程最多 {limits['max_resource_entries']} 项资源。",
        f"- 最多展开 {limits['max_changed_fields']} 条字段变化。",
        "- 本报告只比较通过迁移、规范化和统一静态校验的工程。",
        "- 本报告不证明固件已经构建、烧录或完成硬件实测。",
        "",
    ])
    return "\n".join(lines).encode("utf-8")


def export_project_comparison(left: object, right: object, catalog: dict
                              ) -> ComparisonExportResult:
    """只比较一次，并从同一结果生成 JSON、Markdown 和校验文件。"""
    comparison = compare_projects(left, right, catalog)
    base = f"{comparison['left']['board_id']}-to-{comparison['right']['board_id']}"
    json_content = _json_bytes(comparison)
    markdown_content = _markdown(comparison)
    artifacts = (
        ReportArtifact(f"{base}-工程差异-v1.json", "application/json",
                       json_content, _sha256(json_content)),
        ReportArtifact(f"{base}-工程差异报告-v1.md",
                       "text/markdown; charset=utf-8", markdown_content,
                       _sha256(markdown_content)),
    )
    checksums = "".join(
        f"{artifact.sha256}  {artifact.filename}\n"
        for artifact in artifacts).encode("utf-8")
    artifacts += (ReportArtifact(
        "SHA256SUMS", "text/plain; charset=utf-8", checksums,
        _sha256(checksums)),)
    archive = deterministic_zip(artifacts)
    return ComparisonExportResult(
        comparison=comparison, artifacts=artifacts, archive=archive,
        archive_filename=f"{base}-工程差异资料-v1.zip",
        archive_sha256=_sha256(archive))


def comparison_export_response(result: ComparisonExportResult) -> dict:
    """转换为 Studio 本地 API 响应。"""
    return {
        "ok": True,
        "format": "PROJECT_COMPARISON_EXPORT_V1",
        "schema_version": COMPARISON_EXPORT_SCHEMA_VERSION,
        "comparison_sha256": result.comparison["comparison_sha256"],
        "comparison": result.comparison,
        "archive_filename": result.archive_filename,
        "archive_sha256": result.archive_sha256,
        "archive_byte_count": len(result.archive),
        "archive_base64": base64.b64encode(result.archive).decode("ascii"),
        "artifacts": [{
            "filename": artifact.filename,
            "content_type": artifact.content_type,
            "byte_count": len(artifact.content),
            "sha256": artifact.sha256,
        } for artifact in result.artifacts],
    }

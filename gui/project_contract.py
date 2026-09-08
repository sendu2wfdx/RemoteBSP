#!/usr/bin/env python3
"""RemoteBSP Studio 工程文档的版本、迁移和稳定身份。"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass


CURRENT_PROJECT_SCHEMA_VERSION = 1


class ProjectContractError(ValueError):
    """工程文档不满足版本契约。"""


@dataclass(frozen=True)
class PreparedProject:
    """已经迁移并规范化、可以交给配置生成器的工程。"""

    document: dict
    original_schema_version: int
    schema_version: int
    migrations: tuple[str, ...]
    sha256: str
    summary: dict


def _clone_json(value: object) -> object:
    """复制 JSON 值，同时拒绝 NaN、Infinity 和非 JSON 对象。"""
    try:
        encoded = json.dumps(value, ensure_ascii=False, allow_nan=False)
        return json.loads(encoded)
    except (TypeError, ValueError, json.JSONDecodeError) as error:
        raise ProjectContractError(f"工程不是有效JSON文档：{error}") from error


def canonical_project_bytes(project: dict) -> bytes:
    """返回与键顺序和排版无关的规范工程字节。"""
    try:
        text = json.dumps(
            project, ensure_ascii=False, allow_nan=False, sort_keys=True,
            indent=2,
        ) + "\n"
        return text.encode("utf-8")
    except (TypeError, ValueError) as error:
        raise ProjectContractError(f"工程不是有效JSON文档：{error}") from error


def _resource_summary(project: dict) -> dict:
    paths = (
        ("gpio", "resources"),
        ("uart", "ports"),
        ("motion", "axes"),
        ("pwm", "channels"),
        ("timed_bitstream", "ws2812"),
    )
    counts: dict[str, int] = {}
    for group, key in paths:
        value = project.get(group, {})
        items = value.get(key, []) if isinstance(value, dict) else []
        counts[f"{group}.{key}"] = len(items) if isinstance(items, list) else 0
    return {
        "board_id": project.get("board_id"),
        "resource_count": sum(counts.values()),
        "resource_counts": counts,
    }


def prepare_project(project: object) -> PreparedProject:
    """迁移工程并计算稳定哈希；具体资源合法性由生成器校验。"""
    if not isinstance(project, dict):
        raise ProjectContractError("工程必须是JSON对象")
    document = _clone_json(project)
    assert isinstance(document, dict)

    raw_version = document.get("schema_version", 0)
    if isinstance(raw_version, bool) or not isinstance(raw_version, int):
        raise ProjectContractError("工程schema_version必须是整数")
    if raw_version < 0:
        raise ProjectContractError("工程schema_version不能为负数")
    if raw_version > CURRENT_PROJECT_SCHEMA_VERSION:
        raise ProjectContractError(
            f"工程schema_version={raw_version}高于当前支持的"
            f"{CURRENT_PROJECT_SCHEMA_VERSION}，请升级RemoteBSP Studio")

    original_version = raw_version
    migrations: list[str] = []
    if raw_version == 0:
        # 最早期草案没有版本字段，资源层级与 v1 相同。
        document["schema_version"] = 1
        raw_version = 1
        migrations.append("v0->v1：补充工程schema_version")

    canonical = canonical_project_bytes(document)
    return PreparedProject(
        document=document,
        original_schema_version=original_version,
        schema_version=raw_version,
        migrations=tuple(migrations),
        sha256=hashlib.sha256(canonical).hexdigest(),
        summary=_resource_summary(document),
    )

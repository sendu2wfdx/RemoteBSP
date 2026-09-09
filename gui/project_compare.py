#!/usr/bin/env python3
"""RemoteBSP Studio 版本化工程的有界、确定性差异比较。"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass

from project_artifacts import build_resource_entries
from project_config import ProjectConfigError, collect_validated_project


COMPARE_SCHEMA_VERSION = 1
MAX_PROJECT_BYTES = 128 * 1024
MAX_RESOURCE_ENTRIES = 256
MAX_CHANGED_FIELDS = 512


@dataclass(frozen=True)
class ComparableProject:
    board: dict
    project_sha256: str
    schema_version: int
    original_schema_version: int
    migrations: tuple[str, ...]
    resources: tuple[dict, ...]


def _json_bytes(value: object) -> bytes:
    try:
        return (json.dumps(value, ensure_ascii=False, allow_nan=False,
                           sort_keys=True, separators=(",", ":")) + "\n").encode(
                               "utf-8")
    except (TypeError, ValueError) as error:
        raise ProjectConfigError(f"比较输入不是有效JSON：{error}") from error


def _bounded_input(value: object, label: str) -> None:
    size = len(_json_bytes(value))
    if size > MAX_PROJECT_BYTES:
        raise ProjectConfigError(
            f"{label}规范JSON超过{MAX_PROJECT_BYTES // 1024} KiB比较上限")


def _prepare(value: object, catalog: dict, label: str) -> ComparableProject:
    _bounded_input(value, label)
    inventory = collect_validated_project(value, catalog)
    resources = tuple(build_resource_entries(
        inventory.board, inventory.resources))
    if len(resources) > MAX_RESOURCE_ENTRIES:
        raise ProjectConfigError(
            f"{label}资源条目超过{MAX_RESOURCE_ENTRIES}项比较上限")
    prepared = inventory.prepared
    return ComparableProject(
        board=inventory.board, project_sha256=prepared.sha256,
        schema_version=prepared.schema_version,
        original_schema_version=prepared.original_schema_version,
        migrations=prepared.migrations, resources=resources)


def _identity(entry: dict) -> str:
    """返回不包含可修改名称、引脚和合同的稳定槽位身份。"""
    return f"{entry['kind']}/{int(entry['index']):03d}"


def _resource_label(entry: dict) -> str:
    labels = {
        "gpio": "GPIO", "uart": "UART", "motion_axis": "运动轴",
        "pwm": "PWM", "ws2812": "WS2812", "i2c_bus": "I2C总线",
        "i2c_device": "I2C设备", "spi_bus": "SPI总线",
        "spi_device": "SPI设备",
    }
    return f"{labels.get(entry['kind'], entry['kind'])}“{entry['name']}”"


def _binding_map(entry: dict) -> dict:
    return {item["role"]: {key: value for key, value in item.items()
                           if key != "role"}
            for item in entry["bindings"]}


def _comparable_entry(entry: dict) -> dict:
    return {
        "name": entry["name"],
        "backend_status": entry["backend_status"],
        "bindings": _binding_map(entry),
        "parameters": entry["parameters"],
    }


def _flatten_changes(before: object, after: object,
                     prefix: str = "") -> list[tuple[str, object, object]]:
    if isinstance(before, dict) and isinstance(after, dict):
        result: list[tuple[str, object, object]] = []
        for key in sorted(set(before) | set(after)):
            path = f"{prefix}.{key}" if prefix else key
            if key not in before:
                result.append((path, None, after[key]))
            elif key not in after:
                result.append((path, before[key], None))
            else:
                result.extend(_flatten_changes(before[key], after[key], path))
        return result
    return [] if before == after else [(prefix, before, after)]


_CONTRACT_FIELDS = {
    "flags", "maximum_clock_hz", "maximum_transfer_bytes",
    "queue_capacity", "minimum_timeout_us", "maximum_timeout_us",
    "maximum_operations_per_second",
}


def _category(path: str) -> str:
    if path.startswith("bindings."):
        return "wiring"
    leaf = path.rsplit(".", 1)[-1]
    if leaf in _CONTRACT_FIELDS:
        return "contract"
    return "setting"


def _field_label(path: str) -> str:
    parts = path.split(".")
    if parts[0] == "bindings" and len(parts) >= 3:
        suffix = "共享来源" if parts[-1] == "shared_from" else "接线"
        return f"{parts[1]} {suffix}"
    labels = {
        "name": "逻辑名称", "backend_status": "后端状态",
        "flags": "能力标志", "maximum_clock_hz": "最高时钟",
        "maximum_transfer_bytes": "最大事务字节数",
        "queue_capacity": "队列容量",
        "minimum_timeout_us": "最短超时",
        "maximum_timeout_us": "最长超时",
        "maximum_operations_per_second": "每秒最大操作数",
        "baud_rate": "波特率", "frequency_hz": "频率",
        "maximum_step_rate_hz": "最高STEP频率",
        "endpoint_id": "硬件端点", "controller": "控制器",
        "chip_select_pin": "片选引脚", "address": "I2C地址",
        "mode": "SPI Mode", "bits_per_word": "SPI位宽",
        "driver_type": "驱动类型", "tmc_address": "TMC地址",
        "pixel_count": "灯珠数量", "color_order": "色序",
        "default_duty_percent": "默认占空比",
    }
    return labels.get(parts[-1], parts[-1])


def _short_value(value: object) -> str:
    if value is None:
        return "未设置"
    if value is True:
        return "是"
    if value is False:
        return "否"
    if isinstance(value, list):
        text = "、".join(str(item) for item in value[:8])
        return f"[{text}{'…' if len(value) > 8 else ''}]"
    text = str(value).replace("\r", " ").replace("\n", " ")
    return text[:96] + ("…" if len(text) > 96 else "")


def _field_change(path: str, before: object, after: object) -> dict:
    label = _field_label(path)
    return {
        "path": path,
        "category": _category(path),
        "before": before,
        "after": after,
        "summary": f"{label}：{_short_value(before)} → {_short_value(after)}",
    }


def _project_info(project: ComparableProject) -> dict:
    return {
        "board_id": project.board["id"],
        "board_label": project.board["label"],
        "project_sha256": project.project_sha256,
        "schema_version": project.schema_version,
        "original_schema_version": project.original_schema_version,
        "migrations": list(project.migrations),
        "resource_count": len(project.resources),
    }


def compare_projects(left_value: object, right_value: object,
                     catalog: dict) -> dict:
    """比较两份合法工程，返回稳定机器差异和中文摘要。"""
    left = _prepare(left_value, catalog, "左侧工程")
    right = _prepare(right_value, catalog, "右侧工程")
    changes: list[dict] = []

    if left.original_schema_version != right.original_schema_version or \
            left.schema_version != right.schema_version:
        changes.append({
            "change": "schema_changed",
            "summary": (
                f"工程版本：原始 v{left.original_schema_version} / 规范 v"
                f"{left.schema_version} → 原始 v{right.original_schema_version} "
                f"/ 规范 v{right.schema_version}"),
            "before": {"original": left.original_schema_version,
                       "normalized": left.schema_version},
            "after": {"original": right.original_schema_version,
                      "normalized": right.schema_version},
        })
    if left.board["id"] != right.board["id"]:
        changes.append({
            "change": "board_changed",
            "summary": (f"板卡：{left.board['label']} → "
                        f"{right.board['label']}"),
            "before": left.board["id"], "after": right.board["id"],
        })

    left_entries = {_identity(item): item for item in left.resources}
    right_entries = {_identity(item): item for item in right.resources}
    left_keys = set(left_entries)
    right_keys = set(right_entries)
    for identity in sorted(left_keys - right_keys):
        entry = left_entries[identity]
        changes.append({
            "change": "resource_removed", "identity": identity,
            "kind": entry["kind"], "name": entry["name"],
            "resource_sha256": hashlib.sha256(
                _json_bytes(entry)).hexdigest(),
            "summary": f"删除 {_resource_label(entry)}",
        })
    for identity in sorted(right_keys - left_keys):
        entry = right_entries[identity]
        changes.append({
            "change": "resource_added", "identity": identity,
            "kind": entry["kind"], "name": entry["name"],
            "resource_sha256": hashlib.sha256(
                _json_bytes(entry)).hexdigest(),
            "summary": f"新增 {_resource_label(entry)}",
        })

    changed_field_count = 0
    omitted_field_count = 0
    modified_count = 0
    unchanged_count = 0
    for identity in sorted(left_keys & right_keys):
        before = left_entries[identity]
        after = right_entries[identity]
        raw_fields = _flatten_changes(
            _comparable_entry(before), _comparable_entry(after))
        if not raw_fields:
            unchanged_count += 1
            continue
        modified_count += 1
        remaining = max(0, MAX_CHANGED_FIELDS - changed_field_count)
        selected = raw_fields[:remaining]
        omitted_field_count += len(raw_fields) - len(selected)
        fields = [_field_change(*item) for item in selected]
        changed_field_count += len(fields)
        detail = "；".join(item["summary"] for item in fields[:3])
        if not detail:
            detail = "变化详情已达到输出上限"
        elif len(fields) > 3 or len(raw_fields) > len(fields):
            detail += "；还有其他变化"
        changes.append({
            "change": "resource_modified", "identity": identity,
            "kind": after["kind"], "name": after["name"],
            "field_change_count": len(raw_fields), "fields": fields,
            "summary": f"修改 {_resource_label(after)}：{detail}",
        })

    added_count = len(right_keys - left_keys)
    removed_count = len(left_keys - right_keys)
    metadata_count = sum(item["change"] in (
        "schema_changed", "board_changed") for item in changes)
    equal = not changes
    if equal:
        headline = "两份工程的板卡与资源配置一致"
    elif not added_count and not removed_count and not modified_count:
        headline = "资源配置一致，但工程版本或板卡信息有变化"
    else:
        headline = (
            f"发现 {added_count} 项新增、{removed_count} 项删除、"
            f"{modified_count} 项修改")
        if metadata_count:
            headline += "，并有工程级变化"

    response = {
        "ok": True,
        "format": "PROJECT_COMPARISON_V1",
        "schema_version": COMPARE_SCHEMA_VERSION,
        "equal": equal,
        "left": _project_info(left),
        "right": _project_info(right),
        "summary": {
            "headline": headline,
            "added": added_count,
            "removed": removed_count,
            "modified": modified_count,
            "unchanged": unchanged_count,
            "project_level_changes": metadata_count,
            "field_changes": changed_field_count + omitted_field_count,
            "omitted_field_changes": omitted_field_count,
            "truncated": omitted_field_count > 0,
            "project_hash_equal":
                left.project_sha256 == right.project_sha256,
        },
        "changes": changes,
        "limits": {
            "max_project_bytes": MAX_PROJECT_BYTES,
            "max_resource_entries": MAX_RESOURCE_ENTRIES,
            "max_changed_fields": MAX_CHANGED_FIELDS,
        },
    }
    response["comparison_sha256"] = hashlib.sha256(
        _json_bytes(response)).hexdigest()
    return response

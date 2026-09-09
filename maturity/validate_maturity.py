#!/usr/bin/env python3
"""严格验证 RemoteBSP 成熟度清单 v1。"""

from __future__ import annotations

import argparse
import json
import re
from datetime import date as calendar_date
from pathlib import Path, PurePosixPath


SCHEMA_VERSION = 1
MAXIMUM_MANIFEST_BYTES = 256 * 1024
DIMENSION_IDS = {
    "determinism", "resource_model", "multi_node_synchronization",
    "fault_isolation", "observability", "configuration_build",
    "upgrade_recovery", "runtime_api", "security_boundaries",
    "hardware_evidence",
}
LEVEL_NAMES = (
    "implemented", "automated_test", "cross_compiled", "hardware_verified")
STATUSES = {"absent", "partial", "verified", "not_applicable"}
IDENTIFIER = re.compile(r"^[a-z][a-z0-9_-]{0,63}$")
IMPLEMENTED_KINDS = {"source", "document"}
AUTOMATED_KINDS = {"host_test", "mock_test", "vcan_test"}
CROSS_COMPILE_KINDS = {
    "cross_compile_record", "documented_cross_compile_result"}
HARDWARE_KINDS = {
    "hardware_artifact", "hardware_test_record",
    "documented_hardware_result"}


class MaturityValidationError(ValueError):
    """成熟度清单不满足 v1 封闭契约。"""


def _pairs(pairs: list[tuple[str, object]]) -> dict:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise MaturityValidationError(f"JSON包含重复字段：{key}")
        result[key] = value
    return result


def _exact(item: dict, expected: set[str], path: str) -> None:
    actual = set(item)
    if actual != expected:
        missing = expected - actual
        extra = actual - expected
        details = []
        if missing:
            details.append("缺少" + ",".join(sorted(missing)))
        if extra:
            details.append("未知" + ",".join(sorted(extra)))
        raise MaturityValidationError(
            f"{path}字段不匹配：{'；'.join(details)}")


def _object(value: object, path: str) -> dict:
    if not isinstance(value, dict):
        raise MaturityValidationError(f"{path}必须是对象")
    return value


def _array(value: object, path: str) -> list:
    if not isinstance(value, list):
        raise MaturityValidationError(f"{path}必须是数组")
    return value


def _string(value: object, path: str, *, maximum: int = 500) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise MaturityValidationError(
            f"{path}必须是1～{maximum}字符的字符串")
    return value


def _identifier(value: object, path: str) -> str:
    value = _string(value, path, maximum=64)
    if not IDENTIFIER.fullmatch(value):
        raise MaturityValidationError(f"{path}不是合法标识符")
    return value


def load_manifest(path: Path) -> dict:
    try:
        encoded = path.read_bytes()
    except OSError as error:
        raise MaturityValidationError(f"无法读取成熟度清单：{error}") from error
    if len(encoded) > MAXIMUM_MANIFEST_BYTES:
        raise MaturityValidationError("成熟度清单超过256 KiB上限")
    try:
        return json.loads(
            encoded.decode("utf-8"), object_pairs_hook=_pairs,
            parse_constant=lambda value: (_ for _ in ()).throw(
                MaturityValidationError(f"JSON包含非标准数值：{value}")))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise MaturityValidationError(f"成熟度清单不是合法UTF-8 JSON：{error}") from error


def _evidence_path(value: object, repo_root: Path, path: str) -> str:
    text = _string(value, path, maximum=240)
    candidate = PurePosixPath(text)
    if candidate.is_absolute() or "\\" in text or \
            any(part in {"", ".", ".."} for part in candidate.parts):
        raise MaturityValidationError(f"{path}必须是规范的仓库相对路径")
    resolved = (repo_root / Path(*candidate.parts)).resolve()
    try:
        resolved.relative_to(repo_root.resolve())
    except ValueError as error:
        raise MaturityValidationError(f"{path}越出仓库边界") from error
    if not resolved.is_file():
        raise MaturityValidationError(f"{path}引用的证据文件不存在：{text}")
    return text


def _validate_level(level_value: object, level_name: str, dimension_id: str,
                    repo_root: Path,
                    evidence_ids: set[str]) -> tuple[str, set[str]]:
    path = f"dimensions.{dimension_id}.{level_name}"
    level = _object(level_value, path)
    _exact(level, {"status", "evidence", "blocker_ids"}, path)
    status = _string(level["status"], path + ".status", maximum=20)
    if status not in STATUSES:
        raise MaturityValidationError(f"{path}.status不受支持：{status}")
    evidence = _array(level["evidence"], path + ".evidence")
    blocker_id_list = [
        _identifier(value, path + ".blocker_ids")
        for value in _array(level["blocker_ids"], path + ".blocker_ids")]
    blocker_ids = set(blocker_id_list)
    if len(blocker_ids) != len(blocker_id_list):
        raise MaturityValidationError(f"{path}.blocker_ids包含重复项")
    if status in {"absent", "not_applicable"} and evidence:
        raise MaturityValidationError(f"{path}为{status}时不能附带完成证据")
    if status in {"partial", "verified"} and not evidence:
        raise MaturityValidationError(f"{path}为{status}时必须附带证据")
    if status in {"absent", "partial"} and not blocker_ids:
        raise MaturityValidationError(f"{path}为{status}时必须列出blocker")

    allowed_kinds = {
        "implemented": IMPLEMENTED_KINDS,
        "automated_test": AUTOMATED_KINDS,
        "cross_compiled": CROSS_COMPILE_KINDS,
        "hardware_verified": HARDWARE_KINDS,
    }[level_name]
    for index, raw in enumerate(evidence):
        evidence_path = f"{path}.evidence[{index}]"
        item = _object(raw, evidence_path)
        _exact(item, {"id", "kind", "path", "note"}, evidence_path)
        evidence_id = _identifier(item["id"], evidence_path + ".id")
        if evidence_id in evidence_ids:
            raise MaturityValidationError(f"证据ID重复：{evidence_id}")
        evidence_ids.add(evidence_id)
        kind = _string(item["kind"], evidence_path + ".kind", maximum=48)
        if kind not in allowed_kinds:
            raise MaturityValidationError(
                f"{evidence_path}.kind={kind}不能证明{level_name}")
        _evidence_path(item["path"], repo_root, evidence_path + ".path")
        note = _string(item["note"], evidence_path + ".note")
        if kind == "mock_test" and "Mock" not in note:
            raise MaturityValidationError(
                f"{evidence_path}必须明确说明Mock不是实体实测")
    return status, blocker_ids


def validate_manifest(document: object, repo_root: Path) -> None:
    root = _object(document, "manifest")
    _exact(root, {
        "schema_version", "manifest_id", "evidence_as_of",
        "status_definitions", "dimensions", "blockers",
        "overall_comparison",
    }, "manifest")
    if root["schema_version"] != SCHEMA_VERSION or \
            isinstance(root["schema_version"], bool):
        raise MaturityValidationError("manifest.schema_version必须为1")
    if root["manifest_id"] != "remotebsp-maturity-baseline-v1":
        raise MaturityValidationError("manifest.manifest_id不受支持")
    date = _string(root["evidence_as_of"], "manifest.evidence_as_of", maximum=10)
    try:
        calendar_date.fromisoformat(date)
    except ValueError as error:
        raise MaturityValidationError(
            "manifest.evidence_as_of必须为有效的YYYY-MM-DD日期") from error
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
        raise MaturityValidationError("manifest.evidence_as_of必须为YYYY-MM-DD")
    definitions = _object(root["status_definitions"], "status_definitions")
    _exact(definitions, STATUSES, "status_definitions")
    for status in STATUSES:
        _string(definitions[status], f"status_definitions.{status}")

    dimensions = _array(root["dimensions"], "dimensions")
    dimension_ids: set[str] = set()
    dimension_statuses: dict[str, dict[str, str]] = {}
    evidence_ids: set[str] = set()
    referenced_blockers: dict[str, set[str]] = {}
    for index, raw in enumerate(dimensions):
        path = f"dimensions[{index}]"
        item = _object(raw, path)
        _exact(item, {
            "id", "title", "summary", "implemented", "automated_test",
            "cross_compiled", "hardware_verified",
        }, path)
        dimension_id = _identifier(item["id"], path + ".id")
        if dimension_id in dimension_ids:
            raise MaturityValidationError(f"成熟度维度重复：{dimension_id}")
        dimension_ids.add(dimension_id)
        _string(item["title"], path + ".title", maximum=80)
        _string(item["summary"], path + ".summary")
        referenced_blockers[dimension_id] = set()
        dimension_statuses[dimension_id] = {}
        for level_name in LEVEL_NAMES:
            status, blocker_ids = _validate_level(
                item[level_name], level_name, dimension_id, repo_root,
                evidence_ids)
            dimension_statuses[dimension_id][level_name] = status
            referenced_blockers[dimension_id].update(blocker_ids)
    if not DIMENSION_IDS.issubset(dimension_ids):
        raise MaturityValidationError(
            "成熟度维度集合不完整：" +
            ",".join(sorted(DIMENSION_IDS - dimension_ids)))

    blocker_items = _array(root["blockers"], "blockers")
    blockers: dict[str, str | None] = {}
    for index, raw in enumerate(blocker_items):
        path = f"blockers[{index}]"
        item = _object(raw, path)
        _exact(item, {
            "id", "dimension_id", "summary", "requires_hardware",
            "closure_evidence",
        }, path)
        blocker_id = _identifier(item["id"], path + ".id")
        if blocker_id in blockers:
            raise MaturityValidationError(f"blocker ID重复：{blocker_id}")
        dimension_id = item["dimension_id"]
        if dimension_id is not None:
            dimension_id = _identifier(dimension_id, path + ".dimension_id")
            if dimension_id not in dimension_ids:
                raise MaturityValidationError(f"blocker引用未知维度：{dimension_id}")
        if not isinstance(item["requires_hardware"], bool):
            raise MaturityValidationError(f"{path}.requires_hardware必须为布尔值")
        _string(item["summary"], path + ".summary")
        _string(item["closure_evidence"], path + ".closure_evidence")
        blockers[blocker_id] = dimension_id

    for dimension_id, blocker_ids in referenced_blockers.items():
        for blocker_id in blocker_ids:
            if blocker_id not in blockers:
                raise MaturityValidationError(f"引用了未知blocker：{blocker_id}")
            owner = blockers[blocker_id]
            if owner not in {None, dimension_id}:
                raise MaturityValidationError(
                    f"维度{dimension_id}引用了其他维度的blocker：{blocker_id}")

    comparison = _object(root["overall_comparison"], "overall_comparison")
    _exact(comparison, {
        "claim", "allowed", "status", "blocker_ids",
        "benchmark_evidence", "reason",
    }, "overall_comparison")
    if comparison["claim"] != "RemoteBSP 整体超过 Klipper":
        raise MaturityValidationError(
            "overall_comparison.claim不受支持")
    allowed = comparison["allowed"]
    if not isinstance(allowed, bool):
        raise MaturityValidationError("overall_comparison.allowed必须为布尔值")
    status = comparison["status"]
    if status not in {"blocked", "ready"}:
        raise MaturityValidationError("overall_comparison.status不受支持")
    comparison_blocker_list = [
        _identifier(value, "overall_comparison.blocker_ids")
        for value in _array(comparison["blocker_ids"],
                            "overall_comparison.blocker_ids")]
    comparison_blockers = set(comparison_blocker_list)
    if len(comparison_blockers) != len(comparison_blocker_list):
        raise MaturityValidationError(
            "overall_comparison.blocker_ids包含重复项")
    if not comparison_blockers.issubset(blockers):
        raise MaturityValidationError("整体比较引用了未知blocker")
    benchmark_evidence = _array(
        comparison["benchmark_evidence"],
        "overall_comparison.benchmark_evidence")
    for index, raw in enumerate(benchmark_evidence):
        path = f"overall_comparison.benchmark_evidence[{index}]"
        item = _object(raw, path)
        _exact(item, {"id", "kind", "path", "note"}, path)
        evidence_id = _identifier(item["id"], path + ".id")
        if evidence_id in evidence_ids:
            raise MaturityValidationError(f"证据ID重复：{evidence_id}")
        evidence_ids.add(evidence_id)
        if item["kind"] != "comparative_benchmark":
            raise MaturityValidationError(
                "整体比较证据必须使用comparative_benchmark类型")
        _evidence_path(item["path"], repo_root, path + ".path")
        _string(item["note"], path + ".note")

    if allowed:
        if status != "ready" or comparison_blockers or blockers or any(
                referenced_blockers.values()):
            raise MaturityValidationError(
                "整体比较仅能在ready且清单中没有任何blocker时允许")
        if not benchmark_evidence:
            raise MaturityValidationError(
                "整体比较缺少明确的comparative_benchmark证据")
        for dimension_id, levels in dimension_statuses.items():
            if levels["implemented"] != "verified" or \
                    levels["automated_test"] != "verified" or \
                    levels["cross_compiled"] not in {
                        "verified", "not_applicable"} or \
                    levels["hardware_verified"] not in {
                        "verified", "not_applicable"}:
                raise MaturityValidationError(
                    f"维度{dimension_id}尚未达到整体比较门槛")
    else:
        if status != "blocked" or not comparison_blockers:
            raise MaturityValidationError(
                "被阻止的整体比较必须至少列出一个有效blocker")
    _string(comparison["reason"], "overall_comparison.reason")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest", type=Path,
        default=root / "maturity" / "remotebsp-maturity-v1.json",
        help="待验证的成熟度清单")
    parser.add_argument(
        "--repo-root", type=Path, default=root,
        help="证据相对路径所属仓库根目录")
    args = parser.parse_args()
    try:
        document = load_manifest(args.manifest)
        validate_manifest(document, args.repo_root)
    except MaturityValidationError as error:
        parser.exit(1, f"成熟度清单验证失败：{error}\n")
    comparison = document["overall_comparison"]
    print("RemoteBSP 成熟度基线 v1 验证通过；整体比较状态="
          f"{comparison['status']}，allowed={str(comparison['allowed']).lower()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

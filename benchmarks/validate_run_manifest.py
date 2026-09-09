#!/usr/bin/env python3
"""验证一轮 RemoteBSP / Klipper 配对基准运行记录和原始文件哈希。"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import stat
from datetime import datetime
from pathlib import Path, PurePosixPath


MAXIMUM_MANIFEST_BYTES = 512 * 1024
MAXIMUM_RAW_FILES = 128
MAXIMUM_RAW_BYTES_TOTAL = 16 * 1024 * 1024 * 1024
IDENTIFIER = re.compile(r"^[a-z][a-z0-9_-]{0,95}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
REVISION = re.compile(r"^[0-9a-f]{40}$")
UNITS = {"ns", "us", "ms", "count", "percent", "bytes_per_second"}


class RunManifestError(ValueError):
    """运行记录不满足封闭契约或原始证据校验失败。"""


def _pairs(pairs: list[tuple[str, object]]) -> dict:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise RunManifestError(f"JSON包含重复字段：{key}")
        result[key] = value
    return result


def _object(value: object, path: str) -> dict:
    if not isinstance(value, dict):
        raise RunManifestError(f"{path}必须是对象")
    return value


def _array(value: object, path: str) -> list:
    if not isinstance(value, list):
        raise RunManifestError(f"{path}必须是数组")
    return value


def _exact(value: dict, fields: set[str], path: str) -> None:
    if set(value) != fields:
        raise RunManifestError(f"{path}字段集合不匹配")


def _string(value: object, path: str, maximum: int = 500) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise RunManifestError(f"{path}必须是1～{maximum}字符字符串")
    return value


def _identifier(value: object, path: str) -> str:
    text = _string(value, path, 96)
    if not IDENTIFIER.fullmatch(text):
        raise RunManifestError(f"{path}不是合法标识符")
    return text


def _sha256(value: object, path: str) -> str:
    text = _string(value, path, 64)
    if not SHA256.fullmatch(text):
        raise RunManifestError(f"{path}不是SHA-256")
    return text


def _timestamp(value: object, path: str) -> datetime:
    text = _string(value, path, 40)
    if not text.endswith("Z"):
        raise RunManifestError(f"{path}必须是UTC Z时间")
    try:
        parsed = datetime.fromisoformat(text[:-1] + "+00:00")
    except ValueError as error:
        raise RunManifestError(f"{path}不是合法时间") from error
    return parsed


def _repo_file(value: object, repo_root: Path, path: str) -> Path:
    text = _string(value, path, 240)
    candidate = PurePosixPath(text)
    if candidate.is_absolute() or "\\" in text or any(
            part in {"", ".", ".."} for part in candidate.parts):
        raise RunManifestError(f"{path}必须是规范仓库相对路径")
    lexical = repo_root / Path(*candidate.parts)
    current = repo_root
    for part in candidate.parts:
        current = current / part
        if current.is_symlink():
            raise RunManifestError(f"{path}不能经过符号链接")
    resolved = lexical.resolve()
    try:
        resolved.relative_to(repo_root.resolve())
    except ValueError as error:
        raise RunManifestError(f"{path}越出仓库") from error
    if not resolved.is_file():
        raise RunManifestError(f"{path}必须是仓库内普通文件")
    return resolved


def _reject_excessive_json_depth(text: str, maximum: int = 64) -> None:
    depth = 0
    in_string = False
    escaped = False
    for character in text:
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
        elif character == '"':
            in_string = True
        elif character in "[{":
            depth += 1
            if depth > maximum:
                raise RunManifestError(f"JSON嵌套超过{maximum}层")
        elif character in "]}":
            depth -= 1


def load_run_manifest(path: Path) -> dict:
    if path.is_symlink() or not path.is_file():
        raise RunManifestError("运行记录必须是普通非符号链接文件")
    try:
        with path.open("rb") as stream:
            encoded = stream.read(MAXIMUM_MANIFEST_BYTES + 1)
    except OSError as error:
        raise RunManifestError(f"无法读取运行记录：{error}") from error
    if len(encoded) > MAXIMUM_MANIFEST_BYTES:
        raise RunManifestError("运行记录超过512 KiB")
    try:
        text = encoded.decode("utf-8")
        _reject_excessive_json_depth(text)
        return json.loads(text, object_pairs_hook=_pairs,
                          parse_constant=lambda value: (_ for _ in ()).throw(
                              RunManifestError(f"非法数值：{value}")))
    except RunManifestError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError,
            ValueError) as error:
        raise RunManifestError(f"运行记录不是合法UTF-8 JSON：{error}") from error


def validate_run_manifest(document: object, repo_root: Path, *,
                          expected_plan_sha256: str | None = None) -> set[str]:
    root = _object(document, "run")
    _exact(root, {"schema_version", "format", "run_id", "plan_sha256",
                  "started_at", "completed_at", "operator_id",
                  "environment_digest", "systems", "scenarios"}, "run")
    if root["schema_version"] != 1 or isinstance(root["schema_version"], bool):
        raise RunManifestError("schema_version必须为1")
    if root["format"] != "REMOTEBSP_KLIPPER_BENCHMARK_RUN_V1":
        raise RunManifestError("format不受支持")
    _identifier(root["run_id"], "run_id")
    plan_sha256 = _sha256(root["plan_sha256"], "plan_sha256")
    if expected_plan_sha256 is not None and plan_sha256 != expected_plan_sha256:
        raise RunManifestError("运行记录未绑定当前计划摘要")
    started = _timestamp(root["started_at"], "started_at")
    completed = _timestamp(root["completed_at"], "completed_at")
    if completed < started:
        raise RunManifestError("completed_at早于started_at")
    _identifier(root["operator_id"], "operator_id")
    _sha256(root["environment_digest"], "environment_digest")

    systems = _object(root["systems"], "systems")
    _exact(systems, {"remotebsp", "klipper"}, "systems")
    for name in ("remotebsp", "klipper"):
        system = _object(systems[name], f"systems.{name}")
        _exact(system, {"revision", "host_config_sha256",
                        "firmware_config_sha256s", "firmware_image_sha256s"},
               f"systems.{name}")
        revision = _string(system["revision"], f"systems.{name}.revision", 40)
        if not REVISION.fullmatch(revision):
            raise RunManifestError(f"systems.{name}.revision不是40位Git哈希")
        _sha256(system["host_config_sha256"], f"systems.{name}.host_config_sha256")
        for field in ("firmware_config_sha256s", "firmware_image_sha256s"):
            firmware_hashes = _array(system[field], f"systems.{name}.{field}")
            if not 1 <= len(firmware_hashes) <= 16:
                raise RunManifestError(f"systems.{name}.{field}必须为1～16项")
            checked_hashes = [
                _sha256(value, f"systems.{name}.{field}[{index}]")
                for index, value in enumerate(firmware_hashes)]
            if len(set(checked_hashes)) != len(checked_hashes):
                raise RunManifestError(f"systems.{name}.{field}包含重复项")

    scenarios = _array(root["scenarios"], "scenarios")
    if not 1 <= len(scenarios) <= 16:
        raise RunManifestError("scenarios必须为1～16项")
    scenario_ids: set[str] = set()
    raw_paths: set[str] = set()
    total_raw_bytes = 0
    for scenario_index, raw_scenario in enumerate(scenarios):
        path = f"scenarios[{scenario_index}]"
        scenario = _object(raw_scenario, path)
        _exact(scenario, {"id", "status", "safety_failure", "metrics",
                          "raw_files"}, path)
        scenario_id = _identifier(scenario["id"], path + ".id")
        if scenario_id in scenario_ids:
            raise RunManifestError(f"{path}.id重复")
        scenario_ids.add(scenario_id)
        if scenario["status"] not in {"passed", "failed"}:
            raise RunManifestError(f"{path}.status无效")
        if not isinstance(scenario["safety_failure"], bool):
            raise RunManifestError(f"{path}.safety_failure必须是布尔值")
        if scenario["safety_failure"] and scenario["status"] != "failed":
            raise RunManifestError(f"{path}安全失败时status必须为failed")

        metrics = _array(scenario["metrics"], path + ".metrics")
        if not 1 <= len(metrics) <= 32:
            raise RunManifestError(f"{path}.metrics必须为1～32项")
        metric_ids: set[str] = set()
        for metric_index, raw_metric in enumerate(metrics):
            metric_path = f"{path}.metrics[{metric_index}]"
            metric = _object(raw_metric, metric_path)
            _exact(metric, {"id", "unit", "remotebsp", "klipper"}, metric_path)
            metric_id = _identifier(metric["id"], metric_path + ".id")
            if metric_id in metric_ids:
                raise RunManifestError(f"{metric_path}.id重复")
            metric_ids.add(metric_id)
            if metric["unit"] not in UNITS:
                raise RunManifestError(f"{metric_path}.unit无效")
            for system_name in ("remotebsp", "klipper"):
                measurement_path = f"{metric_path}.{system_name}"
                measurement = _object(metric[system_name], measurement_path)
                _exact(measurement, {"value", "sample_count"}, measurement_path)
                value = measurement["value"]
                if isinstance(value, bool) or \
                        not isinstance(value, (int, float)) or \
                        not math.isfinite(value):
                    raise RunManifestError(
                        f"{measurement_path}.value必须是有限数值")
                count = measurement["sample_count"]
                if isinstance(count, bool) or not isinstance(count, int) or \
                        not 1 <= count <= 100000000:
                    raise RunManifestError(
                        f"{measurement_path}.sample_count越界")

        raw_files = _array(scenario["raw_files"], path + ".raw_files")
        if not raw_files or len(raw_files) > 64:
            raise RunManifestError(f"{path}.raw_files必须为1～64项")
        for raw_index, raw_file_value in enumerate(raw_files):
            raw_path = f"{path}.raw_files[{raw_index}]"
            raw_file = _object(raw_file_value, raw_path)
            _exact(raw_file, {"path", "sha256", "size_bytes"}, raw_path)
            resolved = _repo_file(raw_file["path"], repo_root, raw_path + ".path")
            relative = resolved.relative_to(repo_root.resolve()).as_posix()
            if relative in raw_paths:
                raise RunManifestError(f"{raw_path}.path重复")
            raw_paths.add(relative)
            expected_hash = _sha256(raw_file["sha256"], raw_path + ".sha256")
            expected_size = raw_file["size_bytes"]
            if isinstance(expected_size, bool) or not isinstance(expected_size, int) or \
                    not 1 <= expected_size <= 4 * 1024 * 1024 * 1024:
                raise RunManifestError(f"{raw_path}.size_bytes越界")
            total_raw_bytes += expected_size
            if len(raw_paths) > MAXIMUM_RAW_FILES or \
                    total_raw_bytes > MAXIMUM_RAW_BYTES_TOTAL:
                raise RunManifestError("单次运行原始文件数量或总字节超过上限")
            try:
                actual_size, actual_hash = _hash_file(resolved)
            except OSError as error:
                raise RunManifestError(
                    f"{raw_path}读取失败：{error}") from error
            if actual_size != expected_size or actual_hash != expected_hash:
                raise RunManifestError(f"{raw_path}大小或SHA-256不匹配")
    return raw_paths


def _hash_file(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0)
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as stream:
        information = os.fstat(stream.fileno())
        if not stat.S_ISREG(information.st_mode):
            raise OSError("原始证据不是普通文件")
        size = 0
        while chunk := stream.read(1024 * 1024):
            size += len(chunk)
            if size > 4 * 1024 * 1024 * 1024:
                raise OSError("原始证据超过4 GiB上限")
            digest.update(chunk)
    return size, digest.hexdigest()


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True,
                        help="待验证的配对运行记录")
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument("--expected-plan-sha256",
                        help="预注册计划定义的SHA-256")
    args = parser.parse_args()
    try:
        manifest = load_run_manifest(args.manifest)
        paths = validate_run_manifest(
            manifest, args.repo_root,
            expected_plan_sha256=args.expected_plan_sha256)
    except RunManifestError as error:
        parser.exit(1, f"运行记录验证失败：{error}\n")
    print(f"运行记录所列文件完整性验证通过；原始文件={len(paths)}；"
          "不代表指标正确或比较结论")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

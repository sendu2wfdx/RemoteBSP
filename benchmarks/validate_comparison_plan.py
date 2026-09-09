#!/usr/bin/env python3
"""严格验证 RemoteBSP / Klipper 对照基准计划 v1。"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path, PurePosixPath

MAXIMUM_BYTES = 256 * 1024
IDENTIFIER = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
REVISION = re.compile(r"^[0-9a-f]{40}$")
STATUSES = {"draft", "preregistered"}
UNITS = {"ns", "us", "ms", "count", "percent", "bytes_per_second"}
DIRECTIONS = {"lower", "higher", "zero"}
STATISTICS = {"p50", "p95", "p99", "max", "sum", "failure_rate"}


class ComparisonPlanError(ValueError):
    """对照基准计划不满足封闭契约。"""


def _pairs(pairs: list[tuple[str, object]]) -> dict:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ComparisonPlanError(f"JSON包含重复字段：{key}")
        result[key] = value
    return result


def _object(value: object, path: str) -> dict:
    if not isinstance(value, dict):
        raise ComparisonPlanError(f"{path}必须是对象")
    return value


def _array(value: object, path: str) -> list:
    if not isinstance(value, list):
        raise ComparisonPlanError(f"{path}必须是数组")
    return value


def _exact(value: dict, fields: set[str], path: str) -> None:
    if set(value) != fields:
        raise ComparisonPlanError(f"{path}字段集合不匹配")


def _string(value: object, path: str, maximum: int = 1000) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise ComparisonPlanError(f"{path}必须是1～{maximum}字符的字符串")
    return value


def _boolean(value: object, path: str) -> bool:
    if not isinstance(value, bool):
        raise ComparisonPlanError(f"{path}必须是布尔值")
    return value


def _integer(value: object, path: str, low: int, high: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise ComparisonPlanError(f"{path}必须位于{low}～{high}")
    return value


def _optional_string(value: object, path: str) -> str | None:
    if value is None:
        return None
    return _string(value, path, 500)


def _repo_path(value: object, repo_root: Path, path: str,
               *, must_exist: bool) -> str:
    text = _string(value, path, 240)
    candidate = PurePosixPath(text)
    if candidate.is_absolute() or "\\" in text or any(
            part in {"", ".", ".."} for part in candidate.parts):
        raise ComparisonPlanError(f"{path}必须是规范仓库相对路径")
    lexical = repo_root / Path(*candidate.parts)
    current = repo_root
    for part in candidate.parts:
        current = current / part
        if current.is_symlink():
            raise ComparisonPlanError(f"{path}不能经过符号链接")
    resolved = lexical.resolve()
    try:
        resolved.relative_to(repo_root.resolve())
    except ValueError as error:
        raise ComparisonPlanError(f"{path}越出仓库") from error
    if must_exist and not resolved.is_file():
        raise ComparisonPlanError(f"{path}引用文件不存在：{text}")
    return text


def plan_definition_sha256(document: dict) -> str:
    """计算排除执行状态和产物路径后的预注册定义摘要。"""
    definition = {
        key: document[key]
        for key in (
            "schema_version", "plan_id", "scope", "system_pins", "fairness",
            "environment", "scenarios", "claim_gate")
    }
    encoded = json.dumps(definition, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


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
                raise ComparisonPlanError(f"JSON嵌套超过{maximum}层")
        elif character in "]}":
            depth -= 1


def load_plan(path: Path) -> dict:
    if path.is_symlink() or not path.is_file():
        raise ComparisonPlanError("计划必须是普通非符号链接文件")
    try:
        with path.open("rb") as stream:
            encoded = stream.read(MAXIMUM_BYTES + 1)
    except OSError as error:
        raise ComparisonPlanError(f"无法读取计划：{error}") from error
    if len(encoded) > MAXIMUM_BYTES:
        raise ComparisonPlanError("计划超过256 KiB上限")
    try:
        text = encoded.decode("utf-8")
        _reject_excessive_json_depth(text)
        return json.loads(text, object_pairs_hook=_pairs,
                          parse_constant=lambda value: (_ for _ in ()).throw(
                              ComparisonPlanError(f"非法数值：{value}")))
    except ComparisonPlanError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError,
            ValueError) as error:
        raise ComparisonPlanError(f"计划不是合法UTF-8 JSON：{error}") from error


def validate_plan(document: object, repo_root: Path) -> None:
    root = _object(document, "plan")
    _exact(root, {"schema_version", "plan_id", "status", "scope",
                  "system_pins", "fairness", "environment", "scenarios",
                  "artifacts", "claim_gate"}, "plan")
    if root["schema_version"] != 1 or isinstance(root["schema_version"], bool):
        raise ComparisonPlanError("schema_version必须为1")
    plan_id = _string(root["plan_id"], "plan_id", 64)
    if not IDENTIFIER.fullmatch(plan_id.replace("-", "_")):
        raise ComparisonPlanError("plan_id格式无效")
    status = _string(root["status"], "status", 20)
    if status not in STATUSES:
        raise ComparisonPlanError("status不受支持")
    _string(root["scope"], "scope")

    pins = _object(root["system_pins"], "system_pins")
    _exact(pins, {"remotebsp", "klipper"}, "system_pins")
    for system_name in ("remotebsp", "klipper"):
        pin = _object(pins[system_name], f"system_pins.{system_name}")
        _exact(pin, {"repository", "revision", "host_config",
                     "firmware_configs"}, f"system_pins.{system_name}")
        repository = _string(pin["repository"], f"{system_name}.repository", 240)
        if not repository.startswith("https://") or not repository.endswith(".git"):
            raise ComparisonPlanError(f"{system_name}.repository必须是HTTPS Git地址")
        revision = pin["revision"]
        if revision is not None and (not isinstance(revision, str) or
                                     not REVISION.fullmatch(revision)):
            raise ComparisonPlanError(f"{system_name}.revision必须是40位小写Git哈希")
        host_config = _optional_string(pin["host_config"], f"{system_name}.host_config")
        configs = _array(pin["firmware_configs"], f"{system_name}.firmware_configs")
        if len(configs) > 16:
            raise ComparisonPlanError(f"{system_name}.firmware_configs超过16项")
        checked_configs: list[str] = []
        for index, path in enumerate(configs):
            checked_configs.append(_repo_path(
                path, repo_root, f"{system_name}.firmware_configs[{index}]",
                must_exist=status != "draft"))
        if len(set(checked_configs)) != len(checked_configs):
            raise ComparisonPlanError(f"{system_name}.firmware_configs包含重复项")
        if host_config is not None:
            _repo_path(host_config, repo_root, f"{system_name}.host_config",
                       must_exist=status != "draft")
        if status != "draft" and (revision is None or host_config is None or not configs):
            raise ComparisonPlanError(f"{system_name}在预注册后必须锁定版本和配置")

    fairness = _object(root["fairness"], "fairness")
    fairness_flags = {"same_host", "same_mcu_boards", "same_transport",
                      "same_electrical_load", "same_instrument_and_probe_points",
                      "randomized_run_order"}
    _exact(fairness, fairness_flags | {"notes"}, "fairness")
    for flag in fairness_flags:
        if not _boolean(fairness[flag], f"fairness.{flag}"):
            raise ComparisonPlanError(f"fairness.{flag}必须为true")
    _string(fairness["notes"], "fairness.notes")

    environment = _object(root["environment"], "environment")
    _exact(environment, {"host_hardware", "host_os", "mcu_boards", "transport",
                         "instruments", "ambient_policy"}, "environment")
    for name in ("host_hardware", "host_os", "transport"):
        value = _optional_string(environment[name], f"environment.{name}")
        if status != "draft" and value is None:
            raise ComparisonPlanError(f"environment.{name}在预注册后不能为空")
    for name in ("mcu_boards", "instruments"):
        values = _array(environment[name], f"environment.{name}")
        if len(values) > 16:
            raise ComparisonPlanError(f"environment.{name}超过16项")
        checked_values: list[str] = []
        for index, value in enumerate(values):
            checked_values.append(_string(value, f"environment.{name}[{index}]", 240))
        if len(set(checked_values)) != len(checked_values):
            raise ComparisonPlanError(f"environment.{name}包含重复项")
        if status != "draft" and not values:
            raise ComparisonPlanError(f"environment.{name}在预注册后不能为空")
    _string(environment["ambient_policy"], "environment.ambient_policy")

    scenarios = _array(root["scenarios"], "scenarios")
    if not 1 <= len(scenarios) <= 16:
        raise ComparisonPlanError("scenarios必须为1～16项")
    scenario_ids: set[str] = set()
    metric_ids: set[str] = set()
    for index, raw in enumerate(scenarios):
        path = f"scenarios[{index}]"
        scenario = _object(raw, path)
        _exact(scenario, {"id", "purpose", "preconditions", "workload",
                          "warmup_runs", "measured_runs", "timeout_seconds",
                          "metrics"}, path)
        scenario_id = _string(scenario["id"], path + ".id", 64)
        if not IDENTIFIER.fullmatch(scenario_id) or scenario_id in scenario_ids:
            raise ComparisonPlanError(f"{path}.id无效或重复")
        scenario_ids.add(scenario_id)
        for name in ("purpose", "preconditions", "workload"):
            _string(scenario[name], f"{path}.{name}")
        _integer(scenario["warmup_runs"], path + ".warmup_runs", 1, 1000)
        _integer(scenario["measured_runs"], path + ".measured_runs", 30, 10000)
        _integer(scenario["timeout_seconds"], path + ".timeout_seconds", 1, 86400)
        metrics = _array(scenario["metrics"], path + ".metrics")
        if not 1 <= len(metrics) <= 32:
            raise ComparisonPlanError(f"{path}.metrics必须为1～32项")
        for metric_index, raw_metric in enumerate(metrics):
            metric_path = f"{path}.metrics[{metric_index}]"
            metric = _object(raw_metric, metric_path)
            _exact(metric, {"id", "unit", "direction", "statistic",
                            "measurement", "acceptance_rule"}, metric_path)
            metric_id = _string(metric["id"], metric_path + ".id", 64)
            if not IDENTIFIER.fullmatch(metric_id) or metric_id in metric_ids:
                raise ComparisonPlanError(f"{metric_path}.id无效或重复")
            metric_ids.add(metric_id)
            if metric["unit"] not in UNITS or metric["direction"] not in DIRECTIONS or \
                    metric["statistic"] not in STATISTICS:
                raise ComparisonPlanError(f"{metric_path}枚举值无效")
            _string(metric["measurement"], metric_path + ".measurement")
            rule = _string(metric["acceptance_rule"], metric_path + ".acceptance_rule")
            if status != "draft" and "预注册阈值" in rule:
                raise ComparisonPlanError(f"{metric_path}尚未填写固定验收阈值")

    artifacts = _object(root["artifacts"], "artifacts")
    _exact(artifacts, {"configuration_paths", "raw_data_paths", "analysis_path",
                       "run_manifest_paths"}, "artifacts")
    for name in ("configuration_paths", "raw_data_paths", "run_manifest_paths"):
        values = _array(artifacts[name], f"artifacts.{name}")
        if len(values) > 256:
            raise ComparisonPlanError(f"artifacts.{name}超过256项")
        checked_values: list[str] = []
        for index, value in enumerate(values):
            checked_values.append(_repo_path(
                value, repo_root, f"artifacts.{name}[{index}]",
                must_exist=(status == "preregistered" and
                            name == "configuration_paths")))
        if len(set(checked_values)) != len(checked_values):
            raise ComparisonPlanError(f"artifacts.{name}包含重复项")
    analysis_path = _optional_string(artifacts["analysis_path"],
                                     "artifacts.analysis_path")
    if analysis_path is not None:
        _repo_path(analysis_path, repo_root, "artifacts.analysis_path",
                   must_exist=False)
    if status == "preregistered" and not artifacts["configuration_paths"]:
        raise ComparisonPlanError("预注册计划必须列出配置证据路径")

    gate = _object(root["claim_gate"], "claim_gate")
    _exact(gate, {"minimum_independent_runs", "all_scenarios_required",
                  "raw_data_required", "no_failed_safety_trial",
                  "independent_reproduction_required", "rule"}, "claim_gate")
    _integer(gate["minimum_independent_runs"], "claim_gate.minimum_independent_runs", 3, 100)
    for flag in ("all_scenarios_required", "raw_data_required",
                 "no_failed_safety_trial", "independent_reproduction_required"):
        if not _boolean(gate[flag], f"claim_gate.{flag}"):
            raise ComparisonPlanError(f"claim_gate.{flag}必须为true")
    _string(gate["rule"], "claim_gate.rule")


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path,
                        default=repo_root / "benchmarks" / "comparison-plan-v1.json")
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    args = parser.parse_args()
    try:
        plan = load_plan(args.plan)
        validate_plan(plan, args.repo_root)
    except ComparisonPlanError as error:
        parser.exit(1, f"对照基准计划验证失败：{error}\n")
    print(f"对照基准计划v1验证通过；状态={plan['status']}，"
          "v1不接受executed或比较结论")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

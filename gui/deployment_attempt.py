#!/usr/bin/env python3
"""部署尝试证据：区分未执行、失败和完整回读核验，不冒充硬件成功。"""

from __future__ import annotations

import hashlib
import hmac
import json
import re
import secrets
from dataclasses import asdict
from datetime import datetime

from firmware_deployment import (
    DeploymentResult, DeviceIdentity, FirmwareDeploymentError)


FORMAT = "REMOTEBSP_DEPLOYMENT_ATTEMPT_V1"
MAX_STDOUT_BYTES = 1024 * 1024
_HASH = re.compile(r"[0-9a-f]{64}")
_UUID = re.compile(r"[0-9a-f]{32}")
_ATTEMPT_ID = re.compile(r"[0-9a-f]{32}")
_ERROR_TYPE = re.compile(r"[A-Za-z][A-Za-z0-9_.]{0,127}")


def _digest(value: dict) -> str:
    encoded = json.dumps(
        {key: item for key, item in value.items() if key != "sha256"},
        ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _timestamp(value: object, name: str) -> str:
    if not isinstance(value, str) or len(value) > 40 or not value.endswith("Z"):
        raise FirmwareDeploymentError(f"部署尝试{name}必须是UTC RFC3339时间")
    try:
        parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as error:
        raise FirmwareDeploymentError(f"部署尝试{name}无效") from error
    if parsed.utcoffset() is None or parsed.utcoffset().total_seconds() != 0:
        raise FirmwareDeploymentError(f"部署尝试{name}必须是UTC时间")
    return value


def create_absent_attempt(plan: object) -> dict:
    if not isinstance(plan, dict) or not isinstance(plan.get("sha256"), str) or \
            not _HASH.fullmatch(plan["sha256"]) or \
            plan.get("hardware_access") is not False or \
            plan.get("flash_performed") is not False:
        raise FirmwareDeploymentError("只有已验证的未执行部署计划可创建初始尝试")
    attempt = {
        "format": FORMAT, "schema_version": 1,
        "attempt_id": secrets.token_hex(16),
        "plan_format": plan.get("format"), "plan_sha256": plan["sha256"],
        "build_id": plan.get("build_id"), "backend": plan.get("backend"),
        "previous_attempt_sha256": None,
        "started_utc": None, "ended_utc": None,
        "execution": {"status": "absent", "tool_invoked": False,
                      "exit_code": None, "stdout_byte_count": 0,
                      "stdout_sha256": None, "error_type": None},
        "readback": {"status": "absent", "error_type": None,
                     "expected_identity": plan.get("expected_identity"),
                     "observed_identity": None},
        "outcome": "absent", "hardware_success_claimed": False,
    }
    attempt["sha256"] = _digest(attempt)
    return validate_deployment_attempt(attempt)


def finalize_attempt(
        initial: object, plan: object, *, started_utc: str, ended_utc: str,
        tool_invoked: bool, exit_code: int | None, stdout: bytes,
        result: DeploymentResult | None = None,
        execution_error_type: str | None = None,
        readback_error_type: str | None = None) -> dict:
    previous = validate_deployment_attempt(initial)
    if previous["outcome"] != "absent" or previous["plan_sha256"] != plan.get("sha256"):
        raise FirmwareDeploymentError("只能从同一计划的未执行尝试生成终态")
    start = _timestamp(started_utc, "开始时间")
    end = _timestamp(ended_utc, "结束时间")
    if end < start or tool_invoked is not True or type(exit_code) is not int or \
            not -2147483648 <= exit_code <= 2147483647 or \
            not isinstance(stdout, bytes) or len(stdout) > MAX_STDOUT_BYTES:
        raise FirmwareDeploymentError("部署工具执行证据无效或输出超过1 MiB")
    stdout_sha = hashlib.sha256(stdout).hexdigest()
    execution_ok = exit_code == 0 and execution_error_type is None
    observed = None
    readback_status = "absent" if not execution_ok and readback_error_type is None else "failed"
    outcome = "failed"
    if result is not None:
        if not execution_ok or not result.verified or result.build_id != plan.get("build_id") or \
                result.backend != plan.get("backend"):
            raise FirmwareDeploymentError("部署成功结果与工具执行或计划不一致")
        expected = plan.get("expected_identity")
        if not isinstance(expected, dict) or asdict(result.expected) != expected:
            raise FirmwareDeploymentError("部署成功结果的预期身份与计划不一致")
        observed = asdict(result.observed)
        if not isinstance(observed.get("device_uuid"), str) or \
                not _UUID.fullmatch(observed["device_uuid"]) or any(
                    observed.get(name) != expected.get(name) for name in (
                        "board_id", "project_sha256", "config_sha256",
                        "firmware_identity_sha256")):
            raise FirmwareDeploymentError("部署回读未通过四重身份和设备UUID核对")
        readback_status = "verified"
        outcome = "verified"
    elif execution_ok and readback_error_type is None:
        raise FirmwareDeploymentError("工具成功但没有回读结果时必须记录回读失败原因")
    terminal = {
        **{key: value for key, value in previous.items() if key not in (
            "sha256", "started_utc", "ended_utc", "execution", "readback",
            "outcome", "hardware_success_claimed", "previous_attempt_sha256")},
        "previous_attempt_sha256": previous["sha256"],
        "started_utc": start, "ended_utc": end,
        "execution": {"status": "succeeded" if execution_ok else "failed",
                      "tool_invoked": True, "exit_code": exit_code,
                      "stdout_byte_count": len(stdout),
                      "stdout_sha256": stdout_sha,
                      "error_type": execution_error_type},
        "readback": {"status": readback_status,
                     "error_type": None if result is not None else readback_error_type,
                     "expected_identity": plan.get("expected_identity"),
                     "observed_identity": observed},
        "outcome": outcome,
        "hardware_success_claimed": outcome == "verified",
    }
    terminal["sha256"] = _digest(terminal)
    return validate_deployment_attempt(terminal)


def validate_deployment_attempt(value: object, plan: object | None = None) -> dict:
    fields = {"format", "schema_version", "attempt_id", "plan_format",
              "plan_sha256", "build_id", "backend", "previous_attempt_sha256",
              "started_utc", "ended_utc", "execution", "readback", "outcome",
              "hardware_success_claimed", "sha256"}
    if not isinstance(value, dict) or set(value) != fields or \
            value.get("format") != FORMAT or value.get("schema_version") != 1 or \
            not isinstance(value.get("attempt_id"), str) or \
            not _ATTEMPT_ID.fullmatch(value["attempt_id"]) or \
            not isinstance(value.get("plan_sha256"), str) or \
            not _HASH.fullmatch(value["plan_sha256"]) or \
            not isinstance(value.get("sha256"), str) or \
            not hmac.compare_digest(value["sha256"], _digest(value)):
        raise FirmwareDeploymentError("部署尝试记录格式或SHA-256无效")
    execution = value.get("execution")
    readback = value.get("readback")
    if not isinstance(execution, dict) or set(execution) != {
            "status", "tool_invoked", "exit_code", "stdout_byte_count",
            "stdout_sha256", "error_type"} or not isinstance(readback, dict) or \
            set(readback) != {"status", "error_type", "expected_identity",
                              "observed_identity"}:
        raise FirmwareDeploymentError("部署尝试执行或回读字段无效")
    expected = readback.get("expected_identity")
    identity_fields = {"board_id", "project_sha256", "config_sha256",
                       "firmware_identity_sha256"}
    if not isinstance(expected, dict) or set(expected) != identity_fields or \
            not isinstance(expected.get("board_id"), str) or any(
                not isinstance(expected.get(name), str) or
                not _HASH.fullmatch(expected[name]) for name in (
                    "project_sha256", "config_sha256",
                    "firmware_identity_sha256")):
        raise FirmwareDeploymentError("部署尝试预期身份无效")
    outcome = value.get("outcome")
    if outcome == "absent":
        if value.get("started_utc") is not None or value.get("ended_utc") is not None or \
                value.get("previous_attempt_sha256") is not None or \
                execution != {"status": "absent", "tool_invoked": False,
                              "exit_code": None, "stdout_byte_count": 0,
                              "stdout_sha256": None, "error_type": None} or \
                readback.get("status") != "absent" or \
                readback.get("observed_identity") is not None or \
                value.get("hardware_success_claimed") is not False:
            raise FirmwareDeploymentError("未执行部署尝试含有执行或成功证据")
    elif outcome in ("failed", "verified"):
        start = _timestamp(value.get("started_utc"), "开始时间")
        end = _timestamp(value.get("ended_utc"), "结束时间")
        if datetime.fromisoformat(end[:-1] + "+00:00") < \
                datetime.fromisoformat(start[:-1] + "+00:00"):
            raise FirmwareDeploymentError("部署尝试结束时间早于开始时间")
        if not isinstance(value.get("previous_attempt_sha256"), str) or \
                not _HASH.fullmatch(value["previous_attempt_sha256"]) or \
                execution.get("tool_invoked") is not True or \
                type(execution.get("exit_code")) is not int or \
                type(execution.get("stdout_byte_count")) is not int or \
                not 0 <= execution["stdout_byte_count"] <= MAX_STDOUT_BYTES or \
                not isinstance(execution.get("stdout_sha256"), str) or \
                not _HASH.fullmatch(execution["stdout_sha256"]):
            raise FirmwareDeploymentError("部署尝试终态执行证据无效")
        for error_type in (execution.get("error_type"),
                           readback.get("error_type")):
            if error_type is not None and (not isinstance(error_type, str) or
                    not _ERROR_TYPE.fullmatch(error_type)):
                raise FirmwareDeploymentError("部署尝试错误类型无效")
        execution_succeeded = execution.get("exit_code") == 0 and \
            execution.get("error_type") is None
        if (execution.get("status") == "succeeded") != execution_succeeded:
            raise FirmwareDeploymentError("部署工具状态、退出码和错误类型不一致")
        verified = outcome == "verified"
        if (readback.get("status") == "verified") != verified or \
                (readback.get("observed_identity") is not None) != verified or \
                value.get("hardware_success_claimed") is not verified:
            raise FirmwareDeploymentError("部署尝试结果与回读状态不一致")
        if verified:
            observed = readback["observed_identity"]
            if execution.get("status") != "succeeded" or \
                    execution.get("exit_code") != 0 or \
                    execution.get("error_type") is not None or \
                    readback.get("error_type") is not None or \
                    not isinstance(observed, dict) or \
                    set(observed) != identity_fields | {"device_uuid"} or \
                    not isinstance(observed.get("device_uuid"), str) or \
                    not _UUID.fullmatch(observed["device_uuid"]) or any(
                        observed.get(name) != expected[name]
                        for name in identity_fields):
                raise FirmwareDeploymentError("verified部署尝试缺少完整工具与身份证据")
        elif readback.get("status") not in ("absent", "failed") or \
                readback.get("observed_identity") is not None or \
                (readback.get("status") == "absent" and
                 readback.get("error_type") is not None) or \
                (readback.get("status") == "failed" and
                 readback.get("error_type") is None) or \
                (execution_succeeded and readback.get("status") != "failed"):
            raise FirmwareDeploymentError("失败部署尝试含有不一致的回读证据")
    else:
        raise FirmwareDeploymentError("部署尝试结果状态无效")
    if plan is not None and (not isinstance(plan, dict) or
            value["plan_sha256"] != plan.get("sha256") or
            value["plan_format"] != plan.get("format") or
            value["build_id"] != plan.get("build_id") or
            value["backend"] != plan.get("backend") or
            readback.get("expected_identity") != plan.get("expected_identity")):
        raise FirmwareDeploymentError("部署尝试与部署计划不一致")
    return value

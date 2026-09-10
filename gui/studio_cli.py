#!/usr/bin/env python3
"""RemoteBSP Studio 非交互命令行；硬件操作必须由显式部署命令发起。"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Sequence, TextIO

from firmware_builder import (
    DEFAULT_BUILD_ROOT,
    DEFAULT_OUTPUT_ROOT,
    FirmwareBuildError,
    build_firmware_project,
)
from firmware_deployment import (
    FirmwareDeploymentError,
    IdentityCapabilityError,
    JsonIdentityFileReader,
    RUNTIME_IDENTITY_CAPABILITIES_MISSING,
    ToolbusdIdentityReader,
    create_can_katapult_deployment_plan,
    create_stlink_deployment_plan,
    create_usb_katapult_deployment_plan,
    deploy_can_katapult,
    deploy_usb_katapult,
    deploy_stlink,
    validate_stlink_deployment_plan,
    validate_can_katapult_deployment_plan,
    validate_usb_katapult_deployment_plan,
)
from deployment_record import create_deployment_record, validate_deployment_record
from deployment_attempt import (
    EXECUTION_CONFIRMATION, create_absent_attempt, execute_deployment_plan,
    validate_deployment_attempt)
from deployment_evidence_bundle import create_bundle, verify_bundle
from device_parameters import (
    DeviceParameterError,
    DeviceParameterManager,
    MAX_BACKUP_BYTES,
    WRITE_CONFIRMATION,
)
from adc_calibration import (
    create_adc_calibration, preflight_adc_calibration,
    validate_adc_calibration,
)
from parameter_audit import ParameterAuditStore, parameter_evidence
from production_batch import (
    MAX_BATCH_COMPARISONS,
    MAX_BATCH_MANIFEST_BYTES,
    MAX_BATCH_RECORDS,
    MAX_PRODUCTION_RECORD_BYTES,
    export_production_batch,
    validate_production_batch_manifest,
)
from production_history import (
    DEFAULT_HISTORY_ROOT,
    MAX_HISTORY_QUERY_CHARS,
    MAX_HISTORY_RESULTS,
    ProductionHistoryStore,
)
from production_signing import (
    MAX_SIGNATURE_BYTES,
    MAX_TRUST_POLICY_BYTES,
    add_trusted_key,
    create_trust_policy,
    generate_key_pair,
    revoke_trusted_key,
    sign_evidence,
    validate_trust_policy,
    verify_evidence_with_policy,
)
from project_compare import MAX_PROJECT_BYTES
from project_config import ProjectConfigError, validate_project


GUI_ROOT = Path(__file__).resolve().parent
DEFAULT_CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
MAX_CATALOG_BYTES = 2 * 1024 * 1024
MAX_COMPARISON_INPUT_BYTES = 128 * 1024
MAX_CLI_PATH_CHARS = 512
MAX_ADC_CALIBRATION_BYTES = 16 * 1024
MAX_DEPLOYMENT_PLAN_BYTES = 64 * 1024
MAX_DEPLOYMENT_ATTEMPT_BYTES = 64 * 1024

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_INPUT = 3
EXIT_OPERATION = 4


class CliUsageError(ValueError):
    pass


def _strict_object(pairs: list[tuple[str, object]]) -> dict:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ProjectConfigError(f"JSON包含重复字段：{key}")
        value[key] = item
    return value


class StrictParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise CliUsageError(message)


def _encode(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, allow_nan=False,
                      sort_keys=True, separators=(",", ":")) + "\n"


def _emit(stream: TextIO, value: object) -> None:
    stream.write(_encode(value))


def _bounded_path(value: object, label: str) -> Path:
    if not isinstance(value, (str, Path)):
        raise ProjectConfigError(f"{label}路径类型无效")
    text = str(value)
    if not text or len(text) > MAX_CLI_PATH_CHARS or "\0" in text:
        raise ProjectConfigError(
            f"{label}路径必须为1至{MAX_CLI_PATH_CHARS}个字符且不能包含NUL")
    path = Path(os.path.abspath(text))
    if len(str(path)) > MAX_CLI_PATH_CHARS:
        raise ProjectConfigError(
            f"{label}解析后的路径超过{MAX_CLI_PATH_CHARS}个字符")
    return path


def _read_json(path_value: object, label: str, maximum: int) -> object:
    path = _bounded_path(path_value, label)
    if path.is_symlink() or not path.is_file():
        raise ProjectConfigError(f"{label}必须是普通JSON文件：{path}")
    with path.open("rb") as stream:
        content = stream.read(maximum + 1)
    if len(content) > maximum:
        raise ProjectConfigError(
            f"{label}超过{maximum // 1024} KiB输入上限")
    try:
        return json.loads(
            content.decode("utf-8"), object_pairs_hook=_strict_object,
            parse_constant=lambda value: (_ for _ in ()).throw(
                ProjectConfigError(f"JSON包含非标准数值：{value}")))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ProjectConfigError(f"{label}不是有效UTF-8 JSON：{error}") from error


def _catalog(path: object) -> dict:
    value = _read_json(path, "板卡目录", MAX_CATALOG_BYTES)
    if not isinstance(value, dict):
        raise ProjectConfigError("板卡目录必须是JSON对象")
    return value


def _project(path: object) -> dict:
    value = _read_json(path, "Studio工程", MAX_PROJECT_BYTES)
    if not isinstance(value, dict):
        raise ProjectConfigError("Studio工程必须是JSON对象")
    return value


def _execution_status(*, software_build: str) -> dict:
    return {
        "software_build": software_build,
        "firmware_flash": "not_performed",
        "hardware_access": False,
        "future_flasher_boundary": (
            "未来烧录器必须使用独立显式命令和适配器；本CLI没有烧录入口。"),
    }


def _validation_payload(result, *, dry_run: bool = False) -> dict:
    return {
        "ok": True,
        "format": "STUDIO_CLI_PROJECT_VALIDATION_V1",
        "dry_run": dry_run,
        "board_id": result.board_id,
        "resource_count": result.resource_count,
        "project_sha256": result.project_sha256,
        "project_schema_version": result.project_schema_version,
        "project_original_schema_version": result.original_schema_version,
        "project_migrations": list(result.migrations),
        "summary": result.summary,
        "execution_status": _execution_status(software_build="not_performed"),
    }


def _prepare_output(path_value: object, *, force: bool) -> Path:
    path = _bounded_path(path_value, "输出文件")
    parent = path.parent
    if not parent.is_dir() or parent.is_symlink():
        raise ProjectConfigError("输出目录必须是已存在的普通目录")
    if (path.exists() or path.is_symlink()) and not force:
        raise ProjectConfigError("输出文件已存在；如需替换请显式使用--force")
    return path


def _atomic_output(path_value: object, content: bytes, *, force: bool) -> Path:
    path = _prepare_output(path_value, force=force)
    parent = path.parent
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        if force:
            os.replace(temporary, path)
        else:
            # 硬链接在同目录内原子地声明最终名称；若另一个进程抢先创建，
            # FileExistsError 会使本次操作失败，不能覆盖竞态中出现的文件。
            os.link(temporary, path)
            temporary.unlink()
        if hasattr(os, "O_DIRECTORY"):
            directory_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
    finally:
        temporary.unlink(missing_ok=True)
    return path


def _run_validate(args) -> dict:
    result = validate_project(
        _project(args.project), _catalog(args.catalog))
    return _validation_payload(result)


def _run_build(args) -> dict:
    project = _project(args.project)
    catalog = _catalog(args.catalog)
    build_root = _bounded_path(args.build_root, "构建目录")
    output_root = _bounded_path(args.output_root, "构建产物目录")
    if args.dry_run:
        result = validate_project(project, catalog)
        response = _validation_payload(result, dry_run=True)
        response.update({
            "format": "STUDIO_CLI_BUILD_V1",
            "planned_jobs": args.jobs,
            "build_output_written": False,
            "note": "dry-run只执行共用工程校验；未运行Kconfig或编译器。",
        })
        return response
    result = build_firmware_project(
        project, catalog, jobs=args.jobs, build_root=build_root,
        output_root=output_root)
    return {
        "ok": True, "format": "STUDIO_CLI_BUILD_V1", "dry_run": False,
        "build_id": result.build_id,
        "board_id": result.board_id,
        "firmware_target": result.firmware_target,
        "config_sha256": result.config_sha256,
        "project_sha256": result.record.get("project_sha256"),
        "output_dir": str(result.output_dir),
        "artifacts": [{
            "filename": item.filename, "size": item.size,
            "sha256": item.sha256,
        } for item in result.artifacts],
        "memory": result.record.get("memory", {}),
        "execution_status": _execution_status(software_build="performed"),
    }


def _run_deploy_stlink(args) -> dict:
    output_root = _bounded_path(args.output_root, "构建产物目录")
    reader = JsonIdentityFileReader(
        _bounded_path(args.identity_file, "设备身份文件"))
    result = deploy_stlink(
        args.build_id, reader,
        output_root=output_root, probe_serial=args.probe_serial,
        flash_timeout=args.flash_timeout,
        reconnect_timeout=args.reconnect_timeout,
        poll_interval=args.poll_interval)
    deployment_record = create_deployment_record(
        result, output_root=output_root)
    record_output = None
    if args.record_output is not None:
        record_output = _atomic_output(
            args.record_output, deployment_record.content, force=args.force)
    return {
        "ok": True, "format": "STUDIO_CLI_STLINK_DEPLOYMENT_V1",
        "build_id": result.build_id, "backend": result.backend,
        "board_id": result.expected.board_id,
        "device_uuid": result.observed.device_uuid,
        "attempts": result.attempts, "verified": result.verified,
        "deployment_record": deployment_record.record,
        "deployment_record_sha256": deployment_record.sha256,
        "deployment_record_filename": deployment_record.filename,
        "deployment_record_output": (
            str(record_output) if record_output is not None else None),
        "execution_status": {
            "software_build": "not_performed",
            "firmware_flash": "performed_and_verified",
            "hardware_access": True,
        },
    }


def _run_deployment_preflight_stlink(args) -> dict:
    artifact = create_stlink_deployment_plan(
        args.build_id,
        output_root=_bounded_path(args.output_root, "构建产物目录"),
        probe_serial=args.probe_serial)
    content = (json.dumps(artifact, ensure_ascii=False, allow_nan=False,
                          sort_keys=True, indent=2) + "\n").encode("utf-8")
    written = _atomic_output(args.plan_output, content, force=args.force)
    return {"ok": True,
            "format": "STUDIO_CLI_STLINK_DEPLOYMENT_PREFLIGHT_V1",
            "plan": artifact, "plan_output": str(written),
            "hardware_access": False, "flash_performed": False}


def _validated_deployment_plan(artifact: object, output_root: Path) -> dict:
    if isinstance(artifact, dict) and artifact.get("format") == \
            "REMOTEBSP_STLINK_DEPLOYMENT_PLAN_V1":
        validated = validate_stlink_deployment_plan(
            artifact, output_root=output_root)
    elif isinstance(artifact, dict) and artifact.get("format") == \
            "REMOTEBSP_USB_KATAPULT_DEPLOYMENT_PLAN_V1":
        validated = validate_usb_katapult_deployment_plan(
            artifact, output_root=output_root)
    elif isinstance(artifact, dict) and artifact.get("format") == \
            "REMOTEBSP_CAN_KATAPULT_DEPLOYMENT_PLAN_V1":
        validated = validate_can_katapult_deployment_plan(
            artifact, output_root=output_root)
    else:
        raise FirmwareDeploymentError("部署计划类型不受支持")
    return validated


def _run_deployment_plan_validate(args) -> dict:
    artifact = _read_json(args.plan, "部署计划", MAX_DEPLOYMENT_PLAN_BYTES)
    validated = _validated_deployment_plan(
        artifact, _bounded_path(args.output_root, "构建产物目录"))
    return {"ok": True,
            "format": "STUDIO_CLI_DEPLOYMENT_PLAN_VALIDATE_V1",
            "build_id": validated["build_id"],
            "backend": validated["backend"],
            "plan_sha256": validated["sha256"],
            "hardware_access": False, "flash_performed": False}


def _run_deployment_attempt_create(args) -> dict:
    plan = _validated_deployment_plan(
        _read_json(args.plan, "部署计划", MAX_DEPLOYMENT_PLAN_BYTES),
        _bounded_path(args.output_root, "构建产物目录"))
    attempt = create_absent_attempt(plan)
    content = (json.dumps(attempt, ensure_ascii=False, allow_nan=False,
                          sort_keys=True, indent=2) + "\n").encode("utf-8")
    written = _atomic_output(args.attempt_output, content, force=args.force)
    return {"ok": True, "format": "STUDIO_CLI_DEPLOYMENT_ATTEMPT_CREATE_V1",
            "attempt": attempt, "attempt_output": str(written),
            "outcome": "absent", "hardware_success_claimed": False}


def _run_deployment_attempt_validate(args) -> dict:
    plan = _validated_deployment_plan(
        _read_json(args.plan, "部署计划", MAX_DEPLOYMENT_PLAN_BYTES),
        _bounded_path(args.output_root, "构建产物目录"))
    attempt = validate_deployment_attempt(
        _read_json(args.attempt, "部署尝试", MAX_DEPLOYMENT_ATTEMPT_BYTES),
        plan)
    return {"ok": True,
            "format": "STUDIO_CLI_DEPLOYMENT_ATTEMPT_VALIDATE_V1",
            "attempt_id": attempt["attempt_id"],
            "outcome": attempt["outcome"],
            "hardware_success_claimed": attempt["hardware_success_claimed"]}


def _run_deployment_bundle_create(args) -> dict:
    return create_bundle(
        plan=_read_json(args.plan, "部署计划", MAX_DEPLOYMENT_PLAN_BYTES),
        attempt=_read_json(args.attempt, "部署尝试", MAX_DEPLOYMENT_ATTEMPT_BYTES),
        output_root=_bounded_path(args.output_root, "构建产物目录"),
        output=_bounded_path(args.output, "部署证据包输出"), force=args.force)


def _run_deployment_bundle_verify(args) -> dict:
    return verify_bundle(_bounded_path(args.bundle, "部署证据包"))


def _run_deployment_execute(args) -> dict:
    plan = _read_json(args.plan, "部署计划", MAX_DEPLOYMENT_PLAN_BYTES)
    reader = JsonIdentityFileReader(
        _bounded_path(args.identity_file, "设备身份文件"))
    now = lambda: datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    attempt = execute_deployment_plan(
        plan, output_root=_bounded_path(args.output_root, "构建产物目录"),
        reader=reader,
        attempt_output=_bounded_path(args.attempt_output, "部署尝试输出"),
        execute=args.execute, confirmation=args.confirmation,
        force=args.force, flash_timeout=args.flash_timeout,
        reconnect_timeout=args.reconnect_timeout,
        poll_interval=args.poll_interval, started_utc=now(),
        ended_utc_provider=now)
    if attempt["outcome"] != "verified":
        raise FirmwareDeploymentError(
            f"部署未通过完整回读核验；failed尝试已原子保存到{args.attempt_output}")
    return {"ok": attempt["outcome"] == "verified",
            "format": "STUDIO_CLI_DEPLOYMENT_EXECUTE_V1",
            "attempt": attempt, "outcome": attempt["outcome"],
            "hardware_success_claimed": attempt["hardware_success_claimed"]}


def _run_deployment_preflight_usb_katapult(args) -> dict:
    artifact = create_usb_katapult_deployment_plan(
        args.build_id,
        output_root=_bounded_path(args.output_root, "构建产物目录"),
        usb_device=args.usb_device,
        flashtool=_bounded_path(args.flashtool, "Katapult flashtool"))
    content = (json.dumps(artifact, ensure_ascii=False, allow_nan=False,
                          sort_keys=True, indent=2) + "\n").encode("utf-8")
    written = _atomic_output(args.plan_output, content, force=args.force)
    return {"ok": True,
            "format": "STUDIO_CLI_USB_KATAPULT_PREFLIGHT_V1",
            "plan": artifact, "plan_output": str(written),
            "hardware_access": False, "flash_performed": False}


def _run_deployment_preflight_can_katapult(args) -> dict:
    artifact = create_can_katapult_deployment_plan(
        args.build_id,
        output_root=_bounded_path(args.output_root, "构建产物目录"),
        can_interface=args.can_interface, katapult_uuid=args.katapult_uuid,
        flashtool=_bounded_path(args.flashtool, "Katapult flashtool"))
    content = (json.dumps(artifact, ensure_ascii=False, allow_nan=False,
                          sort_keys=True, indent=2) + "\n").encode("utf-8")
    written = _atomic_output(args.plan_output, content, force=args.force)
    return {"ok": True,
            "format": "STUDIO_CLI_CAN_KATAPULT_PREFLIGHT_V1",
            "plan": artifact, "plan_output": str(written),
            "hardware_access": False, "flash_performed": False}


def _run_deploy_can_katapult(args) -> dict:
    output_root = _bounded_path(args.output_root, "构建产物目录")
    reader = JsonIdentityFileReader(
        _bounded_path(args.identity_file, "设备身份文件"))
    result = deploy_can_katapult(
        args.build_id, reader, output_root=output_root,
        can_interface=args.can_interface, katapult_uuid=args.katapult_uuid,
        flashtool=_bounded_path(args.flashtool, "Katapult flashtool"),
        flash_timeout=args.flash_timeout,
        reconnect_timeout=args.reconnect_timeout,
        poll_interval=args.poll_interval)
    deployment_record = create_deployment_record(result, output_root=output_root)
    record_output = None
    if args.record_output is not None:
        record_output = _atomic_output(
            args.record_output, deployment_record.content, force=args.force)
    return {
        "ok": True, "format": "STUDIO_CLI_CAN_KATAPULT_DEPLOYMENT_V1",
        "build_id": result.build_id, "backend": result.backend,
        "board_id": result.expected.board_id,
        "device_uuid": result.observed.device_uuid,
        "attempts": result.attempts, "verified": result.verified,
        "deployment_record": deployment_record.record,
        "deployment_record_sha256": deployment_record.sha256,
        "deployment_record_filename": deployment_record.filename,
        "deployment_record_output": str(record_output) if record_output else None,
        "execution_status": {"software_build": "not_performed",
                             "firmware_flash": "performed_and_verified",
                             "hardware_access": True},
    }


def _run_deploy_usb_katapult(args) -> dict:
    output_root = _bounded_path(args.output_root, "固件输出目录")
    reader = JsonIdentityFileReader(_bounded_path(
        args.identity_file, "设备身份文件"))
    result = deploy_usb_katapult(
        args.build_id, reader, output_root=output_root,
        usb_device=args.usb_device,
        flashtool=_bounded_path(args.flashtool, "Katapult flashtool"),
        flash_timeout=args.flash_timeout,
        reconnect_timeout=args.reconnect_timeout,
        poll_interval=args.poll_interval)
    deployment_record = create_deployment_record(result, output_root=output_root)
    record_output = None
    if args.record_output:
        record_output = _atomic_output(_prepare_output(
            args.record_output, force=args.force), deployment_record.content,
            force=args.force)
    return {
        "ok": True, "format": "STUDIO_CLI_USB_KATAPULT_DEPLOYMENT_V1",
        "status": "performed_and_verified", "backend": result.backend,
        "build_id": result.build_id, "board_id": result.expected.board_id,
        "device_uuid": result.observed.device_uuid, "attempts": result.attempts,
        "verified": result.verified,
        "deployment_record": deployment_record.record,
        "deployment_record_sha256": deployment_record.sha256,
        "deployment_record_filename": deployment_record.filename,
        "deployment_record_output": str(record_output) if record_output else None,
        "execution_status": {"software_build": "not_performed",
                             "firmware_flash": "performed_and_verified",
                             "hardware_access": True},
    }


def _run_inspect_runtime_identity(args) -> dict:
    reader = ToolbusdIdentityReader(
        args.toolbusd_socket, args.node_id,
        expected_uuid=args.node_uuid, remote_cli=args.remote_cli,
        timeout=args.identity_timeout)
    try:
        identity = reader.read_identity()
        node = reader.read_runtime_node()
        if node.device_uuid != identity.device_uuid or \
                node.board_id != identity.board_id:
            raise FirmwareDeploymentError("身份检查期间运行中节点发生变化")
        identity_complete = True
        missing = []
    except IdentityCapabilityError as error:
        node = reader.read_runtime_node()
        identity = None
        identity_complete = False
        missing = list(error.missing_fields or
                       RUNTIME_IDENTITY_CAPABILITIES_MISSING)
    return {
        "ok": True, "format": "STUDIO_CLI_RUNTIME_IDENTITY_V1",
        "identity_complete": identity_complete,
        "node_id": node.node_id, "board_id": node.board_id,
        "device_uuid": node.device_uuid, "online": node.online,
        "ready": node.ready,
        "firmware": {
            "major": node.firmware_version[0],
            "minor": node.firmware_version[1],
            "patch": node.firmware_version[2],
        },
        "protocol_version": node.protocol_version,
        "project_sha256": (
            identity.project_sha256 if identity is not None else None),
        "config_sha256": (
            identity.config_sha256 if identity is not None else None),
        "firmware_identity_sha256": (
            identity.firmware_identity_sha256
            if identity is not None else None),
        "capabilities_missing": missing,
        "deployment_verified": False,
        "execution_status": _execution_status(software_build="not_performed"),
    }


def _parameter_manager(args) -> DeviceParameterManager:
    return DeviceParameterManager(
        args.toolbusd_socket, args.node_id,
        remote_cli=args.remote_cli, timeout=args.parameter_timeout)


def _parameter_audit(args) -> ParameterAuditStore:
    return ParameterAuditStore(
        _bounded_path(args.audit_dir, "参数审计目录"),
        _bounded_path(args.audit_key_file, "参数审计密钥"))


def _finish_parameter_audit(audit: ParameterAuditStore, operation_id: str,
                            error: Exception) -> None:
    match = re.search(r"完成(\d+)项后中止", str(error))
    applied = int(match.group(1)) if match else 0
    audit.finish(
        operation_id,
        outcome="partial_failure" if applied else "failure",
        generation_after=None, applied_count=applied,
        error_type=type(error).__name__)


def _run_parameter_write(args) -> dict:
    try:
        value = base64.b64decode(args.value_base64, validate=True)
    except (TypeError, ValueError) as error:
        raise ProjectConfigError("--value-base64不是合法Base64") from error
    # 规范化后再交给同一有界后端，避免宽松编码存在多种表示。
    audit = _parameter_audit(args)
    operation_id = audit.begin(
        operation="write", node_id=args.node_id,
        node_uuid=args.expected_uuid,
        expected_generation=args.expected_generation,
        parameters=[parameter_evidence(args.parameter_id, value)])
    try:
        snapshot = _parameter_manager(args).write(
            expected_uuid=args.expected_uuid,
            expected_generation=args.expected_generation,
            parameter_id=args.parameter_id,
            value_base64=base64.b64encode(value).decode("ascii"),
            confirmation=args.confirmation)
    except Exception as error:
        _finish_parameter_audit(audit, operation_id, error)
        raise
    audit.finish(operation_id, outcome="success",
                 generation_after=snapshot["status"]["generation"],
                 applied_count=(1 if snapshot["status"]["generation"] !=
                                args.expected_generation else 0))
    return {
        "ok": True, "format": "STUDIO_CLI_DEVICE_PARAMETER_WRITE_V1",
        "snapshot": snapshot,
        "audit_operation_id": operation_id,
        "execution_status": {
            "software_build": "not_performed", "firmware_flash": "not_performed",
            "hardware_access": True, "device_parameters_written": True,
        },
    }


def _run_parameter_restore(args) -> dict:
    backup = _read_json(args.backup, "设备参数备份", MAX_BACKUP_BYTES)
    if not isinstance(backup, dict) or not isinstance(
            backup.get("parameters"), list):
        raise ProjectConfigError("设备参数备份缺少参数数组")
    evidence = []
    for item in backup["parameters"]:
        if not isinstance(item, dict) or type(item.get("id")) is not int:
            raise ProjectConfigError("设备参数备份参数ID无效")
        try:
            raw = base64.b64decode(item.get("value_base64"), validate=True)
        except (TypeError, ValueError) as error:
            raise ProjectConfigError("设备参数备份值不是合法Base64") from error
        evidence.append(parameter_evidence(item["id"], raw))
    audit = _parameter_audit(args)
    operation_id = audit.begin(
        operation="restore", node_id=args.node_id,
        node_uuid=args.expected_uuid,
        expected_generation=args.expected_generation,
        parameters=evidence)
    try:
        snapshot = _parameter_manager(args).restore(
            backup, expected_uuid=args.expected_uuid,
            expected_generation=args.expected_generation,
            confirmation=args.confirmation)
    except Exception as error:
        _finish_parameter_audit(audit, operation_id, error)
        raise
    applied = snapshot["status"]["generation"] - args.expected_generation
    audit.finish(operation_id, outcome="success",
                 generation_after=snapshot["status"]["generation"],
                 applied_count=applied)
    return {
        "ok": True, "format": "STUDIO_CLI_DEVICE_PARAMETER_RESTORE_V1",
        "snapshot": snapshot,
        "audit_operation_id": operation_id,
        "execution_status": {
            "software_build": "not_performed", "firmware_flash": "not_performed",
            "hardware_access": True, "device_parameters_written": True,
        },
    }


def _run_adc_calibration_create(args) -> dict:
    specification = _read_json(
        args.specification, "ADC校准规格", MAX_ADC_CALIBRATION_BYTES)
    artifact = create_adc_calibration(specification)
    content = (json.dumps(artifact, ensure_ascii=False, allow_nan=False,
                          sort_keys=True, indent=2) + "\n").encode("utf-8")
    written = _atomic_output(args.output, content, force=args.force)
    return {"ok": True, "format": "STUDIO_CLI_ADC_CALIBRATION_CREATE_V1",
            "artifact": artifact, "output": str(written),
            "hardware_access": False, "sampling_performed": False}


def _run_adc_calibration_validate(args) -> dict:
    artifact = validate_adc_calibration(_read_json(
        args.calibration, "ADC校准工件", MAX_ADC_CALIBRATION_BYTES))
    return {"ok": True, "format": "STUDIO_CLI_ADC_CALIBRATION_VALIDATE_V1",
            "parameter_id": artifact["parameter_id"],
            "value_crc32": artifact["value_crc32"],
            "artifact_sha256": artifact["sha256"],
            "hardware_access": False, "sampling_performed": False}


def _run_adc_calibration_preflight(args) -> dict:
    artifact = _read_json(
        args.calibration, "ADC校准工件", MAX_ADC_CALIBRATION_BYTES)
    backup = _read_json(args.backup, "设备参数备份", MAX_BACKUP_BYTES)
    evidence = (_read_json(args.resource_evidence, "ADC资源合同证据",
                           MAX_ADC_CALIBRATION_BYTES)
                if args.resource_evidence else None)
    return {"ok": True, **preflight_adc_calibration(
        artifact, backup, evidence)}


def _run_batch_create(args) -> dict:
    record_paths = args.production_record or []
    comparison_paths = args.comparison or []
    if not 1 <= len(record_paths) <= MAX_BATCH_RECORDS:
        raise ProjectConfigError(
            f"生产记录文件数必须位于1至{MAX_BATCH_RECORDS}")
    if len(comparison_paths) > MAX_BATCH_COMPARISONS:
        raise ProjectConfigError(
            f"工程差异文件不能超过{MAX_BATCH_COMPARISONS}份")
    if args.force and not args.archive_output:
        raise ProjectConfigError("--force只能与--archive-output一起使用")
    planned_output = _prepare_output(
        args.archive_output, force=args.force) if args.archive_output else None
    records = [_read_json(path, f"生产记录[{index}]",
                          MAX_PRODUCTION_RECORD_BYTES)
               for index, path in enumerate(record_paths)]
    comparisons = [_read_json(path, f"工程差异[{index}]",
                              MAX_COMPARISON_INPUT_BYTES)
                   for index, path in enumerate(comparison_paths)]
    result = export_production_batch(
        batch_id=args.batch_id, name=args.name, note=args.note,
        production_records=records, comparison_exports=comparisons)
    output_path = None
    if planned_output is not None and not args.dry_run:
        output_path = _atomic_output(
            planned_output, result.archive, force=args.force)
    return {
        "ok": True, "format": "STUDIO_CLI_BATCH_CREATE_V1",
        "dry_run": args.dry_run,
        "manifest_sha256": result.manifest["manifest_sha256"],
        "manifest": result.manifest,
        "archive_filename": result.archive_filename,
        "archive_sha256": result.archive_sha256,
        "archive_byte_count": len(result.archive),
        "archive_written": output_path is not None,
        "archive_output": str(output_path or planned_output)
            if output_path or planned_output else None,
        "execution_status": _execution_status(software_build="not_performed"),
    }


def _run_batch_validate(args) -> dict:
    manifest = _read_json(
        args.manifest, "生产批次清单", MAX_BATCH_MANIFEST_BYTES)
    validation = validate_production_batch_manifest(manifest)
    if not validation["valid"]:
        raise ProjectConfigError(validation["status_text"])
    return {
        "ok": True, "format": "STUDIO_CLI_BATCH_VALIDATION_V1",
        "validation": validation,
        "execution_status": _execution_status(software_build="not_performed"),
    }


def _validated_signable(path: object) -> dict:
    value = _read_json(path, "待签名生产证据", MAX_BATCH_MANIFEST_BYTES)
    if value.get("format") == "REMOTEBSP_PRODUCTION_BATCH_V1":
        validation = validate_production_batch_manifest(value)
        if not validation["valid"]:
            raise ProjectConfigError(validation["status_text"])
    elif value.get("format") == "REMOTEBSP_DEPLOYMENT_RECORD_V1":
        validate_deployment_record(value)
    else:
        raise ProjectConfigError("只允许签名生产批次清单或部署记录")
    return value


def _run_signing_keygen(args) -> dict:
    private_path = _bounded_path(args.private_key_output, "Ed25519私钥输出")
    public_path = _bounded_path(args.public_key_output, "Ed25519公钥输出")
    if private_path.resolve() == public_path.resolve():
        raise ProjectConfigError("公钥与私钥输出不能是同一路径")
    result = generate_key_pair(private_path, public_path)
    return {"ok": True, "format": "STUDIO_CLI_SIGNING_KEYGEN_V1", **result,
            "time_authority": "none"}


def _run_evidence_sign(args) -> dict:
    evidence = _validated_signable(args.evidence)
    envelope = sign_evidence(
        evidence, _bounded_path(args.private_key, "Ed25519私钥"))
    content = (json.dumps(envelope, ensure_ascii=False, allow_nan=False,
                          sort_keys=True, indent=2) + "\n").encode("utf-8")
    output = _prepare_output(args.signature_output, force=args.force)
    written = _atomic_output(output, content, force=args.force)
    return {"ok": True, "format": "STUDIO_CLI_EVIDENCE_SIGN_V1",
            "signature": envelope, "signature_output": str(written),
            "time_trusted": False}


def _run_evidence_verify(args) -> dict:
    evidence = _validated_signable(args.evidence)
    envelope = _read_json(args.signature, "Ed25519签名信封",
                          MAX_SIGNATURE_BYTES)
    policy = _read_json(args.trust_policy, "签名者信任策略",
                        MAX_TRUST_POLICY_BYTES)
    verification = verify_evidence_with_policy(evidence, envelope, policy)
    return {"ok": True, "format": "STUDIO_CLI_EVIDENCE_VERIFY_V1",
            "verification": verification}


def _policy_content(policy: object) -> bytes:
    validated = validate_trust_policy(policy)
    return (json.dumps(validated, ensure_ascii=False, allow_nan=False,
                       sort_keys=True, indent=2) + "\n").encode("utf-8")


def _run_signing_policy_create(args) -> dict:
    policy = create_trust_policy(
        [_bounded_path(path, "Ed25519公钥") for path in args.public_key],
        authorized_kinds=tuple(args.authorized_kind or (
            "production_batch_manifest", "deployment_record")))
    written = _atomic_output(args.policy_output, _policy_content(policy),
                             force=args.force)
    return {"ok": True, "format": "STUDIO_CLI_SIGNING_POLICY_V1",
            "operation": "create", "policy_output": str(written),
            "key_count": len(policy["keys"]), "time_authority": "none"}


def _run_signing_policy_add(args) -> dict:
    policy = _read_json(args.policy, "签名者信任策略",
                        MAX_TRUST_POLICY_BYTES)
    updated = add_trusted_key(
        policy, _bounded_path(args.public_key, "Ed25519公钥"),
        authorized_kinds=tuple(args.authorized_kind or (
            "production_batch_manifest", "deployment_record")))
    written = _atomic_output(args.policy_output, _policy_content(updated),
                             force=args.force)
    return {"ok": True, "format": "STUDIO_CLI_SIGNING_POLICY_V1",
            "operation": "add", "policy_output": str(written),
            "key_count": len(updated["keys"]), "time_authority": "none"}


def _run_signing_policy_revoke(args) -> dict:
    policy = _read_json(args.policy, "签名者信任策略",
                        MAX_TRUST_POLICY_BYTES)
    updated = revoke_trusted_key(policy, args.key_id)
    written = _atomic_output(args.policy_output, _policy_content(updated),
                             force=args.force)
    return {"ok": True, "format": "STUDIO_CLI_SIGNING_POLICY_V1",
            "operation": "revoke", "policy_output": str(written),
            "key_id": args.key_id, "time_authority": "none"}


def _run_history_save(args) -> dict:
    manifest = _read_json(
        args.manifest, "生产批次清单", MAX_BATCH_MANIFEST_BYTES)
    validation = validate_production_batch_manifest(manifest)
    if not validation["valid"]:
        raise ProjectConfigError(validation["status_text"])
    root = _bounded_path(args.history_root, "本地历史目录")
    if args.dry_run:
        return {
            "ok": True, "format": "STUDIO_CLI_HISTORY_SAVE_V1",
            "dry_run": True, "stored": False,
            "manifest_sha256": validation["manifest_sha256"],
            "history_directory_created": False,
            "execution_status": _execution_status(
                software_build="not_performed"),
        }
    saved = ProductionHistoryStore(root).save(manifest)
    return {
        "ok": True, "format": "STUDIO_CLI_HISTORY_SAVE_V1",
        "dry_run": False, **saved,
        "execution_status": _execution_status(software_build="not_performed"),
    }


def _run_history_search(args) -> dict:
    if len(args.query) > MAX_HISTORY_QUERY_CHARS:
        raise ProjectConfigError(
            f"历史搜索词不能超过{MAX_HISTORY_QUERY_CHARS}个字符")
    root = _bounded_path(args.history_root, "本地历史目录")
    marker = root / ".remotebsp-production-history-v1"
    if not root.is_dir() or not marker.exists():
        raise ProjectConfigError(
            "本地历史尚不存在；请先用history-save显式保存批次")
    return ProductionHistoryStore(root).search(
        args.query, args.field, args.limit)


def _parser() -> StrictParser:
    parser = StrictParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True,
                                parser_class=StrictParser)

    validate = sub.add_parser("project-validate", help="校验Studio工程")
    validate.add_argument("--project", required=True)
    validate.add_argument("--catalog", default=str(DEFAULT_CATALOG_PATH))
    validate.set_defaults(handler=_run_validate)

    build = sub.add_parser("build", help="显式执行软件固件构建")
    build.add_argument("--project", required=True)
    build.add_argument("--catalog", default=str(DEFAULT_CATALOG_PATH))
    build.add_argument("--jobs", type=int, choices=range(1, 65), default=32)
    build.add_argument("--build-root", default=str(DEFAULT_BUILD_ROOT))
    build.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    build.add_argument("--dry-run", action="store_true")
    build.set_defaults(handler=_run_build)

    deployment_preflight = sub.add_parser(
        "deployment-preflight-stlink",
        help="离线生成受保护构建对应的ST-Link烧录计划，不访问硬件")
    deployment_preflight.add_argument("--build-id", required=True)
    deployment_preflight.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    deployment_preflight.add_argument("--probe-serial")
    deployment_preflight.add_argument("--plan-output", required=True)
    deployment_preflight.add_argument("--force", action="store_true")
    deployment_preflight.set_defaults(handler=_run_deployment_preflight_stlink)

    usb_deployment_preflight = sub.add_parser(
        "deployment-preflight-usb-katapult",
        help="离线生成独立USB Katapult恢复阶段计划，不打开设备")
    usb_deployment_preflight.add_argument("--build-id", required=True)
    usb_deployment_preflight.add_argument(
        "--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    usb_deployment_preflight.add_argument("--usb-device", required=True)
    usb_deployment_preflight.add_argument("--flashtool", required=True)
    usb_deployment_preflight.add_argument("--plan-output", required=True)
    usb_deployment_preflight.add_argument("--force", action="store_true")
    usb_deployment_preflight.set_defaults(
        handler=_run_deployment_preflight_usb_katapult)

    can_deployment_preflight = sub.add_parser(
        "deployment-preflight-can-katapult",
        help="离线生成定向CAN Katapult恢复阶段计划，不打开CAN")
    can_deployment_preflight.add_argument("--build-id", required=True)
    can_deployment_preflight.add_argument(
        "--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    can_deployment_preflight.add_argument("--can-interface", required=True)
    can_deployment_preflight.add_argument("--katapult-uuid", required=True)
    can_deployment_preflight.add_argument("--flashtool", required=True)
    can_deployment_preflight.add_argument("--plan-output", required=True)
    can_deployment_preflight.add_argument("--force", action="store_true")
    can_deployment_preflight.set_defaults(
        handler=_run_deployment_preflight_can_katapult)

    deployment_validate = sub.add_parser(
        "deployment-plan-validate",
        help="离线复核ST-Link、CAN或USB Katapult计划，拒绝产物漂移")
    deployment_validate.add_argument("--plan", required=True)
    deployment_validate.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    deployment_validate.set_defaults(handler=_run_deployment_plan_validate)

    attempt_create = sub.add_parser(
        "deployment-attempt-create",
        help="从已验证计划创建未执行部署尝试记录，不访问硬件")
    attempt_create.add_argument("--plan", required=True)
    attempt_create.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    attempt_create.add_argument("--attempt-output", required=True)
    attempt_create.add_argument("--force", action="store_true")
    attempt_create.set_defaults(handler=_run_deployment_attempt_create)

    attempt_validate = sub.add_parser(
        "deployment-attempt-validate",
        help="严格验证部署尝试状态及其计划绑定")
    attempt_validate.add_argument("--plan", required=True)
    attempt_validate.add_argument("--attempt", required=True)
    attempt_validate.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    attempt_validate.set_defaults(handler=_run_deployment_attempt_validate)

    bundle_create = sub.add_parser(
        "deployment-evidence-bundle-create",
        help="确定性打包已严格验证的部署软件证据，不访问硬件")
    bundle_create.add_argument("--plan", required=True)
    bundle_create.add_argument("--attempt", required=True)
    bundle_create.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    bundle_create.add_argument("--output", required=True)
    bundle_create.add_argument("--force", action="store_true")
    bundle_create.set_defaults(handler=_run_deployment_bundle_create)

    bundle_verify = sub.add_parser(
        "deployment-evidence-bundle-verify",
        help="完全离线验证部署证据ZIP和终态，不访问硬件")
    bundle_verify.add_argument("--bundle", required=True)
    bundle_verify.set_defaults(handler=_run_deployment_bundle_verify)

    deployment_execute = sub.add_parser(
        "deployment-execute",
        help="显式执行已验证部署计划并原子保存尝试证据")
    deployment_execute.add_argument("--plan", required=True)
    deployment_execute.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    deployment_execute.add_argument("--identity-file", required=True)
    deployment_execute.add_argument("--attempt-output", required=True)
    deployment_execute.add_argument("--execute", action="store_true")
    deployment_execute.add_argument("--confirmation", required=True,
                                    help=f"必须精确填写{EXECUTION_CONFIRMATION}")
    deployment_execute.add_argument("--flash-timeout", type=int, default=120)
    deployment_execute.add_argument("--reconnect-timeout", type=float, default=10.0)
    deployment_execute.add_argument("--poll-interval", type=float, default=0.25)
    deployment_execute.add_argument("--force", action="store_true")
    deployment_execute.set_defaults(handler=_run_deployment_execute)

    deploy = sub.add_parser(
        "deploy-stlink", help="显式通过ST-Link烧录并核对运行中身份")
    deploy.add_argument("--build-id", required=True)
    deploy.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    deploy.add_argument("--identity-file", required=True,
                        help="由本机上位机原子更新的设备身份JSON文件")
    deploy.add_argument("--probe-serial")
    deploy.add_argument("--flash-timeout", type=int, default=120)
    deploy.add_argument("--reconnect-timeout", type=float, default=10.0)
    deploy.add_argument("--poll-interval", type=float, default=0.25)
    deploy.add_argument("--record-output",
                        help="完整核验成功后原子写入版本化部署记录")
    deploy.add_argument("--force", action="store_true")
    deploy.set_defaults(handler=_run_deploy_stlink)

    katapult = sub.add_parser(
        "deploy-can-katapult", help="显式通过CAN Katapult定向升级并核对身份")
    katapult.add_argument("--build-id", required=True)
    katapult.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    katapult.add_argument("--identity-file", required=True)
    katapult.add_argument("--can-interface", required=True)
    katapult.add_argument("--katapult-uuid", required=True)
    katapult.add_argument("--flashtool", required=True)
    katapult.add_argument("--flash-timeout", type=int, default=120)
    katapult.add_argument("--reconnect-timeout", type=float, default=10.0)
    katapult.add_argument("--poll-interval", type=float, default=0.25)
    katapult.add_argument("--record-output")
    katapult.add_argument("--force", action="store_true")
    katapult.set_defaults(handler=_run_deploy_can_katapult)

    usb_katapult = sub.add_parser(
        "deploy-usb-katapult",
        help="显式通过独立USB Katapult恢复阶段定向升级并核对身份")
    usb_katapult.add_argument("--build-id", required=True)
    usb_katapult.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    usb_katapult.add_argument("--identity-file", required=True)
    usb_katapult.add_argument("--usb-device", required=True,
                              help="固定/dev/serial/by-id设备")
    usb_katapult.add_argument("--flashtool", required=True)
    usb_katapult.add_argument("--flash-timeout", type=int, default=120)
    usb_katapult.add_argument("--reconnect-timeout", type=float, default=10.0)
    usb_katapult.add_argument("--poll-interval", type=float, default=0.25)
    usb_katapult.add_argument("--record-output")
    usb_katapult.add_argument("--force", action="store_true")
    usb_katapult.set_defaults(handler=_run_deploy_usb_katapult)

    inspect_identity = sub.add_parser(
        "inspect-runtime-identity",
        help="只读检查toolbusd当前公开的节点身份子集")
    inspect_identity.add_argument("--toolbusd-socket", required=True)
    inspect_identity.add_argument("--node-id", type=int, default=1)
    inspect_identity.add_argument("--node-uuid")
    inspect_identity.add_argument("--remote-cli", default="remote-cli")
    inspect_identity.add_argument(
        "--identity-timeout", type=float, default=2.0)
    inspect_identity.set_defaults(handler=_run_inspect_runtime_identity)

    def add_parameter_target(command):
        command.add_argument("--toolbusd-socket", required=True)
        command.add_argument("--node-id", type=int, default=1)
        command.add_argument("--expected-uuid", required=True)
        command.add_argument("--expected-generation", type=int, required=True)
        command.add_argument("--confirmation", required=True,
                             help=f"必须精确填写{WRITE_CONFIRMATION}")
        command.add_argument("--remote-cli", default="remote-cli")
        command.add_argument("--parameter-timeout", type=float, default=3.0)
        command.add_argument("--audit-dir", required=True,
                             help="本地参数审计记录目录")
        command.add_argument("--audit-key-file", required=True,
                             help="至少32字节的本地参数审计HMAC密钥文件")

    parameter_write = sub.add_parser(
        "device-parameter-write", help="显式写入单项设备参数")
    add_parameter_target(parameter_write)
    parameter_write.add_argument("--parameter-id", type=lambda value: int(value, 0),
                                 required=True)
    parameter_write.add_argument("--value-base64", required=True)
    parameter_write.set_defaults(handler=_run_parameter_write)

    parameter_restore = sub.add_parser(
        "device-parameter-restore", help="显式恢复设备参数备份")
    add_parameter_target(parameter_restore)
    parameter_restore.add_argument("--backup", required=True)
    parameter_restore.set_defaults(handler=_run_parameter_restore)

    calibration_create = sub.add_parser(
        "adc-calibration-create", help="从生产规格生成ADC校准数据工件（不采样）")
    calibration_create.add_argument("--specification", required=True)
    calibration_create.add_argument("--output", required=True)
    calibration_create.add_argument("--force", action="store_true")
    calibration_create.set_defaults(handler=_run_adc_calibration_create)

    calibration_validate = sub.add_parser(
        "adc-calibration-validate", help="离线验证ADC校准工件")
    calibration_validate.add_argument("--calibration", required=True)
    calibration_validate.set_defaults(handler=_run_adc_calibration_validate)

    calibration_preflight = sub.add_parser(
        "adc-calibration-preflight",
        help="将ADC校准工件与参数备份绑定并生成写入前预检")
    calibration_preflight.add_argument("--calibration", required=True)
    calibration_preflight.add_argument("--backup", required=True)
    calibration_preflight.add_argument(
        "--resource-evidence",
        help="可选：已由上游核验并绑定运行节点UUID的ADC资源合同证据")
    calibration_preflight.set_defaults(handler=_run_adc_calibration_preflight)

    create = sub.add_parser("batch-create", help="生成确定性生产批次")
    create.add_argument("--batch-id", required=True)
    create.add_argument("--name", required=True)
    create.add_argument("--note", default="")
    create.add_argument("--production-record", action="append", required=True)
    create.add_argument("--comparison", action="append")
    create.add_argument("--archive-output")
    create.add_argument("--force", action="store_true")
    create.add_argument("--dry-run", action="store_true")
    create.set_defaults(handler=_run_batch_create)

    batch_validate = sub.add_parser("batch-validate", help="校验现有生产批次")
    batch_validate.add_argument("--manifest", required=True)
    batch_validate.set_defaults(handler=_run_batch_validate)

    keygen = sub.add_parser(
        "signing-keygen", help="生成离线Ed25519签名密钥对")
    keygen.add_argument("--private-key-output", required=True)
    keygen.add_argument("--public-key-output", required=True)
    keygen.set_defaults(handler=_run_signing_keygen)

    evidence_sign = sub.add_parser(
        "evidence-sign", help="离线签名生产批次清单或部署记录")
    evidence_sign.add_argument("--evidence", required=True)
    evidence_sign.add_argument("--private-key", required=True)
    evidence_sign.add_argument("--signature-output", required=True)
    evidence_sign.add_argument("--force", action="store_true")
    evidence_sign.set_defaults(handler=_run_evidence_sign)

    policy_kinds = ("production_batch_manifest", "deployment_record")
    policy_create = sub.add_parser(
        "signing-policy-create", help="创建版本化生产签名者信任策略")
    policy_create.add_argument("--public-key", action="append", required=True)
    policy_create.add_argument("--authorized-kind", action="append",
                               choices=policy_kinds)
    policy_create.add_argument("--policy-output", required=True)
    policy_create.add_argument("--force", action="store_true")
    policy_create.set_defaults(handler=_run_signing_policy_create)

    policy_add = sub.add_parser(
        "signing-policy-add", help="向信任策略加入轮换公钥")
    policy_add.add_argument("--policy", required=True)
    policy_add.add_argument("--public-key", required=True)
    policy_add.add_argument("--authorized-kind", action="append",
                            choices=policy_kinds)
    policy_add.add_argument("--policy-output", required=True)
    policy_add.add_argument("--force", action="store_true")
    policy_add.set_defaults(handler=_run_signing_policy_add)

    policy_revoke = sub.add_parser(
        "signing-policy-revoke", help="在信任策略中明确撤销公钥")
    policy_revoke.add_argument("--policy", required=True)
    policy_revoke.add_argument("--key-id", required=True)
    policy_revoke.add_argument("--policy-output", required=True)
    policy_revoke.add_argument("--force", action="store_true")
    policy_revoke.set_defaults(handler=_run_signing_policy_revoke)

    evidence_verify = sub.add_parser(
        "evidence-verify", help="按签名者信任策略验证生产证据")
    evidence_verify.add_argument("--evidence", required=True)
    evidence_verify.add_argument("--signature", required=True)
    evidence_verify.add_argument("--trust-policy", required=True)
    evidence_verify.set_defaults(handler=_run_evidence_verify)

    save = sub.add_parser("history-save", help="保存已校验批次到本地历史")
    save.add_argument("--manifest", required=True)
    save.add_argument("--history-root", default=str(DEFAULT_HISTORY_ROOT))
    save.add_argument("--dry-run", action="store_true")
    save.set_defaults(handler=_run_history_save)

    search = sub.add_parser("history-search", help="检索本地生产批次历史")
    search.add_argument("--history-root", default=str(DEFAULT_HISTORY_ROOT))
    search.add_argument("--query", default="")
    search.add_argument("--field", choices=(
        "all", "project_sha256", "build_id", "board_id", "record_sha256"),
        default="all")
    search.add_argument("--limit", type=int, choices=range(
        1, MAX_HISTORY_RESULTS + 1), default=MAX_HISTORY_RESULTS)
    search.set_defaults(handler=_run_history_search)
    return parser


def main(argv: Sequence[str] | None = None, *, stdout: TextIO = sys.stdout,
         stderr: TextIO = sys.stderr) -> int:
    try:
        args = _parser().parse_args(argv)
        response = args.handler(args)
        _emit(stdout, response)
        return EXIT_OK
    except CliUsageError as error:
        _emit(stderr, {"ok": False, "format": "STUDIO_CLI_ERROR_V1",
                       "exit_code": EXIT_USAGE, "error": str(error)})
        return EXIT_USAGE
    except (ProjectConfigError, json.JSONDecodeError,
            UnicodeDecodeError) as error:
        _emit(stderr, {"ok": False, "format": "STUDIO_CLI_ERROR_V1",
                       "exit_code": EXIT_INPUT, "error": str(error)})
        return EXIT_INPUT
    except (FirmwareBuildError, FirmwareDeploymentError,
            DeviceParameterError, OSError) as error:
        _emit(stderr, {"ok": False, "format": "STUDIO_CLI_ERROR_V1",
                       "exit_code": EXIT_OPERATION, "error": str(error)})
        return EXIT_OPERATION


if __name__ == "__main__":
    raise SystemExit(main())

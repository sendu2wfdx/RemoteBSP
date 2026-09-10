#!/usr/bin/env python3
"""确定性部署证据 ZIP；只打包已验证的软件证据，不访问硬件。"""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

from deployment_attempt import validate_deployment_attempt
from firmware_builder import resolve_artifact
from firmware_deployment import (
    FirmwareDeploymentError, validate_can_katapult_deployment_plan,
    validate_stlink_deployment_plan, validate_usb_katapult_deployment_plan)

FORMAT = "REMOTEBSP_DEPLOYMENT_EVIDENCE_BUNDLE_V1"
MAX_BUNDLE_BYTES = 32 * 1024 * 1024
_VALIDATORS = {
    "REMOTEBSP_STLINK_DEPLOYMENT_PLAN_V1": validate_stlink_deployment_plan,
    "REMOTEBSP_CAN_KATAPULT_DEPLOYMENT_PLAN_V1": validate_can_katapult_deployment_plan,
    "REMOTEBSP_USB_KATAPULT_DEPLOYMENT_PLAN_V1": validate_usb_katapult_deployment_plan}


def _json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, allow_nan=False,
                       sort_keys=True, indent=2) + "\n").encode()


def _digest(value):
    return hashlib.sha256(json.dumps(
        {k: v for k, v in value.items() if k != "sha256"},
        ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":")).encode()).hexdigest()


def _strict_json(data: bytes, label: str):
    def pairs_hook(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise FirmwareDeploymentError(f"{label}包含重复字段")
            value[key] = item
        return value
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=pairs_hook,
                          parse_constant=lambda item: (_ for _ in ()).throw(
                              FirmwareDeploymentError(f"{label}包含非标准数值")))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise FirmwareDeploymentError(f"{label}不是合法UTF-8 JSON") from error


def _zip_write(archive, name, content):
    info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    archive.writestr(info, content)


def create_bundle(*, plan: dict, attempt: dict, output_root: Path,
                  output: Path, force: bool = False) -> dict:
    validator = _VALIDATORS.get(plan.get("format")) if isinstance(plan, dict) else None
    if validator is None:
        raise FirmwareDeploymentError("证据包部署计划类型无效")
    plan = validator(plan, output_root=output_root)
    attempt = validate_deployment_attempt(attempt, plan)
    artifact_name = "firmware.elf" if plan["backend"] == "stlink-openocd" else "firmware.bin"
    sources = {
        "evidence/studio-project.json": resolve_artifact(plan["build_id"], "studio-project.json", output_root),
        "evidence/firmware.config": resolve_artifact(plan["build_id"], "firmware.config", output_root),
        "evidence/build-record.json": resolve_artifact(plan["build_id"], "build-record.json", output_root),
        f"evidence/{artifact_name}": resolve_artifact(plan["build_id"], artifact_name, output_root)}
    contents = {name: path.read_bytes() for name, path in sources.items()}
    contents["deployment-plan.json"] = _json_bytes(plan)
    contents["deployment-attempt.json"] = _json_bytes(attempt)
    manifest = {"format": FORMAT, "schema_version": 1,
        "build_id": plan["build_id"], "backend": plan["backend"],
        "outcome": attempt["outcome"],
        "hardware_success_claimed": attempt["hardware_success_claimed"],
        "entries": [{"path": name, "byte_count": len(contents[name]),
                     "sha256": hashlib.sha256(contents[name]).hexdigest()}
                    for name in sorted(contents)],
        "private_keys_included": False, "external_tools_included": False}
    manifest["sha256"] = _digest(manifest)
    contents["manifest.json"] = _json_bytes(manifest)
    if output.is_symlink() or not output.parent.is_dir() or \
            ((output.exists() or output.is_symlink()) and not force):
        raise FirmwareDeploymentError("证据包输出路径无效或已存在")
    descriptor, name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp",
                                         dir=output.parent)
    os.close(descriptor); temporary = Path(name)
    try:
        with zipfile.ZipFile(temporary, "w") as archive:
            for entry in sorted(contents):
                _zip_write(archive, entry, contents[entry])
        if temporary.stat().st_size > MAX_BUNDLE_BYTES:
            raise FirmwareDeploymentError("部署证据包超过32 MiB上限")
        os.replace(temporary, output) if force else os.link(temporary, output)
        temporary.unlink(missing_ok=True)
    finally:
        temporary.unlink(missing_ok=True)
    return {"ok": True, "format": FORMAT, "path": str(output),
            "package_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "manifest_sha256": manifest["sha256"], "outcome": attempt["outcome"],
            "hardware_success_claimed": attempt["hardware_success_claimed"]}


def verify_bundle(path: Path) -> dict:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_BUNDLE_BYTES:
        raise FirmwareDeploymentError("证据包文件无效或过大")
    package_sha = hashlib.sha256(path.read_bytes()).hexdigest()
    try:
        with zipfile.ZipFile(path) as archive:
            infos = archive.infolist()
            names = [item.filename for item in infos]
            if len(names) != len(set(names)) or names != sorted(names) or \
                    any(PurePosixPath(name).is_absolute() or ".." in PurePosixPath(name).parts
                        for name in names):
                raise FirmwareDeploymentError("证据包条目顺序、唯一性或路径无效")
            if sum(item.file_size for item in infos) > MAX_BUNDLE_BYTES or \
                    any(item.file_size > MAX_BUNDLE_BYTES for item in infos):
                raise FirmwareDeploymentError("证据包解压内容超过32 MiB上限")
            if any(item.date_time != (1980, 1, 1, 0, 0, 0) or
                   (item.external_attr >> 16) & 0o777 != 0o644 for item in infos):
                raise FirmwareDeploymentError("证据包时间戳或权限不确定")
            content = {name: archive.read(name) for name in names}
    except (OSError, zipfile.BadZipFile) as error:
        raise FirmwareDeploymentError(f"证据包ZIP无效：{error}") from error
    try:
        manifest = _strict_json(content["manifest.json"], "证据包清单")
        plan = _strict_json(content["deployment-plan.json"], "部署计划")
        attempt = _strict_json(content["deployment-attempt.json"], "部署尝试")
    except KeyError as error:
        raise FirmwareDeploymentError(f"证据包核心JSON缺失或无效：{error}") from error
    if manifest.get("format") != FORMAT or manifest.get("sha256") != _digest(manifest) or \
            manifest.get("private_keys_included") is not False or \
            manifest.get("external_tools_included") is not False:
        raise FirmwareDeploymentError("证据包清单无效")
    expected_names = {item.get("path") for item in manifest.get("entries", [])} | {"manifest.json"}
    if expected_names != set(content):
        raise FirmwareDeploymentError("证据包条目集合与清单不一致")
    for item in manifest["entries"]:
        data = content[item["path"]]
        if item.get("byte_count") != len(data) or item.get("sha256") != hashlib.sha256(data).hexdigest():
            raise FirmwareDeploymentError("证据包条目摘要不一致")
    # 离线校验计划自哈希，不重放工具或访问构建目录。
    if plan.get("sha256") != _digest(plan):
        raise FirmwareDeploymentError("证据包计划自哈希无效")
    evidence = plan.get("evidence", {})
    artifact_entry = ("evidence/firmware.elf" if plan.get("backend") ==
                      "stlink-openocd" else "evidence/firmware.bin")
    expected_hashes = {
        "evidence/studio-project.json": evidence.get("studio_project_sha256"),
        "evidence/firmware.config": evidence.get("firmware_config_sha256"),
        "evidence/build-record.json": evidence.get("build_record_sha256"),
        artifact_entry: evidence.get("firmware_elf_sha256",
                                     evidence.get("firmware_bin_sha256"))}
    if any(name not in content or hashlib.sha256(content[name]).hexdigest() != digest
           for name, digest in expected_hashes.items()):
        raise FirmwareDeploymentError("证据包内构建证据与部署计划摘要不一致")
    attempt = validate_deployment_attempt(attempt, plan)
    if manifest.get("outcome") != attempt["outcome"] or \
            manifest.get("hardware_success_claimed") is not attempt["hardware_success_claimed"]:
        raise FirmwareDeploymentError("证据包清单试图改变attempt终态")
    return {"ok": True, "format": FORMAT, "package_sha256": package_sha,
            "manifest_sha256": manifest["sha256"], "outcome": attempt["outcome"],
            "hardware_success_claimed": attempt["hardware_success_claimed"]}

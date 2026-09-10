#!/usr/bin/env python3
"""Studio 构建记录页的离线预检与受控部署视图模型。"""

from __future__ import annotations

import secrets
import re
import hashlib
import json
import os
import tempfile
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

from deployment_attempt import (
    EXECUTION_CONFIRMATION, create_absent_attempt, execute_deployment_plan,
    validate_deployment_attempt)
from firmware_deployment import (
    FirmwareDeploymentError, create_can_katapult_deployment_plan,
    create_stlink_deployment_plan, create_usb_katapult_deployment_plan)
from firmware_deployment import (
    validate_can_katapult_deployment_plan, validate_stlink_deployment_plan,
    validate_usb_katapult_deployment_plan)

_UUID = re.compile(r"[0-9a-f]{32}")


class StudioDeploymentWorkflow:
    """冻结离线计划；只有一次性令牌和二次确认才能调用执行器。"""

    def __init__(self, *, output_root: Path, attempt_root: Path,
                 reader_factory, backend_config: dict,
                 executor=execute_deployment_plan, token_ttl: float = 300,
                 monotonic=time.monotonic):
        if not 30 <= token_ttl <= 600:
            raise FirmwareDeploymentError("Studio部署确认令牌有效期必须位于30～600秒")
        if attempt_root.exists() and (attempt_root.is_symlink() or
                                      not attempt_root.is_dir()):
            raise FirmwareDeploymentError("部署尝试目录无效")
        attempt_root.mkdir(parents=False, exist_ok=True)
        self.plan_root = attempt_root / "plans"
        self.plan_root.mkdir(exist_ok=True)
        self.output_root = output_root
        self.attempt_root = attempt_root
        self.reader_factory = reader_factory
        self.backend_config = dict(backend_config)
        self.executor = executor
        self.token_ttl = token_ttl
        self.monotonic = monotonic
        self._tokens = {}
        self._lock = threading.Lock()

    def preflight(self, *, backend: object, build_id: object,
                  expected_uuid: object, target: object = None) -> dict:
        """只解析构建记录并生成计划，绝不创建读取器或访问硬件。"""
        if not isinstance(expected_uuid, str) or not _UUID.fullmatch(expected_uuid):
            raise FirmwareDeploymentError("Studio部署预期UUID必须是32位小写十六进制")
        config = self.backend_config.get(backend)
        if not isinstance(config, dict):
            raise FirmwareDeploymentError("Studio未启用所选部署后端")
        if backend == "stlink-openocd":
            if target not in (None, {}):
                raise FirmwareDeploymentError("ST-Link离线预检不接受目标覆盖")
            plan = create_stlink_deployment_plan(
                build_id, output_root=self.output_root,
                probe_serial=config.get("probe_serial"))
        elif backend == "can-katapult":
            if not isinstance(target, dict) or set(target) != {"katapult_uuid"}:
                raise FirmwareDeploymentError("CAN预检必须给出定向Katapult UUID")
            plan = create_can_katapult_deployment_plan(
                build_id, output_root=self.output_root,
                can_interface=config["can_interface"],
                katapult_uuid=target["katapult_uuid"],
                flashtool=Path(config["flashtool"]))
        elif backend == "usb-katapult":
            if target not in (None, {}):
                raise FirmwareDeploymentError("USB离线预检不接受设备覆盖")
            plan = create_usb_katapult_deployment_plan(
                build_id, output_root=self.output_root,
                usb_device=config["usb_device"],
                flashtool=Path(config["flashtool"]))
        else:
            raise FirmwareDeploymentError("Studio部署后端无效")
        absent = create_absent_attempt(plan)
        plan_path = self.plan_root / f"{plan['sha256']}-部署计划-v1.json"
        self._atomic_json(plan_path, plan)
        token = secrets.token_urlsafe(32)
        with self._lock:
            now = self.monotonic()
            self._tokens = {key: value for key, value in self._tokens.items()
                            if value["expires"] > now}
            if len(self._tokens) >= 16:
                raise FirmwareDeploymentError("Studio待确认部署已达到16项上限")
            self._tokens[token] = {"expires": now + self.token_ttl,
                                   "plan": plan, "expected_uuid": expected_uuid}
        return self._view(plan, absent, token)

    @staticmethod
    def _atomic_json(path: Path, value: dict) -> None:
        content = (json.dumps(value, ensure_ascii=False, allow_nan=False,
                              sort_keys=True, indent=2) + "\n").encode()
        if path.exists():
            if path.is_symlink() or not path.is_file() or path.read_bytes() != content:
                raise FirmwareDeploymentError("同哈希部署计划归档不一致")
            return
        descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.",
                                             suffix=".tmp", dir=path.parent)
        temporary = Path(name)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(content); stream.flush(); os.fsync(stream.fileno())
            os.link(temporary, path); temporary.unlink()
        finally:
            temporary.unlink(missing_ok=True)

    def history(self, *, build_id: str = "", backend: str = "") -> dict:
        """只读恢复历史；任何损坏项隔离，不成为可执行令牌。"""
        if len(build_id) > 96 or len(backend) > 32:
            raise FirmwareDeploymentError("部署历史筛选条件过长")
        valid, damaged = [], []
        validators = {
            "REMOTEBSP_STLINK_DEPLOYMENT_PLAN_V1": validate_stlink_deployment_plan,
            "REMOTEBSP_CAN_KATAPULT_DEPLOYMENT_PLAN_V1": validate_can_katapult_deployment_plan,
            "REMOTEBSP_USB_KATAPULT_DEPLOYMENT_PLAN_V1": validate_usb_katapult_deployment_plan}
        files = sorted(self.attempt_root.glob("*-部署尝试-v1.json"))[:1024]
        for path in files:
            try:
                if path.is_symlink() or not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
                    raise FirmwareDeploymentError("文件类型或大小无效")
                attempt = json.loads(path.read_text(encoding="utf-8"))
                attempt = validate_deployment_attempt(attempt)
                plan_path = self.plan_root / f"{attempt['plan_sha256']}-部署计划-v1.json"
                if plan_path.is_symlink() or not plan_path.is_file() or plan_path.stat().st_size > 2 * 1024 * 1024:
                    raise FirmwareDeploymentError("绑定计划不存在或无效")
                plan = json.loads(plan_path.read_text(encoding="utf-8"))
                validator = validators.get(plan.get("format"))
                if validator is None:
                    raise FirmwareDeploymentError("绑定计划类型无效")
                plan = validator(plan, output_root=self.output_root)
                validate_deployment_attempt(attempt, plan)
                if (not build_id or attempt["build_id"] == build_id) and \
                        (not backend or attempt["backend"] == backend):
                    valid.append({**self._view(plan, attempt, None, path.name),
                                  "plan_filename": plan_path.name})
            except (OSError, UnicodeError, json.JSONDecodeError,
                    FirmwareDeploymentError) as error:
                damaged.append({"filename": path.name,
                                "error_type": type(error).__name__,
                                "status": "damaged_isolated"})
        return {"ok": True, "format": "STUDIO_DEPLOYMENT_HISTORY_V1",
                "items": valid, "damaged": damaged, "read_only": True,
                "reexecution_allowed": False}

    def export_manifest(self, *, build_id: str = "", backend: str = "") -> dict:
        history = self.history(build_id=build_id, backend=backend)
        references = []
        for item in history["items"]:
            references.extend((
                {"kind": "plan", "filename": f"plans/{item['plan_filename']}",
                 "sha256": item["plan_sha256"]},
                {"kind": "attempt", "filename": item["attempt"]["filename"],
                 "sha256": item["attempt"]["sha256"]}))
        manifest = {"format": "STUDIO_DEPLOYMENT_EVIDENCE_MANIFEST_V1",
                    "schema_version": 1, "references": references,
                    "damaged_excluded": [x["filename"] for x in history["damaged"]],
                    "files_copied": False, "reexecution_allowed": False}
        encoded = json.dumps(manifest, ensure_ascii=False, sort_keys=True,
                             separators=(",", ":")).encode()
        manifest["sha256"] = hashlib.sha256(encoded).hexdigest()
        return manifest

    def create_evidence_bundle(self, attempt_filename: str) -> dict:
        """从已验证历史生成确定性包；文件名不能选择目录或任意路径。"""
        if not isinstance(attempt_filename, str) or Path(attempt_filename).name != attempt_filename or \
                not attempt_filename.endswith("-部署尝试-v1.json"):
            raise FirmwareDeploymentError("部署attempt文件名无效")
        history = self.history()
        item = next((value for value in history["items"]
                     if value["attempt"]["filename"] == attempt_filename), None)
        if item is None:
            raise FirmwareDeploymentError("部署attempt不存在、损坏或未通过严格校验")
        plan = json.loads((self.plan_root / item["plan_filename"]).read_text(encoding="utf-8"))
        attempt = json.loads((self.attempt_root / attempt_filename).read_text(encoding="utf-8"))
        bundle_root = self.attempt_root / "bundles"
        bundle_root.mkdir(exist_ok=True)
        from deployment_evidence_bundle import create_bundle
        return create_bundle(plan=plan, attempt=attempt,
            output_root=self.output_root,
            output=bundle_root / attempt_filename.replace(".json", ".zip"))

    def execute(self, *, confirmation_token: object, execute: object,
                confirmation: object, expected_uuid: object = None,
                flash_timeout: int = 120, reconnect_timeout: float = 10,
                poll_interval: float = .25) -> dict:
        if execute is not True or confirmation != EXECUTION_CONFIRMATION:
            raise FirmwareDeploymentError("危险部署需要勾选执行并精确填写二次确认短语")
        if not isinstance(confirmation_token, str) or len(confirmation_token) > 128:
            raise FirmwareDeploymentError("Studio部署确认令牌无效")
        with self._lock:
            item = self._tokens.pop(confirmation_token, None)
        if item is None or item["expires"] <= self.monotonic():
            raise FirmwareDeploymentError("Studio部署确认令牌不存在、已使用或已过期")
        plan = item["plan"]
        if expected_uuid is not None and expected_uuid != item["expected_uuid"]:
            raise FirmwareDeploymentError("执行目标UUID与预检冻结值不一致")
        attempt_path = self.attempt_root / (
            f"{plan['build_id']}-{secrets.token_hex(8)}-部署尝试-v1.json")
        reader = self.reader_factory(item["expected_uuid"])
        now = lambda: datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
        attempt = self.executor(
            plan, output_root=self.output_root, reader=reader,
            attempt_output=attempt_path, execute=True,
            confirmation=EXECUTION_CONFIRMATION, flash_timeout=flash_timeout,
            reconnect_timeout=reconnect_timeout, poll_interval=poll_interval,
            started_utc=now(), ended_utc_provider=now)
        return self._view(plan, attempt, None, attempt_path.name)

    def _view(self, plan, attempt, token, filename=None):
        execution = attempt["execution"]
        readback = attempt["readback"]
        failure = execution.get("error_type") or readback.get("error_type")
        return {"ok": True, "format": "STUDIO_DEPLOYMENT_WORKFLOW_VIEW_V1",
                "build_id": plan["build_id"], "backend": plan["backend"],
                "plan_sha256": plan["sha256"], "attempt": {
                    "outcome": attempt["outcome"],
                    "execution_status": execution["status"],
                    "readback_status": readback["status"],
                    "failure_type": failure,
                    "hardware_success_claimed": attempt["hardware_success_claimed"],
                    "sha256": attempt["sha256"], "filename": filename},
                "confirmation_token": token,
                "confirmation_phrase": EXECUTION_CONFIRMATION if token else None,
                "expires_in_seconds": self.token_ttl if token else None,
                "hardware_access_performed": False if token else
                    execution["tool_invoked"],
                "warning": ("离线预检与计划哈希不构成烧录或硬件成功证明；"
                            "只有工具成功并完成显式身份回读才可显示verified。")}

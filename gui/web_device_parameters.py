#!/usr/bin/env python3
"""Studio Web 设备参数写入/恢复的两阶段控制器。"""

from __future__ import annotations

import base64
import json
import re
import secrets
import threading
import time

from device_parameters import (
    BACKUP_SCHEMA_VERSION, MAX_BACKUP_BYTES, MAX_PARAMETERS,
    DeviceParameterError, DeviceParameterManager, WRITE_CONFIRMATION,
)
from parameter_audit import ParameterAuditStore, parameter_evidence


WEB_PARAMETER_CONFIRMATION = WRITE_CONFIRMATION
_UUID = re.compile(r"^[0-9a-f]{32}$")


class WebDeviceParameterController:
    """浏览器只提交受限参数数据，不接触命令、socket、路径或密钥。"""

    def __init__(self, manager: DeviceParameterManager,
                 audit: ParameterAuditStore, *, token_ttl: float = 300.0,
                 monotonic=time.monotonic):
        if not 30 <= token_ttl <= 600:
            raise DeviceParameterError("Web参数确认令牌有效期必须位于30～600秒")
        self.manager = manager
        self.audit = audit
        self.token_ttl = token_ttl
        self.monotonic = monotonic
        self._tokens: dict[str, dict] = {}
        self._lock = threading.Lock()

    @staticmethod
    def _target(snapshot: dict, expected_uuid: object,
                expected_generation: object) -> None:
        if not isinstance(expected_uuid, str) or not _UUID.fullmatch(expected_uuid):
            raise DeviceParameterError("Web参数目标UUID必须是小写32位十六进制")
        if type(expected_generation) is not int or not 0 <= expected_generation <= 0xffffffff:
            raise DeviceParameterError("Web参数预期代数无效")
        if snapshot.get("node_uuid") != expected_uuid:
            raise DeviceParameterError("运行节点UUID与Web参数目标不一致")
        status = snapshot.get("status")
        if not isinstance(status, dict) or status.get("generation") != expected_generation:
            raise DeviceParameterError("设备参数代数已变化，请重新预检")

    def _issue(self, operation: dict, snapshot: dict) -> dict:
        token = secrets.token_urlsafe(32)
        now = self.monotonic()
        with self._lock:
            self._tokens = {key: value for key, value in self._tokens.items()
                            if value["expires"] > now}
            if len(self._tokens) >= 16:
                raise DeviceParameterError("Web参数待确认操作达到16项上限")
            self._tokens[token] = {**operation, "expires": now + self.token_ttl}
        return {
            "ok": True, "format": "STUDIO_WEB_PARAMETER_PREFLIGHT_V1",
            "operation": operation["operation"],
            "confirmation_token": token,
            "confirmation_phrase": WEB_PARAMETER_CONFIRMATION,
            "expires_in_seconds": self.token_ttl,
            "node_id": self.manager.node_id,
            "node_uuid": operation["expected_uuid"],
            "generation": operation["expected_generation"],
            "parameter_evidence": operation["evidence"],
            "planned_steps": ["重新核对同一UUID和参数代数", "原子写入审计意图",
                              "通过toolbusd执行CAS参数变更", "读取新快照",
                              "写入审计终态"],
            "hardware_write_performed": False,
        }

    def preflight_write(self, *, expected_uuid: object,
                        expected_generation: object, parameter_id: object,
                        value_base64: object) -> dict:
        if type(parameter_id) is not int or not 0 <= parameter_id <= 0xffff:
            raise DeviceParameterError("Web参数ID无效")
        if not isinstance(value_base64, str) or len(value_base64) > 4 * 64 + 4:
            raise DeviceParameterError("Web参数值编码无效")
        try:
            raw = base64.b64decode(value_base64, validate=True)
        except ValueError as error:
            raise DeviceParameterError("Web参数值不是合法Base64") from error
        snapshot = self.manager.snapshot()
        self._target(snapshot, expected_uuid, expected_generation)
        current = next((item for item in snapshot.get("parameters", [])
                        if item.get("id") == parameter_id), None)
        if current is None:
            raise DeviceParameterError("Web参数ID未在当前设备定义")
        # 最终长度和类型仍由 DeviceParameterManager 在 CAS 前再次验证。
        if len(raw) > 64:
            raise DeviceParameterError("Web参数值超过64字节上限")
        normalized = base64.b64encode(raw).decode("ascii")
        return self._issue({
            "operation": "write", "expected_uuid": expected_uuid,
            "expected_generation": expected_generation,
            "parameter_id": parameter_id, "value_base64": normalized,
            "evidence": [parameter_evidence(parameter_id, raw)],
        }, snapshot)

    def preflight_restore(self, *, expected_uuid: object,
                          expected_generation: object, backup: object) -> dict:
        try:
            encoded = json.dumps(backup, ensure_ascii=False,
                                 allow_nan=False).encode("utf-8")
        except (TypeError, ValueError) as error:
            raise DeviceParameterError("Web参数备份不是有效JSON") from error
        if len(encoded) > MAX_BACKUP_BYTES or not isinstance(backup, dict) or \
                backup.get("schema_version") not in (1, BACKUP_SCHEMA_VERSION) or \
                backup.get("node_uuid") != expected_uuid or \
                not isinstance(backup.get("parameters"), list) or \
                not 1 <= len(backup["parameters"]) <= MAX_PARAMETERS:
            raise DeviceParameterError("Web参数备份格式、大小或目标UUID无效")
        if backup["schema_version"] == BACKUP_SCHEMA_VERSION:
            digest = backup.get("sha256")
            if backup.get("format") != "REMOTEBSP_DEVICE_PARAMETERS" or \
                    not isinstance(digest, str) or \
                    not re.fullmatch(r"[0-9a-f]{64}", digest) or \
                    digest != DeviceParameterManager._backup_digest(backup):
                raise DeviceParameterError("Web参数备份完整性校验失败")
        snapshot = self.manager.snapshot()
        self._target(snapshot, expected_uuid, expected_generation)
        defined = {item.get("id") for item in snapshot.get("parameters", [])}
        evidence = []
        seen = set()
        for item in backup["parameters"]:
            if not isinstance(item, dict) or type(item.get("id")) is not int or \
                    item["id"] in seen or item["id"] not in defined:
                raise DeviceParameterError("Web参数备份含重复或未定义参数")
            seen.add(item["id"])
            raw = DeviceParameterManager._decode_value(item)
            evidence.append(parameter_evidence(item["id"], raw))
        return self._issue({
            "operation": "restore", "expected_uuid": expected_uuid,
            "expected_generation": expected_generation,
            "backup": backup, "evidence": evidence,
        }, snapshot)

    def execute(self, *, confirmation_token: object,
                confirmation: object) -> dict:
        if not isinstance(confirmation_token, str) or len(confirmation_token) > 128:
            raise DeviceParameterError("Web参数确认令牌无效")
        if confirmation != WEB_PARAMETER_CONFIRMATION:
            raise DeviceParameterError("Web参数操作需要精确填写显式确认短语")
        with self._lock:
            operation = self._tokens.pop(confirmation_token, None)
        if operation is None or operation["expires"] <= self.monotonic():
            raise DeviceParameterError("Web参数确认令牌不存在、已使用或已过期")
        # 令牌消费后再次核对 UUID/代数，杜绝使用过期预检写入。
        snapshot = self.manager.snapshot()
        self._target(snapshot, operation["expected_uuid"],
                     operation["expected_generation"])
        operation_id = self.audit.begin(
            operation=operation["operation"], node_id=self.manager.node_id,
            node_uuid=operation["expected_uuid"],
            expected_generation=operation["expected_generation"],
            parameters=operation["evidence"])
        try:
            if operation["operation"] == "write":
                result = self.manager.write(
                    expected_uuid=operation["expected_uuid"],
                    expected_generation=operation["expected_generation"],
                    parameter_id=operation["parameter_id"],
                    value_base64=operation["value_base64"],
                    confirmation=WRITE_CONFIRMATION)
            else:
                result = self.manager.restore(
                    operation["backup"],
                    expected_uuid=operation["expected_uuid"],
                    expected_generation=operation["expected_generation"],
                    confirmation=WRITE_CONFIRMATION)
        except Exception as error:
            match = re.search(r"完成(\d+)项后中止", str(error))
            applied = int(match.group(1)) if match else 0
            self.audit.finish(
                operation_id,
                outcome="partial_failure" if applied else "failure",
                generation_after=None, applied_count=applied,
                error_type=type(error).__name__)
            raise
        generation = result["status"]["generation"]
        self.audit.finish(
            operation_id, outcome="success", generation_after=generation,
            applied_count=generation - operation["expected_generation"])
        return {
            "ok": True, "format": "STUDIO_WEB_PARAMETER_RESULT_V1",
            "operation": operation["operation"], "status": "success",
            "node_uuid": operation["expected_uuid"],
            "generation_before": operation["expected_generation"],
            "generation_after": generation,
            "audit_operation_id": operation_id,
            "snapshot": result,
            "completed_steps": ["目标身份及代数复核", "审计意图持久化",
                                "CAS参数变更", "新快照读取", "审计终态持久化"],
        }

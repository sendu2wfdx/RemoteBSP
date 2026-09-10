#!/usr/bin/env python3
"""Studio Web 的两阶段、一次性确认 ST-Link 部署控制器。"""

from __future__ import annotations

import os
import re
import secrets
import tempfile
import threading
import time
from pathlib import Path
from typing import Callable

from deployment_record import create_deployment_record
from firmware_deployment import (
    FirmwareDeploymentError, IdentityReader, deploy_stlink,
    expected_identity, make_stlink_plan,
)


WEB_DEPLOY_CONFIRMATION = "FLASH_VERIFIED_FIRMWARE"
_BUILD_ID = re.compile(r"^[a-z0-9-]{8,96}$")
_UUID = re.compile(r"^[0-9a-f]{32}$")


class WebDeploymentController:
    """令牌将预检输入冻结；执行接口不接受命令、路径或烧录器配置。"""

    def __init__(self, *, output_root: Path, record_root: Path,
                 reader_factory: Callable[[str], IdentityReader],
                 runner=None, token_ttl: float = 300.0,
                 monotonic: Callable[[], float] = time.monotonic):
        if not 30 <= token_ttl <= 600:
            raise FirmwareDeploymentError("Web部署确认令牌有效期必须位于30～600秒")
        if record_root.exists() and (record_root.is_symlink() or
                                     not record_root.is_dir()):
            raise FirmwareDeploymentError("Web部署记录路径必须是普通目录")
        if not record_root.exists():
            if not record_root.parent.is_dir() or record_root.parent.is_symlink():
                raise FirmwareDeploymentError("Web部署记录父目录无效")
            record_root.mkdir()
        self.output_root = output_root
        self.record_root = record_root
        self.reader_factory = reader_factory
        self.runner = runner
        self.token_ttl = token_ttl
        self.monotonic = monotonic
        self._tokens: dict[str, dict] = {}
        self._lock = threading.Lock()

    def preflight(self, *, build_id: object, expected_uuid: object) -> dict:
        if not isinstance(build_id, str) or not _BUILD_ID.fullmatch(build_id):
            raise FirmwareDeploymentError("Web部署构建ID无效")
        if not isinstance(expected_uuid, str) or not _UUID.fullmatch(expected_uuid):
            raise FirmwareDeploymentError("Web部署预期UUID必须是小写32位十六进制")
        expected = expected_identity(build_id, output_root=self.output_root)
        # 预检同时解析实际将写入的 ELF；只生成固定命令计划，不执行进程。
        make_stlink_plan(build_id, output_root=self.output_root)
        observed = self.reader_factory(expected_uuid).read_identity()
        if observed.device_uuid != expected_uuid:
            raise FirmwareDeploymentError("Web部署运行节点UUID与显式目标不一致")
        if observed.board_id != expected.board_id:
            raise FirmwareDeploymentError("Web部署运行节点板型与构建板型不一致")
        token = secrets.token_urlsafe(32)
        with self._lock:
            now = self.monotonic()
            self._tokens = {key: value for key, value in self._tokens.items()
                            if value["expires"] > now}
            if len(self._tokens) >= 16:
                raise FirmwareDeploymentError("Web部署待确认作业已达到16项上限")
            self._tokens[token] = {
                "build_id": build_id, "expected_uuid": expected_uuid,
                "expires": now + self.token_ttl,
            }
        return {
            "ok": True, "format": "STUDIO_WEB_DEPLOY_PREFLIGHT_V1",
            "confirmation_token": token,
            "expires_in_seconds": self.token_ttl,
            "build_id": build_id, "board_id": expected.board_id,
            "device_uuid": expected_uuid,
            "expected_identity": {
                "project_sha256": expected.project_sha256,
                "config_sha256": expected.config_sha256,
                "firmware_identity_sha256": expected.firmware_identity_sha256,
            },
            "current_identity": {
                "project_sha256": observed.project_sha256,
                "config_sha256": observed.config_sha256,
                "firmware_identity_sha256": observed.firmware_identity_sha256,
            },
            "planned_steps": ["构建产物完整性复核", "ST-Link写入及verify/reset",
                              "等待同一UUID节点重连", "四重运行时身份核验",
                              "原子保存部署记录"],
            "confirmation_phrase": WEB_DEPLOY_CONFIRMATION,
            "hardware_access_performed": True,
            "firmware_flash_performed": False,
        }

    def execute(self, *, confirmation_token: object, confirmation: object,
                flash_timeout: object, reconnect_timeout: object,
                poll_interval: object) -> dict:
        if not isinstance(confirmation_token, str) or len(confirmation_token) > 128:
            raise FirmwareDeploymentError("Web部署确认令牌无效")
        if confirmation != WEB_DEPLOY_CONFIRMATION:
            raise FirmwareDeploymentError("Web部署需要精确填写显式确认短语")
        with self._lock:
            item = self._tokens.pop(confirmation_token, None)
        if item is None or item["expires"] <= self.monotonic():
            raise FirmwareDeploymentError("Web部署确认令牌不存在、已使用或已过期")
        if type(flash_timeout) is not int or not 1 <= flash_timeout <= 600 or \
                type(reconnect_timeout) not in (int, float) or \
                type(poll_interval) not in (int, float):
            raise FirmwareDeploymentError("Web部署超时参数无效")
        reader = self.reader_factory(item["expected_uuid"])
        kwargs = {
            "output_root": self.output_root, "flash_timeout": flash_timeout,
            "reconnect_timeout": float(reconnect_timeout),
            "poll_interval": float(poll_interval),
        }
        if self.runner is not None:
            kwargs["runner"] = self.runner
        result = deploy_stlink(item["build_id"], reader, **kwargs)
        record = create_deployment_record(result, output_root=self.output_root)
        filename = (f"{result.build_id}-{result.observed.device_uuid}-"
                    f"{record.record['record_sha256'][:16]}-部署记录-v1.json")
        path = self.record_root / filename
        self._atomic_create(path, record.content)
        return {
            "ok": True, "format": "STUDIO_WEB_DEPLOY_RESULT_V1",
            "status": "performed_and_verified", "build_id": result.build_id,
            "board_id": result.expected.board_id,
            "device_uuid": result.observed.device_uuid,
            "backend": result.backend, "attempts": result.attempts,
            "verified": True,
            "completed_steps": ["构建产物完整性复核", "ST-Link写入及verify/reset",
                                "同一UUID节点重连", "四重运行时身份核验",
                                "部署记录原子保存"],
            "deployment_record": record.record,
            "deployment_record_sha256": record.sha256,
            "deployment_record_filename": filename,
        }

    @staticmethod
    def _atomic_create(path: Path, content: bytes) -> None:
        descriptor, name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
        temporary = Path(name)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            os.link(temporary, path)
            temporary.unlink()
        except FileExistsError as error:
            raise FirmwareDeploymentError(
                "相同构建和设备的部署记录已存在，拒绝覆盖") from error
        finally:
            temporary.unlink(missing_ok=True)

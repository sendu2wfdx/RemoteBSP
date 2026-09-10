#!/usr/bin/env python3
"""Studio 显式固件部署作业与烧录后身份核对。

本模块只编排已经校验的构建产物、烧录适配器和回读适配器。调用者必须
显式发起部署；构建、工程检查和批次归档不会隐式调用这里的入口。
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, Protocol, Sequence

from firmware_builder import FirmwareBuildError, resolve_artifact


class FirmwareDeploymentError(RuntimeError):
    """部署输入、烧录或回读核对失败。"""


class IdentityCapabilityError(FirmwareDeploymentError):
    """运行时身份合同缺少完成部署核验所必需的字段。"""


_HASH = re.compile(r"[0-9a-f]{64}")
_BUILD_ID = re.compile(r"[a-z0-9-]{8,96}")
_PROBE_SERIAL = re.compile(r"[A-Za-z0-9._:-]{1,96}")
_OPENOCD_TARGET = {
    "mellow-fly-d5-v1": "target/stm32f0x.cfg",
    "weact-bluepill-plus-v1": "target/stm32f1x.cfg",
    "weact-g431-core-v10": "target/stm32g4x.cfg",
}


@dataclass(frozen=True)
class FirmwareIdentity:
    board_id: str
    project_sha256: str
    config_sha256: str
    firmware_identity_sha256: str


@dataclass(frozen=True)
class DeviceIdentity:
    board_id: str
    project_sha256: str
    config_sha256: str
    firmware_identity_sha256: str
    device_uuid: str


@dataclass(frozen=True)
class FlashPlan:
    backend: str
    command: tuple[str, ...]
    artifact: Path


@dataclass(frozen=True)
class DeploymentResult:
    build_id: str
    backend: str
    expected: FirmwareIdentity
    observed: DeviceIdentity
    attempts: int
    verified: bool


class IdentityReader(Protocol):
    def read_identity(self) -> DeviceIdentity:
        """读取设备启动后的固件身份和配置身份。"""


@dataclass(frozen=True)
class RuntimeNodeIdentity:
    """toolbusd 当前实际公开的节点身份子集。"""

    node_id: int
    board_id: str
    device_uuid: str
    online: bool
    ready: bool
    firmware_version: tuple[int, int, int]
    protocol_version: int


RUNTIME_IDENTITY_CAPABILITIES_MISSING = (
    "project_sha256", "config_sha256", "firmware_identity_sha256")


class ToolbusdIdentityReader:
    """通过 remote-cli 的稳定 JSON IPC 边界读取单个运行中节点。

    当前 node-list 合同只公开 UUID、board_type、在线状态和协议/固件
    版本，不公开工程、配置与固件输入哈希。因此本适配器能够严格确认目标
    节点，却会拒绝把这个身份子集伪装成完整的 DeviceIdentity。
    """

    MAX_BYTES = 64 * 1024
    MAX_NODES = 128
    _BOARD_TYPES = {
        0xF072D5: "mellow-fly-d5-v1",
        0x0103CB: "weact-bluepill-plus-v1",
        0x0431CB: "weact-g431-core-v10",
    }

    def __init__(self, socket_path: str, node_id: int, *,
                 expected_uuid: str | None = None,
                 remote_cli: str = "remote-cli", timeout: float = 2.0,
                 runner: Callable[[Sequence[str], float, int], bytes] | None = None):
        if not isinstance(socket_path, str) or not socket_path or \
                len(socket_path.encode("utf-8")) > 512 or any(
                    character in socket_path for character in "\r\n\0"):
            raise FirmwareDeploymentError("toolbusd socket路径无效")
        if type(node_id) is not int or not 1 <= node_id <= 127:
            raise FirmwareDeploymentError("toolbusd节点ID必须位于1～127")
        if expected_uuid is not None and (not isinstance(expected_uuid, str) or
                not re.fullmatch(r"[0-9a-fA-F]{32}", expected_uuid)):
            raise FirmwareDeploymentError("预期节点UUID必须是32位十六进制")
        if not isinstance(remote_cli, str) or not remote_cli or \
                len(remote_cli) > 512 or any(
                    character in remote_cli for character in "\r\n\0"):
            raise FirmwareDeploymentError("remote-cli路径无效")
        if not math.isfinite(timeout) or not 0.05 <= timeout <= 30:
            raise FirmwareDeploymentError("身份读取超时必须位于0.05～30秒")
        self.socket_path = socket_path
        self.node_id = node_id
        self.expected_uuid = expected_uuid.lower() if expected_uuid else None
        self.remote_cli = remote_cli
        self.timeout = timeout
        self._runner = runner or self._run_cli

    @staticmethod
    def require_complete_identity() -> None:
        raise IdentityCapabilityError(
            "当前toolbusd node-list合同不公开" + "、".join(
                RUNTIME_IDENTITY_CAPABILITIES_MISSING) +
            "，不能用于四重烧录身份核验")

    @staticmethod
    def _strict_object(pairs: list[tuple[str, object]]) -> dict:
        value: dict[str, object] = {}
        for key, item in pairs:
            if key in value:
                raise FirmwareDeploymentError(
                    f"toolbusd身份输出包含重复字段：{key}")
            value[key] = item
        return value

    @staticmethod
    def _run_cli(command: Sequence[str], timeout: float,
                 maximum_output: int) -> bytes:
        try:
            with tempfile.TemporaryFile() as output:
                completed = subprocess.run(
                    list(command), check=False, stdout=output,
                    stderr=subprocess.STDOUT, timeout=timeout, shell=False)
                output.seek(0)
                content = output.read(maximum_output + 1)
        except subprocess.TimeoutExpired as error:
            raise TimeoutError("remote-cli身份读取超时") from error
        except OSError as error:
            raise FirmwareDeploymentError(
                f"remote-cli身份读取无法启动：{error}") from error
        if len(content) > maximum_output:
            raise FirmwareDeploymentError("remote-cli身份输出超过64 KiB上限")
        if completed.returncode != 0:
            detail = content.decode("utf-8", errors="replace")[-2000:]
            raise FirmwareDeploymentError(
                f"remote-cli身份读取失败（退出码{completed.returncode}）：{detail}")
        return content

    def read_runtime_node(self) -> RuntimeNodeIdentity:
        command = (self.remote_cli, "--json", "--socket", self.socket_path,
                   "node-list")
        content = self._runner(command, self.timeout, self.MAX_BYTES)
        if not isinstance(content, bytes):
            raise FirmwareDeploymentError("remote-cli身份读取器必须返回bytes")
        if len(content) > self.MAX_BYTES:
            raise FirmwareDeploymentError("remote-cli身份输出超过64 KiB上限")
        try:
            root = json.loads(
                content.decode("utf-8"), object_pairs_hook=self._strict_object,
                parse_constant=lambda item: (_ for _ in ()).throw(
                    FirmwareDeploymentError(
                        f"toolbusd身份输出包含非标准数值：{item}")))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise FirmwareDeploymentError(
                f"remote-cli身份输出不是合法UTF-8 JSON：{error}") from error
        if not isinstance(root, dict) or set(root) != {
                "schema_version", "command", "data"} or \
                type(root.get("schema_version")) is not int or \
                root["schema_version"] != 1 or root.get("command") != "node-list":
            raise FirmwareDeploymentError("remote-cli身份输出信封无效")
        data = root["data"]
        if not isinstance(data, dict) or set(data) != {"nodes"} or \
                not isinstance(data["nodes"], list):
            raise FirmwareDeploymentError("node-list.data字段无效")
        if len(data["nodes"]) > self.MAX_NODES:
            raise FirmwareDeploymentError("node-list节点数量超过128项")
        matches = []
        expected_fields = {"node_id", "online", "ready", "board_type",
                           "firmware", "protocol_version", "uuid"}
        for index, raw in enumerate(data["nodes"]):
            if not isinstance(raw, dict) or set(raw) != expected_fields:
                raise FirmwareDeploymentError(
                    f"node-list.nodes[{index}]字段无效")
            if type(raw["node_id"]) is not int or \
                    not 1 <= raw["node_id"] <= 127 or \
                    type(raw["online"]) is not bool or \
                    type(raw["ready"]) is not bool or \
                    type(raw["board_type"]) is not int or \
                    not 0 <= raw["board_type"] <= 0xffffffff or \
                    type(raw["protocol_version"]) is not int or \
                    not 0 <= raw["protocol_version"] <= 0xff or \
                    not isinstance(raw["uuid"], str) or \
                    not re.fullmatch(r"[0-9a-fA-F]{32}", raw["uuid"]):
                raise FirmwareDeploymentError(
                    f"node-list.nodes[{index}]值无效")
            firmware = raw["firmware"]
            if not isinstance(firmware, dict) or set(firmware) != {
                    "major", "minor", "patch"} or any(
                        type(firmware[name]) is not int or
                        not 0 <= firmware[name] <= 0xffff
                        for name in ("major", "minor", "patch")):
                raise FirmwareDeploymentError(
                    f"node-list.nodes[{index}].firmware无效")
            if raw["node_id"] == self.node_id:
                matches.append(raw)
        if len(matches) != 1:
            raise FirmwareDeploymentError(
                "node-list中目标节点不存在或节点ID重复")
        node = matches[0]
        uuid = node["uuid"].lower()
        if self.expected_uuid is not None and uuid != self.expected_uuid:
            raise FirmwareDeploymentError("运行中节点UUID与显式选择不一致")
        board_id = self._BOARD_TYPES.get(node["board_type"])
        if board_id is None:
            raise FirmwareDeploymentError("运行中节点board_type不受Studio支持")
        if not node["online"] or not node["ready"]:
            raise FirmwareDeploymentError("目标节点尚未在线并完成资源就绪")
        return RuntimeNodeIdentity(
            self.node_id, board_id, uuid, node["online"], node["ready"],
            tuple(firmware[name] for name in ("major", "minor", "patch")),
            node["protocol_version"])

    def read_identity(self) -> DeviceIdentity:
        node = self.read_runtime_node()
        raise IdentityCapabilityError(
            "toolbusd node-list已确认运行中节点"
            f"{node.node_id}/{node.device_uuid}，但当前运行时合同不公开"
            + "、".join(RUNTIME_IDENTITY_CAPABILITIES_MISSING) + "；"
            "为避免把构建记录冒充设备回读，拒绝完成烧录后身份核验")


class JsonIdentityFileReader:
    """从上位机原子更新的有界 JSON 文件读取运行中设备身份。"""

    MAX_BYTES = 16 * 1024

    def __init__(self, path: Path):
        self.path = path

    @staticmethod
    def _strict_object(pairs: list[tuple[str, object]]) -> dict:
        value: dict[str, object] = {}
        for key, item in pairs:
            if key in value:
                raise FirmwareDeploymentError(f"身份文件包含重复字段：{key}")
            value[key] = item
        return value

    def read_identity(self) -> DeviceIdentity:
        try:
            if self.path.is_symlink() or not self.path.is_file():
                raise FirmwareDeploymentError("身份文件必须是普通文件")
            with self.path.open("rb") as stream:
                content = stream.read(self.MAX_BYTES + 1)
            if len(content) > self.MAX_BYTES:
                raise FirmwareDeploymentError("身份文件超过16 KiB上限")
            value = json.loads(
                content.decode("utf-8"), object_pairs_hook=self._strict_object,
                parse_constant=lambda item: (_ for _ in ()).throw(
                    FirmwareDeploymentError(f"身份文件包含非标准数值：{item}")))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise FirmwareDeploymentError(f"身份文件暂不可读：{error}") from error
        if not isinstance(value, dict):
            raise FirmwareDeploymentError("身份文件必须是JSON对象")
        allowed = {"board_id", "project_sha256", "config_sha256",
                   "firmware_identity_sha256", "device_uuid"}
        unknown = sorted(set(value) - allowed)
        missing = sorted(allowed - set(value))
        if unknown or missing:
            details = []
            if missing:
                details.append("缺少" + ",".join(missing))
            if unknown:
                details.append("未知" + ",".join(unknown))
            raise FirmwareDeploymentError("身份文件字段无效：" + "；".join(details))
        board_id = value["board_id"]
        device_uuid = value["device_uuid"]
        if not isinstance(board_id, str) or board_id not in _OPENOCD_TARGET:
            raise FirmwareDeploymentError("身份文件board_id无效")
        if not isinstance(device_uuid, str) or not device_uuid or \
                len(device_uuid) > 128 or any(
                    character in device_uuid for character in "\r\n\0"):
            raise FirmwareDeploymentError("身份文件device_uuid无效")
        return DeviceIdentity(
            board_id=board_id,
            project_sha256=_require_hash(value["project_sha256"],
                                         "project_sha256"),
            config_sha256=_require_hash(value["config_sha256"],
                                        "config_sha256"),
            firmware_identity_sha256=_require_hash(
                value["firmware_identity_sha256"],
                "firmware_identity_sha256"),
            device_uuid=device_uuid)


def _require_hash(value: object, field: str) -> str:
    if not isinstance(value, str) or not _HASH.fullmatch(value):
        raise FirmwareDeploymentError(f"{field}必须是小写SHA-256")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def expected_identity(build_id: str, *, output_root: Path) -> FirmwareIdentity:
    """从受保护的构建记录及其产物导出唯一预期身份。"""
    if not isinstance(build_id, str) or not _BUILD_ID.fullmatch(build_id):
        raise FirmwareDeploymentError("构建ID无效")
    try:
        record_path = resolve_artifact(build_id, "build-record.json", output_root)
        firmware = resolve_artifact(build_id, "firmware.bin", output_root)
        import json
        record = json.loads(record_path.read_text(encoding="utf-8"))
    except (FirmwareBuildError, OSError, ValueError) as error:
        raise FirmwareDeploymentError(f"构建记录不可用于部署：{error}") from error
    if record.get("build_id") != build_id:
        raise FirmwareDeploymentError("构建记录与构建ID不一致")
    board_id = record.get("board_id")
    if board_id not in _OPENOCD_TARGET:
        raise FirmwareDeploymentError("构建记录中的板卡不支持ST-Link")
    return FirmwareIdentity(
        board_id=board_id,
        project_sha256=_require_hash(record.get("project_sha256"),
                                     "project_sha256"),
        config_sha256=_require_hash(record.get("config_sha256"),
                                    "config_sha256"),
        firmware_identity_sha256=_require_hash(
            record.get("firmware_input_sha256"), "firmware_input_sha256"),
    )


def make_stlink_plan(build_id: str, *, output_root: Path,
                     probe_serial: str | None = None) -> FlashPlan:
    """生成无 shell 拼接、带 verify/reset 的 OpenOCD ST-Link 命令。"""
    identity = expected_identity(build_id, output_root=output_root)
    try:
        artifact = resolve_artifact(build_id, "firmware.elf", output_root)
    except FirmwareBuildError as error:
        raise FirmwareDeploymentError(
            f"构建记录不可用于部署：{error}") from error
    command = ["openocd", "-f", "interface/stlink.cfg"]
    if probe_serial is not None:
        if not isinstance(probe_serial, str) or not _PROBE_SERIAL.fullmatch(
                probe_serial):
            raise FirmwareDeploymentError("ST-Link序列号格式无效")
        command.extend(("-c", f"adapter serial {probe_serial}"))
    artifact_text = artifact.as_posix()
    if any(character in artifact_text for character in "{}\r\n"):
        raise FirmwareDeploymentError("固件产物路径不能安全传给OpenOCD")
    command.extend((
        "-f", _OPENOCD_TARGET[identity.board_id],
        "-c", f"program {{{artifact_text}}} verify reset exit"))
    return FlashPlan("stlink-openocd", tuple(command), artifact)


def _run_flash(command: Sequence[str], timeout: int) -> None:
    try:
        completed = subprocess.run(
            list(command), check=False, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=timeout,
            shell=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise FirmwareDeploymentError(f"烧录器启动失败：{error}") from error
    if completed.returncode != 0:
        output = ((completed.stdout or "") + (completed.stderr or ""))[-8000:]
        raise FirmwareDeploymentError(
            f"烧录失败（退出码{completed.returncode}）：\n{output}")


def verify_identity(expected: FirmwareIdentity,
                    observed: DeviceIdentity) -> None:
    """完整核对板卡、工程、配置和固件四重身份，任一缺失均失败。"""
    fields = ("board_id", "project_sha256", "config_sha256",
              "firmware_identity_sha256")
    mismatch = [name for name in fields
                if getattr(expected, name) != getattr(observed, name)]
    if not isinstance(observed.device_uuid, str) or not observed.device_uuid:
        mismatch.append("device_uuid")
    if mismatch:
        raise FirmwareDeploymentError(
            "烧录后身份核对失败：" + ", ".join(mismatch))


def deploy_stlink(
        build_id: str, reader: IdentityReader, *, output_root: Path,
        probe_serial: str | None = None, flash_timeout: int = 120,
        reconnect_timeout: float = 10.0, poll_interval: float = 0.25,
        runner: Callable[[Sequence[str], int], None] = _run_flash,
        sleeper: Callable[[float], None] = time.sleep) -> DeploymentResult:
    """烧录、复位、等待节点重连，并核对运行中固件身份。"""
    if flash_timeout < 1 or flash_timeout > 600:
        raise FirmwareDeploymentError("烧录超时必须位于1～600秒")
    if not math.isfinite(reconnect_timeout) or \
            not math.isfinite(poll_interval) or reconnect_timeout <= 0 or \
            reconnect_timeout > 120 or poll_interval <= 0 or \
            poll_interval > reconnect_timeout:
        raise FirmwareDeploymentError("重连等待参数无效")
    expected = expected_identity(build_id, output_root=output_root)
    capability_check = getattr(reader, "require_complete_identity", None)
    if capability_check is not None:
        capability_check()
    plan = make_stlink_plan(build_id, output_root=output_root,
                            probe_serial=probe_serial)
    runner(plan.command, flash_timeout)
    deadline = time.monotonic() + reconnect_timeout
    attempts = 0
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        attempts += 1
        try:
            observed = reader.read_identity()
            verify_identity(expected, observed)
            return DeploymentResult(build_id, plan.backend, expected, observed,
                                    attempts, True)
        except (FirmwareDeploymentError, OSError, TimeoutError) as error:
            last_error = error
            sleeper(poll_interval)
    raise FirmwareDeploymentError(
        f"烧录后节点未在期限内通过身份核对：{last_error}")

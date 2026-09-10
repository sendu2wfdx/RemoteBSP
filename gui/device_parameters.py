#!/usr/bin/env python3
"""Studio 通过 toolbusd 本地 IPC 管理单节点设备参数。"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import math
import re
import subprocess
import tempfile
import threading
from typing import Callable, Sequence

from firmware_deployment import FirmwareDeploymentError, ToolbusdIdentityReader


class DeviceParameterError(ValueError):
    pass


WRITE_CONFIRMATION = "WRITE_DEVICE_PARAMETERS"
MAX_CLI_OUTPUT = 64 * 1024
MAX_PARAMETERS = 64
MAX_VALUE_BYTES = 64
MAX_BACKUP_BYTES = 32 * 1024
BACKUP_SCHEMA_VERSION = 2

_STATUS = re.compile(
    r"^version=(\d+) generation=(\d+) stored=(\d+) definitions=(\d+) "
    r"maintenance_unlocked=(yes|no) restart_required=(yes|no) "
    r"store_error=(\d+)$")
_DESCRIPTOR = re.compile(
    r"^id=0x([0-9a-f]+) name=([a-z0-9-]+) type=(\d+) "
    r"flags=0x([0-9a-f]+) length=(\d+)\.\.(\d+)$")
_VALUE = re.compile(
    r"^id=0x([0-9a-f]+) name=([a-z0-9-]+) generation=(\d+) "
    r"type=(\d+) value=(.*)$")


class DeviceParameterManager:
    """有界、串行的 Studio 参数适配器；网页不会直接接触 CAN。"""

    def __init__(self, socket_path: str, node_id: int, *,
                 remote_cli: str = "remote-cli", timeout: float = 3.0,
                 runner: Callable[[Sequence[str], float, int], bytes] | None = None):
        # 复用已审计的 socket、节点和可执行文件边界校验。
        reader = ToolbusdIdentityReader(
            socket_path, node_id, remote_cli=remote_cli, timeout=timeout,
            runner=runner)
        self.socket_path = reader.socket_path
        self.node_id = reader.node_id
        self.remote_cli = reader.remote_cli
        self.timeout = reader.timeout
        self._runner = runner or self._run_cli
        self._identity = reader
        self._lock = threading.Lock()

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
            raise DeviceParameterError("设备参数操作超时") from error
        except OSError as error:
            raise DeviceParameterError(f"remote-cli无法启动：{error}") from error
        if len(content) > maximum_output:
            raise DeviceParameterError("remote-cli输出超过64 KiB上限")
        if completed.returncode != 0:
            detail = content.decode("utf-8", errors="replace")[-2000:]
            raise DeviceParameterError(
                f"设备参数操作失败（退出码{completed.returncode}）：{detail}")
        return content

    def _run(self, *arguments: str) -> str:
        command = (self.remote_cli, "--socket", self.socket_path,
                   "--node", str(self.node_id), *arguments)
        value = self._runner(command, self.timeout, MAX_CLI_OUTPUT)
        if not isinstance(value, bytes) or len(value) > MAX_CLI_OUTPUT:
            raise DeviceParameterError("remote-cli返回值无效或超过上限")
        try:
            return value.decode("utf-8").rstrip("\r\n")
        except UnicodeDecodeError as error:
            raise DeviceParameterError("remote-cli输出不是合法UTF-8") from error

    def _runtime_node(self):
        try:
            return self._identity.read_runtime_node()
        except FirmwareDeploymentError as error:
            raise DeviceParameterError(str(error)) from error

    def _status(self) -> dict:
        match = _STATUS.fullmatch(self._run("param-status"))
        if not match:
            raise DeviceParameterError("设备参数状态输出格式无效")
        version, generation, stored, definitions, unlocked, restart, error = \
            match.groups()
        result = {
            "version": int(version), "generation": int(generation),
            "stored_count": int(stored), "definition_count": int(definitions),
            "maintenance_unlocked": unlocked == "yes",
            "restart_required": restart == "yes", "store_error": int(error),
        }
        if result["definition_count"] > MAX_PARAMETERS:
            raise DeviceParameterError("设备参数定义数量超过64项上限")
        return result

    def _descriptors(self) -> list[dict]:
        output = self._run("param-list")
        lines = [] if not output else output.splitlines()
        if len(lines) > MAX_PARAMETERS:
            raise DeviceParameterError("设备参数定义数量超过64项上限")
        descriptors = []
        ids = set()
        for line in lines:
            match = _DESCRIPTOR.fullmatch(line)
            if not match:
                raise DeviceParameterError("设备参数定义输出格式无效")
            parameter_id, name, kind, flags, minimum, maximum = match.groups()
            item = {"id": int(parameter_id, 16), "name": name,
                    "type": int(kind), "flags": int(flags, 16),
                    "minimum_length": int(minimum),
                    "maximum_length": int(maximum)}
            if item["id"] in ids or item["maximum_length"] > MAX_VALUE_BYTES or \
                    item["minimum_length"] > item["maximum_length"]:
                raise DeviceParameterError("设备参数定义重复或长度越界")
            ids.add(item["id"])
            descriptors.append(item)
        return descriptors

    def _read_value(self, descriptor: dict) -> dict:
        output = self._run("param-get", f"0x{descriptor['id']:04x}")
        match = _VALUE.fullmatch(output)
        if not match:
            raise DeviceParameterError("设备参数值输出格式无效")
        parameter_id, name, generation, kind, encoded = match.groups()
        if int(parameter_id, 16) != descriptor["id"] or \
                int(kind) != descriptor["type"] or name != descriptor["name"]:
            raise DeviceParameterError("设备参数值与定义不一致")
        if descriptor["type"] == 2:
            value = encoded.encode("utf-8")
        else:
            if not re.fullmatch(r"(?:[0-9a-f]{2})*", encoded):
                raise DeviceParameterError("二进制设备参数不是小写十六进制")
            value = bytes.fromhex(encoded)
        if len(value) > descriptor["maximum_length"]:
            raise DeviceParameterError("设备参数值超过定义长度")
        return {"id": descriptor["id"], "name": name,
                "type": descriptor["type"], "flags": descriptor["flags"],
                "generation": int(generation), "byte_count": len(value),
                "value_base64": base64.b64encode(value).decode("ascii"),
                **({"text": encoded} if descriptor["type"] == 2 else {})}

    def snapshot(self) -> dict:
        with self._lock:
            node = self._runtime_node()
            status = self._status()
            descriptors = self._descriptors()
            if len(descriptors) != status["definition_count"]:
                raise DeviceParameterError("参数状态与定义数量不一致")
            values = [self._read_value(item) for item in descriptors]
            generations = {item["generation"] for item in values}
            if generations and generations != {status["generation"]}:
                raise DeviceParameterError("读取期间参数代数发生变化，请重试")
            return {"schema_version": 1, "node_id": self.node_id,
                    "node_uuid": node.device_uuid, "status": status,
                    "parameters": values}

    @staticmethod
    def _backup_digest(backup: dict) -> str:
        content = {key: value for key, value in backup.items()
                   if key != "sha256"}
        canonical = json.dumps(content, ensure_ascii=False, sort_keys=True,
                               separators=(",", ":")).encode("utf-8")
        return hashlib.sha256(canonical).hexdigest()

    def backup(self) -> dict:
        """生成带完整性摘要的可移植备份；快照协议保持 v1。"""
        snapshot = self.snapshot()
        backup = {**snapshot, "schema_version": BACKUP_SCHEMA_VERSION,
                  "format": "REMOTEBSP_DEVICE_PARAMETERS"}
        backup["sha256"] = self._backup_digest(backup)
        return backup

    @staticmethod
    def _decode_value(item: object) -> bytes:
        if not isinstance(item, dict) or set(item) - {
                "id", "name", "type", "flags", "generation", "byte_count",
                "value_base64", "text"}:
            raise DeviceParameterError("参数记录字段无效")
        encoded = item.get("value_base64")
        if not isinstance(encoded, str) or len(encoded) > 4 * MAX_VALUE_BYTES + 4:
            raise DeviceParameterError("参数值编码无效")
        try:
            value = base64.b64decode(encoded, validate=True)
        except ValueError as error:
            raise DeviceParameterError("参数值不是合法Base64") from error
        if len(value) > MAX_VALUE_BYTES or item.get("byte_count") != len(value):
            raise DeviceParameterError("参数值长度无效")
        return value

    def write(self, *, expected_uuid: str, expected_generation: int,
              parameter_id: int, value_base64: str, confirmation: str) -> dict:
        record = {"id": parameter_id, "value_base64": value_base64,
                  "byte_count": -1}
        try:
            raw = base64.b64decode(value_base64, validate=True)
        except (TypeError, ValueError) as error:
            raise DeviceParameterError("参数值不是合法Base64") from error
        record["byte_count"] = len(raw)
        self._mutate(expected_uuid, expected_generation, [record], confirmation)
        return self.snapshot()

    def restore(self, backup: object, *, expected_uuid: str,
                expected_generation: int, confirmation: str) -> dict:
        try:
            encoded = json.dumps(backup, ensure_ascii=False).encode("utf-8")
        except (TypeError, ValueError) as error:
            raise DeviceParameterError("备份不是合法JSON对象") from error
        if len(encoded) > MAX_BACKUP_BYTES or not isinstance(backup, dict) or \
                backup.get("schema_version") not in (1, BACKUP_SCHEMA_VERSION) or \
                backup.get("node_uuid") != expected_uuid or \
                not isinstance(backup.get("parameters"), list) or \
                len(backup["parameters"]) > MAX_PARAMETERS:
            raise DeviceParameterError("备份格式、大小或目标UUID无效")
        if backup["schema_version"] == BACKUP_SCHEMA_VERSION:
            if backup.get("format") != "REMOTEBSP_DEVICE_PARAMETERS" or \
                    not isinstance(backup.get("sha256"), str) or \
                    not re.fullmatch(r"[0-9a-f]{64}", backup["sha256"]):
                raise DeviceParameterError("设备参数备份v2元数据无效")
            if not hmac.compare_digest(
                    backup["sha256"], self._backup_digest(backup)):
                raise DeviceParameterError("设备参数备份完整性校验失败")
        self._mutate(expected_uuid, expected_generation,
                     backup["parameters"], confirmation)
        return self.snapshot()

    def _mutate(self, expected_uuid: str, expected_generation: int,
                records: list[object], confirmation: str) -> None:
        if confirmation != WRITE_CONFIRMATION:
            raise DeviceParameterError("写入需要显式维护确认")
        if not isinstance(expected_uuid, str) or not re.fullmatch(
                r"[0-9a-f]{32}", expected_uuid):
            raise DeviceParameterError("预期节点UUID必须是小写32位十六进制")
        if type(expected_generation) is not int or not 0 <= expected_generation <= 0xffffffff:
            raise DeviceParameterError("预期参数代数无效")
        if not records or len(records) > MAX_PARAMETERS:
            raise DeviceParameterError("写入参数数量必须位于1～64")
        with self._lock:
            node = self._runtime_node()
            if node.device_uuid != expected_uuid:
                raise DeviceParameterError("运行节点UUID与写入目标不一致")
            status = self._status()
            if status["maintenance_unlocked"]:
                raise DeviceParameterError("设备已处于维护解锁状态，拒绝接管")
            if status["generation"] != expected_generation:
                raise DeviceParameterError("设备参数代数已变化，请重新读取")
            descriptors = {item["id"]: item for item in self._descriptors()}
            prepared = []
            seen = set()
            for record in records:
                if not isinstance(record, dict) or type(record.get("id")) is not int:
                    raise DeviceParameterError("参数记录ID无效")
                parameter_id = record["id"]
                if parameter_id in seen or parameter_id not in descriptors:
                    raise DeviceParameterError("参数记录重复或未定义")
                seen.add(parameter_id)
                value = self._decode_value(record)
                definition = descriptors[parameter_id]
                if "type" in record and record["type"] != definition["type"]:
                    raise DeviceParameterError("参数记录类型与当前定义不一致")
                if not definition["minimum_length"] <= len(value) <= definition["maximum_length"]:
                    raise DeviceParameterError("参数值不符合定义长度")
                current = self._read_value(definition)
                if current["generation"] != expected_generation:
                    raise DeviceParameterError("读取期间参数代数发生变化，请重试")
                if base64.b64decode(current["value_base64"]) != value:
                    prepared.append((parameter_id, value))
            generation = expected_generation
            applied = 0
            try:
                for parameter_id, value in prepared:
                    argument = "hex:" + value.hex()
                    result = self._run(
                        "param-set-cas", f"0x{parameter_id:04x}",
                        str(generation), argument)
                    match = _STATUS.fullmatch(result)
                    if not match:
                        raise DeviceParameterError("写入结果格式无效")
                    generation += 1
                    applied += 1
                    if int(match.group(2)) != generation or match.group(5) != "no":
                        raise DeviceParameterError("写入后代数或维护锁状态异常")
            except DeviceParameterError as error:
                raise DeviceParameterError(
                    f"设备参数恢复在完成{applied}项后中止；"
                    f"当前预期代数为{generation}，请重新读取：{error}") from error

"""通过现有 remote-cli/libremotebsp 边界读取 toolbusd 的 Runtime Provider。"""

from __future__ import annotations

import copy
import json
import re
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Callable, Protocol, Sequence

from .models import normalize_snapshot
from .provider import RuntimeProvider, RuntimeProviderError, SnapshotRead


_UUID = re.compile(r"^[0-9a-fA-F]{32}$")
_RESOURCE_KINDS = {
    "gpio", "uart", "spi", "i2c", "adc", "pwm", "timer", "storage",
    "stepgen-axis", "timed-bitstream", "i2c-bus", "i2c-device",
    "spi-bus", "spi-device", "stream",
}
_HEALTH_VALUES = {
    "normal": 0,
    "busy": 1,
    "degraded": 2,
    "failed": 3,
    "disabled": 4,
}
_TRAFFIC_CLASSES = (
    "safety", "motion", "system", "interactive", "streaming", "bulk")


class ToolbusIpcError(RuntimeError):
    """toolbusd 本地 IPC 调用或适配失败。"""


class ToolbusIpcProtocolError(ToolbusIpcError):
    """remote-cli 输出不符合当前已知契约。"""


class ToolbusIpcClient(Protocol):
    """Provider 所需的 libremotebsp 只读操作集合。"""

    def traffic_status(self) -> dict: ...

    def list_nodes(self) -> list[dict]: ...

    def list_resources(self, node_id: int) -> list[dict]: ...

    def resource_status(self, node_id: int, resource_id: int) -> dict: ...

    def runtime_snapshot(self, maximum_resources: int) -> dict: ...


CommandRunner = Callable[[Sequence[str], float, int], str]


def _run_remote_cli(command: Sequence[str], timeout_seconds: float,
                    maximum_output_bytes: int) -> str:
    try:
        completed = subprocess.run(
            list(command), check=False, capture_output=True,
            timeout=timeout_seconds)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ToolbusIpcError(f"remote-cli调用失败：{error}") from error
    if len(completed.stdout) > maximum_output_bytes or \
            len(completed.stderr) > maximum_output_bytes:
        raise ToolbusIpcProtocolError("remote-cli输出超过允许上限")
    stdout = completed.stdout.decode("utf-8", errors="strict")
    stderr = completed.stderr.decode("utf-8", errors="replace").strip()
    if completed.returncode != 0:
        detail = stderr or f"退出码{completed.returncode}"
        raise ToolbusIpcError(f"remote-cli返回失败：{detail}")
    return stdout


def _parse_fields(line: str, command: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split():
        key, separator, value = token.partition("=")
        if not separator or not key or not value or key in fields:
            raise ToolbusIpcProtocolError(
                f"{command}输出字段无效：{line}")
        fields[key] = value
    if not fields:
        raise ToolbusIpcProtocolError(f"{command}包含空输出行")
    return fields


def _required(fields: dict[str, str], names: set[str], command: str) -> None:
    missing = names - fields.keys()
    if missing:
        raise ToolbusIpcProtocolError(
            f"{command}缺少字段：{','.join(sorted(missing))}")


def _integer(text: str, name: str, *, minimum: int = 0,
             maximum: int = 0xFFFFFFFF) -> int:
    try:
        value = int(text, 0)
    except ValueError as error:
        raise ToolbusIpcProtocolError(f"{name}不是整数") from error
    if value < minimum or value > maximum:
        raise ToolbusIpcProtocolError(f"{name}超出允许范围")
    return value


def _json_object(value: object, name: str) -> dict:
    if not isinstance(value, dict):
        raise ToolbusIpcProtocolError(f"{name}必须是JSON对象")
    return value


def _json_array(value: object, name: str) -> list:
    if not isinstance(value, list):
        raise ToolbusIpcProtocolError(f"{name}必须是JSON数组")
    return value


def _json_integer(value: object, name: str, *, minimum: int = 0,
                  maximum: int = 0xFFFFFFFF) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or \
            value < minimum or value > maximum:
        raise ToolbusIpcProtocolError(f"{name}不是允许范围内的JSON整数")
    return value


def _json_string(value: object, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise ToolbusIpcProtocolError(f"{name}必须是非空JSON字符串")
    return value


def _json_boolean(value: object, name: str) -> bool:
    if not isinstance(value, bool):
        raise ToolbusIpcProtocolError(f"{name}必须是JSON布尔值")
    return value


def _exact_fields(value: dict, expected: set[str], name: str) -> None:
    actual = set(value)
    if actual != expected:
        missing = expected - actual
        extra = actual - expected
        detail = []
        if missing:
            detail.append("缺少" + ",".join(sorted(missing)))
        if extra:
            detail.append("未知" + ",".join(sorted(extra)))
        raise ToolbusIpcProtocolError(f"{name}字段不匹配：{'；'.join(detail)}")


def _json_pairs(pairs: list[tuple[str, object]]) -> dict:
    result: dict[str, object] = {}
    for name, value in pairs:
        if name in result:
            raise ToolbusIpcProtocolError(f"JSON包含重复字段：{name}")
        result[name] = value
    return result


def _reject_json_constant(value: str) -> object:
    raise ToolbusIpcProtocolError(f"JSON包含非标准数值：{value}")


class RemoteCliIpcClient:
    """以无 shell 子进程调用现有 remote-cli 的只读 libremotebsp API。"""

    def __init__(self, socket_path: Path,
                 executable: str | Path = "remote-cli", *,
                 timeout_seconds: float = 2.0,
                 maximum_output_bytes: int = 1024 * 1024,
                 structured_output: bool = True,
                 runner: CommandRunner = _run_remote_cli):
        if not str(socket_path) or "\x00" in str(socket_path):
            raise ValueError("toolbusd套接字路径无效")
        if timeout_seconds <= 0:
            raise ValueError("IPC超时必须大于0")
        if maximum_output_bytes < 1:
            raise ValueError("IPC输出上限必须大于0")
        self.socket_path = socket_path
        self.executable = str(executable)
        self.timeout_seconds = timeout_seconds
        self.maximum_output_bytes = maximum_output_bytes
        self.structured_output = structured_output
        self.runner = runner

    def _run(self, operation: str, *, node_id: int | None = None,
             arguments: Sequence[str] = ()) -> str:
        command = [self.executable, "--socket", str(self.socket_path)]
        if self.structured_output:
            command.insert(1, "--json")
        if node_id is not None:
            if node_id < 1 or node_id > 127:
                raise ToolbusIpcProtocolError("目标节点ID必须位于1～127")
            command.extend(("--node", str(node_id)))
        command.append(operation)
        command.extend(arguments)
        try:
            output = self.runner(
                command, self.timeout_seconds, self.maximum_output_bytes)
        except UnicodeDecodeError as error:
            raise ToolbusIpcProtocolError(
                "remote-cli输出不是UTF-8") from error
        if not isinstance(output, str):
            raise ToolbusIpcProtocolError("remote-cli执行器必须返回字符串")
        if len(output.encode("utf-8")) > self.maximum_output_bytes:
            raise ToolbusIpcProtocolError("remote-cli输出超过允许上限")
        return output

    @staticmethod
    def _lines(output: str, command: str) -> list[dict[str, str]]:
        return [_parse_fields(line, command)
                for line in output.splitlines() if line.strip()]

    @staticmethod
    def _document(output: str, command: str) -> dict:
        try:
            root = _json_object(json.loads(
                output, object_pairs_hook=_json_pairs,
                parse_constant=_reject_json_constant), command)
        except (json.JSONDecodeError, RecursionError) as error:
            raise ToolbusIpcProtocolError(
                f"{command}输出不是合法JSON：{error}") from error
        _exact_fields(root, {"schema_version", "command", "data"}, command)
        version = _json_integer(
            root["schema_version"], command + ".schema_version",
            minimum=1, maximum=0xFFFFFFFF)
        if version != 1:
            raise ToolbusIpcProtocolError(
                f"{command}结构化输出schema_version不受支持：{version}")
        returned_command = _json_string(
            root["command"], command + ".command")
        if returned_command != command:
            raise ToolbusIpcProtocolError(
                f"{command}结构化输出命令不匹配：{returned_command}")
        return _json_object(root["data"], command + ".data")

    @staticmethod
    def _json_traffic(output: str) -> dict:
        data = RemoteCliIpcClient._document(output, "traffic-status")
        _exact_fields(data, {"traffic"}, "traffic-status.data")
        traffic = _json_object(data["traffic"], "traffic-status.data.traffic")
        numeric_limits = {
            "arbitration_bitrate": 0xFFFFFFFF,
            "data_bitrate": 0xFFFFFFFF,
            "max_utilization_permille": 1000,
            "burst_window_ms": 0xFFFFFFFF,
            "available_permille": 1000,
            "admitted_packets": 0xFFFFFFFFFFFFFFFF,
            "rejected_packets": 0xFFFFFFFFFFFFFFFF,
            "guaranteed_overruns": 0xFFFFFFFFFFFFFFFF,
            "admitted_frames": 0xFFFFFFFFFFFFFFFF,
            "estimated_wire_time_ns": 0xFFFFFFFFFFFFFFFF,
        }
        _exact_fields(traffic, set(numeric_limits) | {"mode", "classes"},
                      "traffic-status.data.traffic")
        mode = _json_string(traffic["mode"], "traffic-status.mode")
        if mode not in {"classical", "fd", "usb"}:
            raise ToolbusIpcProtocolError(
                f"traffic-status返回未知链路模式：{mode}")
        result: dict[str, object] = {"mode": mode}
        for name, maximum in numeric_limits.items():
            result[name] = _json_integer(
                traffic[name], "traffic-status." + name,
                maximum=maximum)
        classes = _json_array(traffic["classes"], "traffic-status.classes")
        if len(classes) != len(_TRAFFIC_CLASSES):
            raise ToolbusIpcProtocolError("traffic-status.classes数量无效")
        normalized_classes = []
        class_fields = {
            "class", "admitted_packets", "rejected_packets",
            "admitted_frames", "estimated_wire_time_ns",
        }
        for index, raw_class in enumerate(classes):
            item = _json_object(raw_class, f"traffic-status.classes[{index}]")
            _exact_fields(item, class_fields,
                          f"traffic-status.classes[{index}]")
            if item["class"] != _TRAFFIC_CLASSES[index]:
                raise ToolbusIpcProtocolError(
                    "traffic-status.classes顺序或名称无效")
            normalized = {"class": item["class"]}
            for name in class_fields - {"class"}:
                normalized[name] = _json_integer(
                    item[name], f"traffic-status.classes[{index}].{name}",
                    maximum=0xFFFFFFFFFFFFFFFF)
            normalized_classes.append(normalized)
        result["classes"] = normalized_classes
        return result

    @staticmethod
    def _json_nodes(output: str) -> list[dict]:
        data = RemoteCliIpcClient._document(output, "node-list")
        _exact_fields(data, {"nodes"}, "node-list.data")
        nodes = []
        expected = {
            "node_id", "online", "ready", "board_type", "firmware",
            "protocol_version", "uuid",
        }
        for index, raw_node in enumerate(_json_array(
                data["nodes"], "node-list.data.nodes")):
            item = _json_object(raw_node, f"node-list.nodes[{index}]")
            _exact_fields(item, expected, f"node-list.nodes[{index}]")
            firmware = _json_object(
                item["firmware"], f"node-list.nodes[{index}].firmware")
            _exact_fields(firmware, {"major", "minor", "patch"},
                          f"node-list.nodes[{index}].firmware")
            uuid = _json_string(item["uuid"], f"node-list.nodes[{index}].uuid")
            if not _UUID.fullmatch(uuid):
                raise ToolbusIpcProtocolError(
                    "node-list.uuid必须为16字节十六进制")
            nodes.append({
                "node_id": _json_integer(
                    item["node_id"], f"node-list.nodes[{index}].node_id",
                    minimum=1, maximum=127),
                "online": _json_boolean(
                    item["online"], f"node-list.nodes[{index}].online"),
                "ready": _json_boolean(
                    item["ready"], f"node-list.nodes[{index}].ready"),
                "board_type": _json_integer(
                    item["board_type"], f"node-list.nodes[{index}].board_type"),
                "firmware": tuple(_json_integer(
                    firmware[name], f"node-list.firmware.{name}",
                    maximum=0xFFFF) for name in ("major", "minor", "patch")),
                "protocol_version": _json_integer(
                    item["protocol_version"],
                    f"node-list.nodes[{index}].protocol_version", maximum=0xFF),
                "uuid": uuid.lower(),
            })
        return nodes

    @staticmethod
    def _json_resources(output: str, node_id: int) -> list[dict]:
        data = RemoteCliIpcClient._document(output, "resource-list")
        _exact_fields(data, {"node_id", "resources"}, "resource-list.data")
        returned_node = _json_integer(
            data["node_id"], "resource-list.data.node_id",
            minimum=1, maximum=127)
        if returned_node != node_id:
            raise ToolbusIpcProtocolError("resource-list返回了错误的节点ID")
        resources = []
        expected = {
            "resource_id", "type", "instance", "source",
            "rx_capacity", "tx_capacity",
        }
        for index, raw_resource in enumerate(_json_array(
                data["resources"], "resource-list.data.resources")):
            item = _json_object(raw_resource,
                                f"resource-list.resources[{index}]")
            _exact_fields(item, expected,
                          f"resource-list.resources[{index}]")
            kind = _json_string(
                item["type"], f"resource-list.resources[{index}].type")
            source = _json_string(
                item["source"], f"resource-list.resources[{index}].source")
            if kind not in _RESOURCE_KINDS:
                raise ToolbusIpcProtocolError(
                    f"resource-list返回未知资源类型：{kind}")
            if source not in {"native", "expanded"}:
                raise ToolbusIpcProtocolError(
                    f"resource-list返回未知资源来源：{source}")
            resources.append({
                "resource_id": _json_integer(
                    item["resource_id"], "resource-list.resource_id",
                    minimum=1),
                "kind": kind.replace("-", "_"),
                "instance": _json_integer(
                    item["instance"], "resource-list.instance",
                    maximum=0xFFFF),
                "source": source,
                "rx_capacity": _json_integer(
                    item["rx_capacity"], "resource-list.rx_capacity"),
                "tx_capacity": _json_integer(
                    item["tx_capacity"], "resource-list.tx_capacity"),
            })
        return resources

    @staticmethod
    def _json_resource_status(output: str, node_id: int,
                              resource_id: int) -> dict:
        data = RemoteCliIpcClient._document(output, "resource-status")
        _exact_fields(data, {"node_id", "resource"}, "resource-status.data")
        returned_node = _json_integer(
            data["node_id"], "resource-status.data.node_id",
            minimum=1, maximum=127)
        if returned_node != node_id:
            raise ToolbusIpcProtocolError("resource-status返回了错误的节点ID")
        item = _json_object(data["resource"], "resource-status.data.resource")
        expected = {
            "resource_id", "health", "health_name", "error_flags",
            "rx_buffered", "tx_buffered", "rx_overruns", "tx_overruns",
        }
        _exact_fields(item, expected, "resource-status.data.resource")
        health_name = _json_string(
            item["health_name"], "resource-status.health_name")
        if health_name not in _HEALTH_VALUES:
            raise ToolbusIpcProtocolError(
                f"resource-status返回未知健康状态：{health_name}")
        health = _json_integer(
            item["health"], "resource-status.health", maximum=0xFF)
        if health != _HEALTH_VALUES[health_name]:
            raise ToolbusIpcProtocolError("resource-status健康状态字段不一致")
        result: dict[str, object] = {"health": health_name}
        for name in expected - {"health", "health_name"}:
            result[name] = _json_integer(
                item[name], "resource-status." + name, maximum=0xFFFFFFFF)
        if result["resource_id"] != resource_id:
            raise ToolbusIpcProtocolError("resource-status返回了错误的资源ID")
        return result

    @staticmethod
    def _json_runtime_snapshot(output: str) -> dict:
        data = RemoteCliIpcClient._document(output, "runtime-snapshot")
        _exact_fields(data, {
            "snapshot_version", "snapshot_sequence", "traffic", "nodes",
            "resources", "node_issues",
        }, "runtime-snapshot.data")
        version = _json_integer(
            data["snapshot_version"], "runtime-snapshot.snapshot_version",
            minimum=1, maximum=0xFFFF)
        if version != 1:
            raise ToolbusIpcProtocolError(
                f"runtime-snapshot版本不受支持：{version}")
        sequence = _json_integer(
            data["snapshot_sequence"], "runtime-snapshot.snapshot_sequence",
            minimum=1, maximum=0xFFFFFFFFFFFFFFFF)
        envelope = lambda command, nested: json.dumps({
            "schema_version": 1, "command": command, "data": nested,
        }, separators=(",", ":"))
        traffic = RemoteCliIpcClient._json_traffic(envelope(
            "traffic-status", {"traffic": data["traffic"]}))
        nodes = RemoteCliIpcClient._json_nodes(envelope(
            "node-list", {"nodes": data["nodes"]}))
        node_ids = {int(node["node_id"]) for node in nodes}
        if len(node_ids) != len(nodes):
            raise ToolbusIpcProtocolError(
                "runtime-snapshot包含重复节点ID")

        raw_resources = _json_array(
            data["resources"], "runtime-snapshot.resources")
        if len(raw_resources) > 128:
            raise ToolbusIpcProtocolError(
                "runtime-snapshot资源数量超过128项")
        resources = []
        identities: set[tuple[int, int]] = set()
        for index, raw in enumerate(raw_resources):
            item = _json_object(raw, f"runtime-snapshot.resources[{index}]")
            _exact_fields(item, {
                "node_id", "status_valid", "descriptor", "status",
            }, f"runtime-snapshot.resources[{index}]")
            node_id = _json_integer(
                item["node_id"], "runtime-snapshot.resource.node_id",
                minimum=1, maximum=127)
            if node_id not in node_ids:
                raise ToolbusIpcProtocolError(
                    "runtime-snapshot资源引用未知节点")
            descriptor = RemoteCliIpcClient._json_resources(envelope(
                "resource-list", {
                    "node_id": node_id, "resources": [item["descriptor"]],
                }), node_id)[0]
            resource_id = int(descriptor["resource_id"])
            identity = (node_id, resource_id)
            if identity in identities:
                raise ToolbusIpcProtocolError(
                    "runtime-snapshot包含重复资源")
            identities.add(identity)
            status = RemoteCliIpcClient._json_resource_status(envelope(
                "resource-status", {
                    "node_id": node_id, "resource": item["status"],
                }), node_id, resource_id)
            resources.append({
                "node_id": node_id,
                "status_valid": _json_boolean(
                    item["status_valid"],
                    "runtime-snapshot.resource.status_valid"),
                "descriptor": descriptor,
                "status": status,
            })

        issues = []
        issue_nodes: set[int] = set()
        for index, raw in enumerate(_json_array(
                data["node_issues"], "runtime-snapshot.node_issues")):
            item = _json_object(raw, f"runtime-snapshot.node_issues[{index}]")
            _exact_fields(item, {"node_id", "code"},
                          f"runtime-snapshot.node_issues[{index}]")
            node_id = _json_integer(
                item["node_id"], "runtime-snapshot.issue.node_id",
                minimum=1, maximum=127)
            code = _json_integer(
                item["code"], "runtime-snapshot.issue.code",
                minimum=1, maximum=1)
            if node_id not in node_ids or node_id in issue_nodes:
                raise ToolbusIpcProtocolError(
                    "runtime-snapshot节点错误项重复或引用未知节点")
            issue_nodes.add(node_id)
            issues.append({"node_id": node_id, "code": code})
        return {
            "version": version, "sequence": sequence, "traffic": traffic,
            "nodes": nodes, "resources": resources, "node_issues": issues,
        }

    def traffic_status(self) -> dict:
        output = self._run("traffic-status")
        if self.structured_output:
            return self._json_traffic(output)
        lines = self._lines(output, "traffic-status")
        if not lines:
            raise ToolbusIpcProtocolError("traffic-status没有汇总行")
        fields = lines[0]
        required = {
            "mode", "arbitration_bitrate", "data_bitrate",
            "max_utilization_permille", "burst_window_ms",
            "available_permille", "admitted_packets", "rejected_packets",
            "guaranteed_overruns", "admitted_frames",
            "estimated_wire_time_ns",
        }
        _required(fields, required, "traffic-status")
        mode = fields["mode"]
        if mode not in {"classical", "fd", "usb"}:
            raise ToolbusIpcProtocolError(
                f"traffic-status返回未知链路模式：{mode}")
        result: dict[str, int | str] = {"mode": mode}
        for name in required - {"mode"}:
            result[name] = _integer(fields[name], "traffic-status." + name,
                                    maximum=0xFFFFFFFFFFFFFFFF)
        return result

    def list_nodes(self) -> list[dict]:
        output = self._run("node-list")
        if self.structured_output:
            return self._json_nodes(output)
        lines = self._lines(output, "node-list")
        nodes: list[dict] = []
        for fields in lines:
            _required(fields, {
                "node_id", "online", "ready", "board_type", "firmware",
                "protocol_version", "uuid",
            }, "node-list")
            online = _integer(fields["online"], "node-list.online", maximum=1)
            ready = _integer(fields["ready"], "node-list.ready", maximum=1)
            uuid = fields["uuid"].lower()
            if not _UUID.fullmatch(uuid):
                raise ToolbusIpcProtocolError("node-list.uuid必须为16字节十六进制")
            version_parts = fields["firmware"].split(".")
            if len(version_parts) != 3:
                raise ToolbusIpcProtocolError("node-list.firmware版本格式无效")
            nodes.append({
                "node_id": _integer(fields["node_id"], "node-list.node_id",
                                    minimum=1, maximum=127),
                "online": bool(online),
                "ready": bool(ready),
                "board_type": _integer(
                    fields["board_type"], "node-list.board_type"),
                "firmware": tuple(_integer(
                    part, "node-list.firmware", maximum=0xFFFF)
                    for part in version_parts),
                "protocol_version": _integer(
                    fields["protocol_version"], "node-list.protocol_version",
                    maximum=0xFF),
                "uuid": uuid,
            })
        return nodes

    def list_resources(self, node_id: int) -> list[dict]:
        output = self._run("resource-list", node_id=node_id)
        if self.structured_output:
            return self._json_resources(output, node_id)
        lines = self._lines(output, "resource-list")
        resources: list[dict] = []
        for fields in lines:
            _required(fields, {
                "resource_id", "type", "instance", "source",
                "rx_capacity", "tx_capacity",
            }, "resource-list")
            kind = fields["type"]
            if kind not in _RESOURCE_KINDS:
                raise ToolbusIpcProtocolError(
                    f"resource-list返回未知资源类型：{kind}")
            source = fields["source"]
            if source not in {"native", "expanded"}:
                raise ToolbusIpcProtocolError(
                    f"resource-list返回未知资源来源：{source}")
            resources.append({
                "resource_id": _integer(
                    fields["resource_id"], "resource-list.resource_id",
                    minimum=1),
                "kind": kind.replace("-", "_"),
                "instance": _integer(
                    fields["instance"], "resource-list.instance",
                    maximum=0xFFFF),
                "source": source,
                "rx_capacity": _integer(
                    fields["rx_capacity"], "resource-list.rx_capacity"),
                "tx_capacity": _integer(
                    fields["tx_capacity"], "resource-list.tx_capacity"),
            })
        return resources

    def resource_status(self, node_id: int, resource_id: int) -> dict:
        output = self._run(
            "resource-status", node_id=node_id,
            arguments=(str(resource_id),))
        if self.structured_output:
            return self._json_resource_status(output, node_id, resource_id)
        lines = self._lines(output, "resource-status")
        if len(lines) != 1:
            raise ToolbusIpcProtocolError(
                "resource-status必须恰好返回一行")
        fields = lines[0]
        required = {
            "resource_id", "health", "error_flags", "health_name",
            "rx_buffered", "tx_buffered", "rx_overruns", "tx_overruns",
        }
        _required(fields, required, "resource-status")
        health_name = fields["health_name"]
        if health_name not in _HEALTH_VALUES:
            raise ToolbusIpcProtocolError(
                f"resource-status返回未知健康状态：{health_name}")
        health = _integer(fields["health"], "resource-status.health",
                          maximum=0xFF)
        if health != _HEALTH_VALUES[health_name]:
            raise ToolbusIpcProtocolError("resource-status健康状态字段不一致")
        result: dict[str, int | str] = {"health": health_name}
        for name in required - {"health", "health_name"}:
            result[name] = _integer(fields[name], "resource-status." + name,
                                    maximum=0xFFFFFFFF)
        if result["resource_id"] != resource_id:
            raise ToolbusIpcProtocolError("resource-status返回了错误的资源ID")
        return result

    def runtime_snapshot(self, maximum_resources: int) -> dict:
        if maximum_resources < 1 or maximum_resources > 128:
            raise ToolbusIpcProtocolError(
                "Runtime快照资源上限必须位于1～128")
        process_timeout_ms = int(self.timeout_seconds * 1000)
        headroom_ms = min(100, max(1, process_timeout_ms // 10))
        timeout_ms = min(
            5000, max(1, process_timeout_ms - headroom_ms))
        output = self._run(
            "runtime-snapshot",
            arguments=(str(maximum_resources), str(timeout_ms)))
        if not self.structured_output:
            raise ToolbusIpcProtocolError(
                "Runtime单次快照要求结构化remote-cli输出")
        return self._json_runtime_snapshot(output)


class _RuntimeSnapshotView:
    """把单次 IPC 结果适配为既有快照组装接口，不再启动子进程。"""

    def __init__(self, snapshot: dict):
        self.snapshot = snapshot
        self._resources: dict[int, list[dict]] = {}
        self._statuses: dict[tuple[int, int], tuple[bool, dict]] = {}
        self._inventory_failures = {
            int(issue["node_id"]) for issue in snapshot["node_issues"]
            if int(issue["code"]) == 1
        }
        for item in snapshot["resources"]:
            node_id = int(item["node_id"])
            descriptor = item["descriptor"]
            resource_id = int(descriptor["resource_id"])
            self._resources.setdefault(node_id, []).append(descriptor)
            self._statuses[(node_id, resource_id)] = (
                bool(item["status_valid"]), item["status"])

    def traffic_status(self) -> dict:
        return self.snapshot["traffic"]

    def list_nodes(self) -> list[dict]:
        return self.snapshot["nodes"]

    def list_resources(self, node_id: int) -> list[dict]:
        if node_id in self._inventory_failures:
            raise ToolbusIpcError("单次快照中该节点资源目录不可用")
        return self._resources.get(node_id, [])

    def resource_status(self, node_id: int, resource_id: int) -> dict:
        valid, status = self._statuses[(node_id, resource_id)]
        if not valid:
            raise ToolbusIpcError("单次快照中该资源状态不可用")
        return status


class ToolbusdSnapshotProvider(RuntimeProvider):
    """将现有 libremotebsp 只读调用聚合为一次 Runtime v1 快照。"""

    def __init__(self, client: ToolbusIpcClient,
                 *, clock_ms: Callable[[], int] | None = None,
                 maximum_resources_per_snapshot: int = 128,
                 cache_ttl_ms: int = 250,
                 maximum_concurrent_status_queries: int = 8,
                 refresh_wait_timeout_ms: int = 5000):
        if maximum_resources_per_snapshot < 1:
            raise ValueError("每次快照资源查询上限必须大于0")
        if cache_ttl_ms < 0 or cache_ttl_ms > 60_000:
            raise ValueError("快照缓存时间必须位于0～60000毫秒")
        if maximum_concurrent_status_queries < 1 or \
                maximum_concurrent_status_queries > 32:
            raise ValueError("资源状态查询并发必须位于1～32")
        if refresh_wait_timeout_ms < 1 or refresh_wait_timeout_ms > 60_000:
            raise ValueError("快照刷新等待时间必须位于1～60000毫秒")
        self.client = client
        self.clock_ms = clock_ms or (lambda: time.monotonic_ns() // 1_000_000)
        self.maximum_resources_per_snapshot = maximum_resources_per_snapshot
        self.cache_ttl_ms = cache_ttl_ms
        self.maximum_concurrent_status_queries = \
            maximum_concurrent_status_queries
        self.refresh_wait_timeout_ms = refresh_wait_timeout_ms
        self._cache_condition = threading.Condition()
        self._cached_snapshot: dict | None = None
        self._cache_stored_at_ms: int | None = None
        self._cached_error: str | None = None
        self._error_stored_at_ms: int | None = None
        self._refreshing = False

    @staticmethod
    def _link_kind(mode: str) -> str:
        return {"classical": "can", "fd": "can_fd", "usb": "usb_bulk"}[
            mode]

    @staticmethod
    def _alert(node_id: str, alert_id: str, code: str, message: str,
               captured_at_ms: int, *, resource_id: str | None = None,
               severity: str = "error") -> dict:
        return {
            "alert_id": alert_id,
            "node_id": node_id,
            "resource_id": resource_id,
            "severity": severity,
            "code": code,
            "message": message,
            "active": True,
            "occurred_at_ms": captured_at_ms,
        }

    def _read_resource_status(self, client: ToolbusIpcClient, node_id: int,
                              descriptor: dict) -> tuple[dict | None,
                                                         ToolbusIpcError | None]:
        try:
            return client.resource_status(
                node_id, int(descriptor["resource_id"])), None
        except ToolbusIpcProtocolError:
            raise
        except ToolbusIpcError as error:
            return None, error

    def get_snapshot(self) -> dict:
        return self.read_snapshot().snapshot

    def read_snapshot(self) -> SnapshotRead:
        """合并并发刷新，并返回缓存年龄；刷新失败不提供陈旧回退。"""
        now_ms = self._clock_value()
        with self._cache_condition:
            cached = self._cached_read(now_ms)
            if cached is not None:
                return cached
            self._raise_cached_failure(now_ms)
            if self._refreshing:
                deadline = time.monotonic() + \
                    self.refresh_wait_timeout_ms / 1000.0
                while self._refreshing:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise RuntimeProviderError(
                            "等待toolbusd快照刷新超时")
                    self._cache_condition.wait(remaining)
                now_ms = self._clock_value()
                cached = self._cached_read(now_ms)
                if cached is not None:
                    return cached
                self._raise_cached_failure(now_ms)
            self._refreshing = True
            self._cached_error = None
            self._error_stored_at_ms = None

        try:
            snapshot = self._build_snapshot()
            stored_at_ms = self._clock_value()
        except RuntimeProviderError as error:
            self._finish_failed_refresh(str(error), now_ms)
            raise
        except (KeyError, TypeError, ValueError,
                ToolbusIpcError) as error:
            converted = RuntimeProviderError(
                f"toolbusd IPC数据无法转换为Runtime快照：{error}")
            self._finish_failed_refresh(str(converted), now_ms)
            raise converted from error

        with self._cache_condition:
            self._cached_snapshot = copy.deepcopy(snapshot)
            self._cache_stored_at_ms = stored_at_ms
            self._cached_error = None
            self._error_stored_at_ms = None
            self._refreshing = False
            self._cache_condition.notify_all()
        return SnapshotRead(
            snapshot=copy.deepcopy(snapshot),
            cache_status="refresh",
            age_ms=max(0, stored_at_ms - int(snapshot["captured_at_ms"])),
            cache_ttl_ms=self.cache_ttl_ms,
        )

    def _clock_value(self) -> int:
        value = self.clock_ms()
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise RuntimeProviderError("Runtime时钟返回值无效")
        return value

    def _cached_read(self, now_ms: int) -> SnapshotRead | None:
        if self._cached_snapshot is None or self._cache_stored_at_ms is None:
            return None
        if now_ms < self._cache_stored_at_ms:
            return None
        age_ms = now_ms - self._cache_stored_at_ms
        if self.cache_ttl_ms == 0 or age_ms > self.cache_ttl_ms:
            return None
        return SnapshotRead(
            snapshot=copy.deepcopy(self._cached_snapshot),
            cache_status="hit",
            age_ms=max(0, now_ms - int(
                self._cached_snapshot["captured_at_ms"])),
            cache_ttl_ms=self.cache_ttl_ms,
        )

    def _raise_cached_failure(self, now_ms: int) -> None:
        if self._cached_error is None or self._error_stored_at_ms is None:
            return
        if now_ms < self._error_stored_at_ms:
            self._cached_error = None
            self._error_stored_at_ms = None
            return
        age_ms = now_ms - self._error_stored_at_ms
        if self.cache_ttl_ms != 0 and age_ms <= self.cache_ttl_ms:
            raise RuntimeProviderError(self._cached_error)
        self._cached_error = None
        self._error_stored_at_ms = None

    def _finish_failed_refresh(self, message: str,
                               fallback_stored_at_ms: int) -> None:
        try:
            stored_at_ms = self._clock_value()
        except RuntimeProviderError:
            # 保留最初的 IPC 错误，同时确保等待者一定被唤醒。
            stored_at_ms = fallback_stored_at_ms
        with self._cache_condition:
            self._cached_error = message
            self._error_stored_at_ms = stored_at_ms
            self._refreshing = False
            self._cache_condition.notify_all()

    def _build_snapshot(self) -> dict:
        captured_at_ms = self._clock_value()
        source_client: ToolbusIpcClient = self.client
        source_sequence: int | None = None
        snapshot_reader = getattr(self.client, "runtime_snapshot", None)
        structured_output = getattr(self.client, "structured_output", True)
        if callable(snapshot_reader) and structured_output:
            try:
                source = snapshot_reader(
                    self.maximum_resources_per_snapshot)
            except ToolbusIpcProtocolError as error:
                raise RuntimeProviderError(
                    f"toolbusd IPC协议不兼容：{error}") from error
            except ToolbusIpcError as error:
                raise RuntimeProviderError(
                    f"toolbusd IPC不可用：{error}") from error
            source_client = _RuntimeSnapshotView(source)
            source_sequence = int(source["sequence"])
            captured_at_ms = self._clock_value()
        try:
            traffic = source_client.traffic_status()
            source_nodes = source_client.list_nodes()
            link_kind = self._link_kind(str(traffic["mode"]))
        except (KeyError, ToolbusIpcError, ValueError) as error:
            raise RuntimeProviderError(f"toolbusd IPC不可用：{error}") from error

        numeric_node_ids: set[int] = set()
        node_uuids: set[str] = set()
        for source_node in source_nodes:
            numeric_id = int(source_node["node_id"])
            uuid = str(source_node["uuid"])
            if numeric_id < 1 or numeric_id > 127:
                raise RuntimeProviderError("toolbusd节点ID超出1～127范围")
            if numeric_id in numeric_node_ids:
                raise RuntimeProviderError(
                    f"toolbusd节点列表包含重复numeric node_id：{numeric_id}")
            if uuid in node_uuids:
                raise RuntimeProviderError(
                    f"toolbusd节点列表包含重复UUID：{uuid}")
            numeric_node_ids.add(numeric_id)
            node_uuids.add(uuid)

        nodes: list[dict] = []
        alerts: list[dict] = []
        resource_query_count = 0
        for source_node in source_nodes:
            numeric_id = int(source_node["node_id"])
            uuid = str(source_node["uuid"])
            node_id = "node-" + uuid
            online = bool(source_node["online"])
            ready = bool(source_node["ready"])
            state = "online" if online and ready else (
                "degraded" if online else "offline")
            resources: list[dict] = []
            runtime_error: str | None = None
            if online and ready:
                try:
                    descriptors = source_client.list_resources(numeric_id)
                except ToolbusIpcProtocolError as error:
                    raise RuntimeProviderError(
                        f"toolbusd IPC协议不兼容：{error}") from error
                except ToolbusIpcError as error:
                    state = "degraded"
                    runtime_error = str(error)
                    alerts.append(self._alert(
                        node_id, f"resource-enum-{numeric_id}",
                        "resource_inventory_unavailable",
                        f"资源目录读取失败：{error}", captured_at_ms))
                    descriptors = []
                resource_query_count += len(descriptors)
                if resource_query_count > self.maximum_resources_per_snapshot:
                    raise RuntimeProviderError(
                        "toolbusd资源数量超过单次快照查询上限")
                try:
                    with ThreadPoolExecutor(
                            max_workers=self.maximum_concurrent_status_queries,
                            thread_name_prefix="runtime-status") as executor:
                        status_results = list(executor.map(
                            lambda descriptor: self._read_resource_status(
                                source_client, numeric_id, descriptor),
                            descriptors))
                except ToolbusIpcProtocolError as error:
                    raise RuntimeProviderError(
                        f"toolbusd IPC协议不兼容：{error}") from error
                for descriptor, (status, status_error) in zip(
                        descriptors, status_results):
                    raw_resource_id = int(descriptor["resource_id"])
                    resource_id = f"resource-{raw_resource_id:08x}"
                    if status_error is None:
                        assert status is not None
                        health = str(status["health"])
                        available = health not in {"failed", "disabled"}
                    else:
                        error = status_error
                        health = "unknown"
                        available = False
                        state = "degraded"
                        status = {
                            "error_flags": 0,
                            "rx_buffered": 0,
                            "tx_buffered": 0,
                            "rx_overruns": 0,
                            "tx_overruns": 0,
                        }
                        alerts.append(self._alert(
                            node_id,
                            f"resource-status-{numeric_id}-"
                            f"{raw_resource_id:08x}",
                            "resource_status_unavailable",
                            f"资源{resource_id}状态读取失败：{error}",
                            captured_at_ms, resource_id=resource_id))
                    resources.append({
                        "resource_id": resource_id,
                        "kind": str(descriptor["kind"]),
                        "name": f"{descriptor['kind']} "
                                f"{descriptor['instance']}",
                        "available": available,
                        "state": {
                            "health": health,
                            "error_flags": status["error_flags"],
                            "rx_buffered": status["rx_buffered"],
                            "tx_buffered": status["tx_buffered"],
                            "rx_overruns": status["rx_overruns"],
                            "tx_overruns": status["tx_overruns"],
                            "instance": descriptor["instance"],
                            "source": descriptor["source"],
                            "rx_capacity": descriptor["rx_capacity"],
                            "tx_capacity": descriptor["tx_capacity"],
                        },
                    })
                    if health in {"degraded", "failed"} or \
                            int(status["error_flags"]) != 0:
                        state = "degraded"
                        severity = "error" if health == "failed" \
                            else "warning"
                        alerts.append(self._alert(
                            node_id,
                            f"resource-health-{numeric_id}-"
                            f"{raw_resource_id:08x}",
                            "resource_health",
                            f"资源{resource_id}健康状态为{health}",
                            captured_at_ms, resource_id=resource_id,
                            severity=severity))
            firmware = tuple(source_node["firmware"])
            nodes.append({
                "node_id": node_id,
                "board_type": f"board-0x{int(source_node['board_type']):08x}",
                "display_name": f"RemoteBSP 节点 {numeric_id}",
                "state": state,
                "last_seen_ms": captured_at_ms if online else 0,
                "links": [{
                    "kind": link_kind,
                    "state": "online" if online else "offline",
                }],
                "resources": resources,
                "runtime": {
                    "bus_node_id": numeric_id,
                    "uuid": uuid,
                    "ready": ready,
                    "last_seen_known": online,
                    "firmware_version": ".".join(str(part)
                                                 for part in firmware),
                    "protocol_version": int(source_node["protocol_version"]),
                    "resource_inventory_error": runtime_error,
                    "traffic": traffic,
                },
            })

        try:
            return normalize_snapshot({
                "schema_version": 1,
                "snapshot_id": (
                    f"toolbusd-{source_sequence}"
                    if source_sequence is not None
                    else f"toolbusd-{captured_at_ms}"),
                "captured_at_ms": captured_at_ms,
                "nodes": nodes,
                "alerts": alerts,
            })
        except (TypeError, ValueError) as error:
            raise RuntimeProviderError(
                f"toolbusd IPC数据无法转换为Runtime快照：{error}") from error

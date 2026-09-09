"""通过现有 remote-cli/libremotebsp 边界读取 toolbusd 的 Runtime Provider。"""

from __future__ import annotations

import re
import subprocess
import time
from pathlib import Path
from typing import Callable, Protocol, Sequence

from .models import normalize_snapshot
from .provider import RuntimeProvider, RuntimeProviderError


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


class RemoteCliIpcClient:
    """以无 shell 子进程调用现有 remote-cli 的只读 libremotebsp API。"""

    def __init__(self, socket_path: Path,
                 executable: str | Path = "remote-cli", *,
                 timeout_seconds: float = 2.0,
                 maximum_output_bytes: int = 1024 * 1024,
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
        self.runner = runner

    def _run(self, operation: str, *, node_id: int | None = None,
             arguments: Sequence[str] = ()) -> str:
        command = [self.executable, "--socket", str(self.socket_path)]
        if node_id is not None:
            if node_id < 1 or node_id > 127:
                raise ToolbusIpcProtocolError("目标节点ID必须位于1～127")
            command.extend(("--node", str(node_id)))
        command.append(operation)
        command.extend(arguments)
        try:
            return self.runner(
                command, self.timeout_seconds, self.maximum_output_bytes)
        except UnicodeDecodeError as error:
            raise ToolbusIpcProtocolError(
                "remote-cli输出不是UTF-8") from error

    @staticmethod
    def _lines(output: str, command: str) -> list[dict[str, str]]:
        return [_parse_fields(line, command)
                for line in output.splitlines() if line.strip()]

    def traffic_status(self) -> dict:
        lines = self._lines(self._run("traffic-status"), "traffic-status")
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
        lines = self._lines(self._run("node-list"), "node-list")
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
        lines = self._lines(
            self._run("resource-list", node_id=node_id), "resource-list")
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
        lines = self._lines(self._run(
            "resource-status", node_id=node_id,
            arguments=(str(resource_id),)), "resource-status")
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


class ToolbusdSnapshotProvider(RuntimeProvider):
    """将现有 libremotebsp 只读调用聚合为一次 Runtime v1 快照。"""

    def __init__(self, client: ToolbusIpcClient,
                 *, clock_ms: Callable[[], int] | None = None,
                 maximum_resources_per_snapshot: int = 128):
        if maximum_resources_per_snapshot < 1:
            raise ValueError("每次快照资源查询上限必须大于0")
        self.client = client
        self.clock_ms = clock_ms or (lambda: time.monotonic_ns() // 1_000_000)
        self.maximum_resources_per_snapshot = maximum_resources_per_snapshot

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

    def get_snapshot(self) -> dict:
        try:
            return self._build_snapshot()
        except RuntimeProviderError:
            raise
        except (KeyError, TypeError, ValueError,
                ToolbusIpcError) as error:
            raise RuntimeProviderError(
                f"toolbusd IPC数据无法转换为Runtime快照：{error}") from error

    def _build_snapshot(self) -> dict:
        captured_at_ms = self.clock_ms()
        if isinstance(captured_at_ms, bool) or not isinstance(
                captured_at_ms, int) or captured_at_ms < 0:
            raise RuntimeProviderError("Runtime时钟返回值无效")
        try:
            traffic = self.client.traffic_status()
            source_nodes = self.client.list_nodes()
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
                    descriptors = self.client.list_resources(numeric_id)
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
                for descriptor in descriptors:
                    raw_resource_id = int(descriptor["resource_id"])
                    resource_id = f"resource-{raw_resource_id:08x}"
                    try:
                        status = self.client.resource_status(
                            numeric_id, raw_resource_id)
                        health = str(status["health"])
                        available = health not in {"failed", "disabled"}
                    except ToolbusIpcProtocolError as error:
                        raise RuntimeProviderError(
                            f"toolbusd IPC协议不兼容：{error}") from error
                    except ToolbusIpcError as error:
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
                "snapshot_id": f"toolbusd-{captured_at_ms}",
                "captured_at_ms": captured_at_ms,
                "nodes": nodes,
                "alerts": alerts,
            })
        except (TypeError, ValueError) as error:
            raise RuntimeProviderError(
                f"toolbusd IPC数据无法转换为Runtime快照：{error}") from error

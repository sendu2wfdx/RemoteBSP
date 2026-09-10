"""通过现有 remote-cli/libremotebsp 边界读取 toolbusd 的 Runtime Provider。"""

from __future__ import annotations

import copy
import json
import re
import struct
import subprocess
import threading
import time
from concurrent.futures import (
    Future,
    ThreadPoolExecutor,
    TimeoutError as FutureTimeout,
)
from pathlib import Path
from typing import Callable, Protocol, Sequence

from .deadline import (
    MonotonicDeadline,
    RequestDeadlineExceeded,
    call_with_deadline,
)
from .models import normalize_snapshot
from .health_projection import (
    HealthProjectionError,
    TrustedNodeHealthProjection,
    TrustedToolbusdHealthProjection,
)
from .provider import (
    RuntimeProvider,
    RuntimeProviderError,
    RuntimeProviderOperationError,
    SnapshotRead,
)


_UUID = re.compile(r"^[0-9a-fA-F]{32}$")
_OPERATION_ID = re.compile(r"^[0-9a-f]{64}$")
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
_CONTROL_OPERATIONS = {
    "runtime-control-acquire", "runtime-gpio-write",
    "runtime-control-release", "runtime-gpio-write-operation",
    "runtime-control-release-operation",
    "runtime-pwm-acquire", "runtime-pwm-configure-operation",
    "runtime-pwm-stop-operation",
    "runtime-timed-bitstream-acquire",
    "runtime-timed-bitstream-configure-operation",
    "runtime-timed-bitstream-frame-operation",
    "runtime-timed-bitstream-stop-operation",
    "runtime-bus-reset-acquire", "runtime-bus-resource-reset-operation",
}
_OPERATION_BASE_FIELDS = {
    "operation_id", "lease_id", "expected_node_uuid", "resource_id",
    "kind", "state", "replayed", "recovery", "object_id", "value",
    "error_code",
}
_OPERATION_PWM_FIELDS = {"frequency_hz", "duty", "active_low"}
_OPERATION_KINDS = {"gpio_write", "control_release", "pwm_configure", "pwm_stop",
                    "timed_bitstream_configure", "timed_bitstream_frame",
                    "timed_bitstream_stop", "bus_resource_reset"}
_OPERATION_STATES = {
    "pending", "committed", "rejected", "unknown", "expired_unknown",
}
_OPERATION_RECOVERIES = {
    "none", "not_sent", "safe_closed", "scope_blocked",
    "awaiting_reboot", "node_reboot_confirmed",
}
_OPERATION_ERRORS = {
    "rejected", "deadline", "backend", "persistence", "history_expired",
}


class ToolbusIpcError(RuntimeError):
    """toolbusd 本地 IPC 调用或适配失败。"""


class ToolbusIpcProtocolError(ToolbusIpcError):
    """remote-cli 输出不符合当前已知契约。"""


class ToolbusIpcOperationError(ToolbusIpcError):
    """remote-cli v1 错误信封；数字码与 C++ IPC 合同保持一致。"""

    _ROOT_FIELDS = {"schema_version", "command", "error"}
    _FIELDS = {"ipc_error_version", "code", "category", "retryable",
               "possibly_committed", "message"}
    _CATEGORIES = {
        1: 1, 2: 1, 100: 2, 101: 3, 105: 3,
        102: 4, 103: 4, 104: 4, 107: 4, 109: 4,
        106: 5, 200: 5, 201: 5, 203: 5,
        202: 6, 108: 7, 255: 7,
    }
    _POSSIBLY_COMMITTED_CODES = {108, 201, 202, 255}
    _RETRYABLE_CODES = {102, 106, 200, 201, 202, 203}

    def __init__(self, code: int, category: int, retryable: bool,
                 possibly_committed: bool, message: str):
        if self._CATEGORIES.get(code) != category or type(retryable) is not bool \
                or type(possibly_committed) is not bool or \
                (possibly_committed and code not in
                 self._POSSIBLY_COMMITTED_CODES) or \
                (possibly_committed and retryable) or \
                (not possibly_committed and
                 retryable != (code in self._RETRYABLE_CODES)):
            raise ToolbusIpcProtocolError("remote-cli结构化错误字段无效")
        if not isinstance(message, str) or not message:
            raise ToolbusIpcProtocolError("remote-cli结构化错误消息无效")
        try:
            encoded_message = message.encode("utf-8", errors="strict")
        except UnicodeEncodeError as error:
            raise ToolbusIpcProtocolError(
                "remote-cli结构化错误消息不是有效UTF-8") from error
        if len(encoded_message) > 256 or any(
                ord(character) < 0x20 or ord(character) == 0x7f
                for character in message):
            raise ToolbusIpcProtocolError("remote-cli结构化错误消息无效")
        self.code = code
        self.category = category
        self.retryable = retryable
        self.possibly_committed = possibly_committed
        self.message = message
        super().__init__(message)

    @classmethod
    def from_document(cls, output: str, command: str
                      ) -> "ToolbusIpcOperationError":
        try:
            root = _json_object(json.loads(
                output, object_pairs_hook=_json_pairs,
                parse_constant=_reject_json_constant), command)
        except (json.JSONDecodeError, RecursionError) as error:
            raise ToolbusIpcProtocolError(
                f"{command}错误输出不是合法JSON") from error
        _exact_fields(root, cls._ROOT_FIELDS, command)
        if _json_integer(root["schema_version"], "schema_version",
                         minimum=1, maximum=0xffffffff) != 1 or \
                _json_string(root["command"], "command") != command:
            raise ToolbusIpcProtocolError("remote-cli错误输出版本或命令不匹配")
        error_root = _json_object(root["error"], "error")
        _exact_fields(error_root, cls._FIELDS, "error")
        if _json_integer(error_root["ipc_error_version"],
                         "error.ipc_error_version", minimum=1,
                         maximum=0xffff) != 1:
            raise ToolbusIpcProtocolError("remote-cli错误信封版本不受支持")
        return cls(
            _json_integer(error_root["code"], "error.code",
                          maximum=0xffff),
            _json_integer(error_root["category"], "error.category",
                          maximum=0xff),
            _json_boolean(error_root["retryable"], "error.retryable"),
            _json_boolean(error_root["possibly_committed"],
                          "error.possibly_committed"),
            _json_string(error_root["message"], "error.message"))


def _provider_operation_error(error: ToolbusIpcError
                              ) -> RuntimeProviderOperationError:
    if isinstance(error, ToolbusIpcOperationError):
        if error.code == 202:
            code, category = "deadline_exceeded", "timeout"
        elif error.code in {1, 2}:
            code, category = "protocol_incompatible", "protocol"
        elif error.code in {101, 102, 103, 104, 105, 107, 109, 200}:
            code, category = "target_rejected", "target"
        else:
            code, category = "backend_unavailable", "transport"
        return RuntimeProviderOperationError(
            code, category=category,
            retryable=error.retryable,
            possibly_committed=error.possibly_committed,
            invalidates_global_operational=error.code in {1, 2, 100})
    if isinstance(error, ToolbusIpcProtocolError):
        return RuntimeProviderOperationError(
            "protocol_incompatible", category="protocol",
            retryable=False, possibly_committed=True, detail=str(error),
            invalidates_global_operational=True)
    # 旧 CLI 只有字符串，不能按文本猜业务语义或提交状态。
    return RuntimeProviderOperationError(
        "backend_unavailable", category="transport",
        retryable=False, possibly_committed=True, detail=str(error),
        invalidates_global_operational=True)


class _RemoteCliProcessError(ToolbusIpcError):
    """保留非零退出的有界 stdout，交由知道命令名的上层严格解析。"""

    def __init__(self, stdout: str, detail: str):
        self.stdout = stdout
        super().__init__(detail)


class ToolbusIpcClient(Protocol):
    """Provider 所需的 libremotebsp 只读操作集合。"""

    def traffic_status(self) -> dict: ...

    def daemon_identity(self) -> str: ...

    def runtime_control_acquire(
            self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int,
            ttl_ms: int) -> None: ...

    def runtime_gpio_write(
            self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int,
            idempotency_key: str, value: bool) -> dict: ...

    def runtime_control_release(
            self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str) -> None: ...

    def runtime_gpio_write_operation(
            self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int, idempotency_key: str, value: bool) -> dict: ...

    def runtime_control_release_operation(
            self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str) -> dict: ...

    def runtime_pwm_acquire(self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int, ttl_ms: int) -> None: ...

    def runtime_pwm_configure_operation(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str,
            node_id: int, resource_id: int, idempotency_key: str,
            frequency_hz: int, duty: int, active_low: bool) -> dict: ...

    def runtime_pwm_stop_operation(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str,
            node_id: int, resource_id: int, idempotency_key: str) -> dict: ...

    def runtime_bus_reset_acquire(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str,
            node_id: int, resource_id: int, ttl_ms: int) -> None: ...

    def runtime_bus_resource_reset_operation(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str,
            node_id: int, resource_id: int,
            idempotency_key: str) -> dict: ...

    def runtime_operation_status(
            self, daemon_instance_id: str, owner_key_id: str,
            operation_id: str) -> dict: ...

    def runtime_operation_lookup(
            self, daemon_instance_id: str, owner_key_id: str,
            kind: str, lease_id: str, idempotency_key: str) -> dict: ...

    def list_nodes(self) -> list[dict]: ...

    def list_resources(self, node_id: int) -> list[dict]: ...

    def resource_status(self, node_id: int, resource_id: int) -> dict: ...

    def runtime_snapshot(self, maximum_resources: int) -> dict: ...

    def health_snapshot(self) -> dict: ...

    def node_health_snapshot(self, node_id: int) -> dict: ...


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
        raise _RemoteCliProcessError(
            stdout, f"remote-cli返回失败：{detail}")
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


def _json_nullable_integer(value: object, name: str, *, minimum: int = 0,
                           maximum: int = 0xFFFFFFFF) -> int | None:
    if value is None:
        return None
    return _json_integer(value, name, minimum=minimum, maximum=maximum)


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
             arguments: Sequence[str] = (),
             deadline: MonotonicDeadline | None = None) -> str:
        command = [self.executable, "--socket", str(self.socket_path)]
        if self.structured_output:
            command.insert(1, "--json")
        if node_id is not None:
            if node_id < 1 or node_id > 127:
                raise ToolbusIpcProtocolError("目标节点ID必须位于1～127")
            command.extend(("--node", str(node_id)))
        command.append(operation)
        command.extend(arguments)
        timeout_seconds = self.timeout_seconds if deadline is None else \
            deadline.remaining_seconds(cap=self.timeout_seconds)

        def check_after_runner() -> None:
            if deadline is None:
                return
            try:
                deadline.check()
            except RequestDeadlineExceeded as error:
                if operation in _CONTROL_OPERATIONS:
                    raise ToolbusIpcOperationError(
                        202, 6, False, True,
                        "控制命令已开始执行但未在期限内返回") from error
                raise

        try:
            output = self.runner(
                command, timeout_seconds, self.maximum_output_bytes)
        except _RemoteCliProcessError as error:
            if self.structured_output:
                # daemon 已返回的精确信封优先于事后期限，不能丢失提交语义。
                raise ToolbusIpcOperationError.from_document(
                    error.stdout, operation) from error
            raise ToolbusIpcError("remote-cli返回失败") from error
        except ToolbusIpcOperationError:
            raise
        except UnicodeDecodeError as error:
            raise ToolbusIpcProtocolError(
                "remote-cli输出不是UTF-8") from error
        except Exception:
            check_after_runner()
            raise
        if not isinstance(output, str):
            raise ToolbusIpcProtocolError("remote-cli执行器必须返回字符串")
        if len(output.encode("utf-8")) > self.maximum_output_bytes:
            raise ToolbusIpcProtocolError("remote-cli输出超过允许上限")
        check_after_runner()
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
    def _json_operation_outcome(output: str, command: str, *,
                                expected_kind: str | None = None,
                                expected_operation_id: str | None = None,
                                expected_value: bool | None = None,
                                allow_unknown_kind: bool = False) -> dict:
        """严格验证 operation ledger 的状态组合，镜像 C++ wire 合同。"""
        data = RemoteCliIpcClient._document(output, command)
        pwm_document = bool(_OPERATION_PWM_FIELDS & set(data))
        _exact_fields(data, _OPERATION_BASE_FIELDS |
                      (_OPERATION_PWM_FIELDS if pwm_document else set()),
                      command + ".data")
        operation_id = _json_string(
            data["operation_id"], command + ".operation_id")
        if _OPERATION_ID.fullmatch(operation_id) is None or \
                operation_id == "0" * 64:
            raise ToolbusIpcProtocolError("operation_id必须是非零64位小写十六进制")
        if expected_operation_id is not None and \
                operation_id != expected_operation_id:
            raise ToolbusIpcProtocolError("操作查询返回了错误的operation_id")
        raw_lease_id = data["lease_id"]
        lease_id = None if raw_lease_id is None else _json_string(
            raw_lease_id, command + ".lease_id")
        if lease_id is not None and (
                not _UUID.fullmatch(lease_id) or lease_id == "0" * 32 or
                lease_id != lease_id.lower()):
            raise ToolbusIpcProtocolError("操作结果lease_id无效")
        raw_node_uuid = data["expected_node_uuid"]
        node_uuid = None if raw_node_uuid is None else _json_string(
            raw_node_uuid, command + ".expected_node_uuid")
        if node_uuid is not None and (
                not _UUID.fullmatch(node_uuid) or node_uuid == "0" * 32 or
                node_uuid != node_uuid.lower()):
            raise ToolbusIpcProtocolError("操作结果expected_node_uuid无效")
        raw_resource = data["resource_id"]
        resource_id = None if raw_resource is None else _json_integer(
            raw_resource, command + ".resource_id", minimum=1,
            maximum=0xFFFFFFFF)
        raw_kind = data["kind"]
        if raw_kind is None:
            kind = None
        else:
            kind = _json_string(raw_kind, command + ".kind")
            if kind not in _OPERATION_KINDS:
                raise ToolbusIpcProtocolError("操作结果包含未知kind")
        if kind is None and not allow_unknown_kind:
            raise ToolbusIpcProtocolError("该操作响应不得省略kind")
        if expected_kind is not None and kind != expected_kind:
            raise ToolbusIpcProtocolError("操作结果kind与请求不匹配")
        state = _json_string(data["state"], command + ".state")
        recovery = _json_string(data["recovery"], command + ".recovery")
        if state not in _OPERATION_STATES or \
                recovery not in _OPERATION_RECOVERIES:
            raise ToolbusIpcProtocolError("操作结果包含未知状态或恢复状态")
        replayed = _json_boolean(data["replayed"], command + ".replayed")
        raw_object = data["object_id"]
        object_id = None if raw_object is None else _json_integer(
            raw_object, command + ".object_id", minimum=1,
            maximum=0xFFFFFFFF)
        raw_value = data["value"]
        value = None if raw_value is None else _json_boolean(
            raw_value, command + ".value")
        raw_error = data["error_code"]
        error_code = None if raw_error is None else _json_string(
            raw_error, command + ".error_code")
        if error_code is not None and error_code not in _OPERATION_ERRORS:
            raise ToolbusIpcProtocolError("操作结果包含未知error_code")
        frequency_hz = duty = None
        active_low = None
        if pwm_document:
            frequency_hz = None if data["frequency_hz"] is None else _json_integer(
                data["frequency_hz"], command + ".frequency_hz", minimum=1,
                maximum=0xFFFFFFFF)
            duty = None if data["duty"] is None else _json_integer(
                data["duty"], command + ".duty", maximum=10000)
            active_low = None if data["active_low"] is None else _json_boolean(
                data["active_low"], command + ".active_low")

        valid = False
        if kind is None:
            valid = state == "expired_unknown" and recovery == "none" and \
                lease_id is None and node_uuid is None and \
                resource_id is None and \
                object_id is None and value is None and \
                error_code == "history_expired"
        elif state == "pending":
            valid = recovery == "none" and object_id is None and \
                value is None and error_code is None
        elif state == "committed" and kind == "gpio_write":
            valid = recovery == "none" and object_id is not None and \
                value is not None and error_code is None
        elif state == "committed" and kind == "control_release":
            valid = recovery == "safe_closed" and object_id is None and \
                value is None and error_code is None
        elif state == "committed" and kind == "pwm_configure":
            valid = recovery == "none" and object_id is not None and value is None and \
                frequency_hz is not None and duty is not None and active_low is not None and \
                error_code is None
        elif state == "committed" and kind == "pwm_stop":
            valid = recovery == "safe_closed" and object_id is not None and value is None and \
                frequency_hz is None and duty is None and active_low is None and error_code is None
        elif state == "committed" and kind in {
                "timed_bitstream_configure", "timed_bitstream_frame"}:
            valid = recovery == "none" and object_id is not None and \
                value is None and error_code is None
        elif state == "committed" and kind == "timed_bitstream_stop":
            valid = recovery == "safe_closed" and object_id is not None and \
                value is None and error_code is None
        elif state == "committed" and kind == "bus_resource_reset":
            valid = recovery == "safe_closed" and object_id is None and \
                value is None and error_code is None
        elif state == "rejected":
            valid = recovery in {"not_sent", "safe_closed"} and \
                object_id is None and value is None and \
                error_code in {"rejected", "deadline", "backend",
                               "persistence"}
        elif state == "unknown":
            valid = recovery in {"safe_closed", "scope_blocked",
                                 "awaiting_reboot",
                                 "node_reboot_confirmed"} and \
                object_id is None and value is None and \
                error_code in {"deadline", "backend", "persistence"}
        elif state == "expired_unknown":
            valid = recovery == "none" and lease_id is None and \
                node_uuid is None and resource_id is None and \
                object_id is None and \
                value is None and error_code == "history_expired"
        if state != "expired_unknown" and (
                lease_id is None or node_uuid is None or resource_id is None):
            valid = False
        if not valid:
            raise ToolbusIpcProtocolError("操作结果字段组合无效")
        if expected_value is not None and state == "committed" and \
                value != expected_value:
            raise ToolbusIpcProtocolError("GPIO提交结果与请求目标电平不一致")
        result = {
            "operation_id": operation_id, "lease_id": lease_id,
            "expected_node_uuid": node_uuid, "resource_id": resource_id,
            "kind": kind, "state": state,
            "replayed": replayed, "recovery": recovery,
            "object_id": object_id, "value": value,
            "error_code": error_code,
        }
        if pwm_document:
            result.update(frequency_hz=frequency_hz, duty=duty,
                          active_low=active_low)
        return result

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
    def _json_daemon_identity(output: str) -> str:
        data = RemoteCliIpcClient._document(output, "daemon-identity")
        _exact_fields(data, {"ipc_version", "instance_id"},
                      "daemon-identity.data")
        version = _json_integer(
            data["ipc_version"], "daemon-identity.ipc_version",
            minimum=1, maximum=0xFFFF)
        if version != 1:
            raise ToolbusIpcProtocolError(
                f"daemon-identity IPC版本不受支持：{version}")
        instance_id = _json_string(
            data["instance_id"], "daemon-identity.instance_id")
        if not _UUID.fullmatch(instance_id) or instance_id == "0" * 32:
            raise ToolbusIpcProtocolError(
                "daemon-identity.instance_id必须是非零128位十六进制")
        return instance_id.lower()

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
            "resources", "node_issues", "clocks", "bus_health",
        }, "runtime-snapshot.data")
        version = _json_integer(
            data["snapshot_version"], "runtime-snapshot.snapshot_version",
            minimum=1, maximum=0xFFFF)
        if version != 3:
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

        raw_clocks = _json_array(
            data["clocks"], "runtime-snapshot.clocks")
        if len(raw_clocks) != len(nodes):
            raise ToolbusIpcProtocolError(
                "runtime-snapshot时钟质量项必须与节点一一对应")
        clocks = []
        clock_nodes: set[int] = set()
        nullable_unsigned = (
            "minimum_network_rtt_ns", "error_bound_ns", "sample_age_ns",
            "last_sample_host_time_ns")
        for index, raw in enumerate(raw_clocks):
            field_name = f"runtime-snapshot.clocks[{index}]"
            item = _json_object(raw, field_name)
            _exact_fields(item, {
                "node_id", "registered", "estimate_valid", "state",
                "boot_epoch", "model_generation", "sample_count",
                "selected_sample_count", "rate_deviation_ppb",
                "drift_uncertainty_ppm", "minimum_network_rtt_ns",
                "error_bound_ns", "sample_age_ns",
                "last_sample_host_time_ns",
            }, field_name)
            node_id = _json_integer(
                item["node_id"], field_name + ".node_id",
                minimum=1, maximum=127)
            if node_id not in node_ids or node_id in clock_nodes:
                raise ToolbusIpcProtocolError(
                    "runtime-snapshot时钟质量项重复或引用未知节点")
            clock_nodes.add(node_id)
            registered = _json_boolean(
                item["registered"], field_name + ".registered")
            estimate_valid = _json_boolean(
                item["estimate_valid"], field_name + ".estimate_valid")
            state = _json_string(item["state"], field_name + ".state")
            if state not in {"unregistered", "unsynced", "synced",
                             "degraded"}:
                raise ToolbusIpcProtocolError(
                    "runtime-snapshot时钟同步状态未知")
            boot_epoch = _json_nullable_integer(
                item["boot_epoch"], field_name + ".boot_epoch",
                minimum=1, maximum=0xFFFFFFFFFFFFFFFF)
            model_generation = _json_nullable_integer(
                item["model_generation"], field_name + ".model_generation",
                minimum=1, maximum=0xFFFFFFFFFFFFFFFF)
            sample_count = _json_integer(
                item["sample_count"], field_name + ".sample_count",
                maximum=0xFFFF)
            selected_sample_count = _json_integer(
                item["selected_sample_count"],
                field_name + ".selected_sample_count", maximum=0xFFFF)
            if selected_sample_count > sample_count:
                raise ToolbusIpcProtocolError(
                    "runtime-snapshot时钟入选样本数超过总样本数")
            rate_deviation_ppb = _json_nullable_integer(
                item["rate_deviation_ppb"],
                field_name + ".rate_deviation_ppb",
                minimum=-0x80000000, maximum=0x7FFFFFFF)
            drift_uncertainty_ppm = _json_nullable_integer(
                item["drift_uncertainty_ppm"],
                field_name + ".drift_uncertainty_ppm",
                maximum=0xFFFFFFFF)
            nullable_values = {
                name: _json_nullable_integer(
                    item[name], field_name + "." + name,
                    maximum=0xFFFFFFFFFFFFFFFF)
                for name in nullable_unsigned
            }
            quantitative_values = [
                rate_deviation_ppb, drift_uncertainty_ppm,
                *nullable_values.values()]
            if not registered:
                if state != "unregistered" or estimate_valid or \
                        boot_epoch is not None or \
                        model_generation is not None or sample_count != 0 or \
                        selected_sample_count != 0 or \
                        any(value is not None for value in quantitative_values):
                    raise ToolbusIpcProtocolError(
                        "runtime-snapshot未注册时钟字段不一致")
            else:
                if state == "unregistered" or boot_epoch is None or \
                        model_generation is None:
                    raise ToolbusIpcProtocolError(
                        "runtime-snapshot已注册时钟字段不完整")
                if not estimate_valid and (
                        state != "unsynced" or
                        any(value is not None
                            for value in quantitative_values)):
                    raise ToolbusIpcProtocolError(
                        "runtime-snapshot未知时钟估计字段不一致")
                if estimate_valid and any(
                        value is None for value in quantitative_values):
                    raise ToolbusIpcProtocolError(
                        "runtime-snapshot有效时钟估计字段不完整")
                if state in {"synced", "degraded"} and not estimate_valid:
                    raise ToolbusIpcProtocolError(
                        "runtime-snapshot时钟同步状态与估计有效性不一致")
            clocks.append({
                "node_id": node_id, "registered": registered,
                "estimate_valid": estimate_valid, "state": state,
                "boot_epoch": boot_epoch,
                "model_generation": model_generation,
                "sample_count": sample_count,
                "selected_sample_count": selected_sample_count,
                "rate_deviation_ppb": rate_deviation_ppb,
                "drift_uncertainty_ppm": drift_uncertainty_ppm,
                **nullable_values,
            })
        bus_health = []
        bus_identities: set[tuple[int, int]] = set()
        for index, raw in enumerate(_json_array(
                data["bus_health"], "runtime-snapshot.bus_health")):
            field_name = f"runtime-snapshot.bus_health[{index}]"
            item = _json_object(raw, field_name)
            _exact_fields(item, {
                "node_id", "resource_id", "last_status_valid",
                "last_status", "consecutive_failures",
                "peak_consecutive_failures", "last_result_time_us",
            }, field_name)
            node_id = _json_integer(item["node_id"], field_name + ".node_id",
                                    minimum=1, maximum=127)
            resource_id = _json_integer(
                item["resource_id"], field_name + ".resource_id",
                minimum=1, maximum=0xFFFFFFFF)
            identity = (node_id, resource_id)
            if node_id not in node_ids or identity in bus_identities:
                raise ToolbusIpcProtocolError(
                    "runtime-snapshot总线健康项重复或引用未知节点")
            bus_identities.add(identity)
            valid = _json_boolean(
                item["last_status_valid"], field_name + ".last_status_valid")
            status = _json_integer(item["last_status"],
                                   field_name + ".last_status", maximum=5)
            current = _json_integer(
                item["consecutive_failures"],
                field_name + ".consecutive_failures", maximum=0xFFFFFFFF)
            peak = _json_integer(
                item["peak_consecutive_failures"],
                field_name + ".peak_consecutive_failures", maximum=0xFFFFFFFF)
            last_time = _json_integer(
                item["last_result_time_us"],
                field_name + ".last_result_time_us",
                maximum=0xFFFFFFFFFFFFFFFF)
            if current > peak or (not valid and
                                  (status != 0 or current != 0 or peak != 0 or
                                   last_time != 0)):
                raise ToolbusIpcProtocolError(
                    "runtime-snapshot总线健康状态字段不一致")
            bus_health.append({
                "node_id": node_id, "resource_id": resource_id,
                "last_status_valid": valid, "last_status": status,
                "consecutive_failures": current,
                "peak_consecutive_failures": peak,
                "last_result_time_us": last_time,
            })
        return {
            "version": version, "sequence": sequence, "traffic": traffic,
            "nodes": nodes, "resources": resources, "node_issues": issues,
            "clocks": clocks, "bus_health": bus_health,
        }

    @staticmethod
    def _json_health_snapshot(output: str) -> dict:
        data = RemoteCliIpcClient._document(output, "health-snapshot")
        _exact_fields(data, {"ipc_version", "daemon_instance_id", "health"},
                      "health-snapshot.data")
        ipc_version = _json_integer(
            data["ipc_version"], "health-snapshot.ipc_version",
            minimum=1, maximum=0xFFFF)
        if ipc_version != 1:
            raise ToolbusIpcProtocolError(
                f"health-snapshot IPC版本不受支持：{ipc_version}")
        instance_id = _json_string(
            data["daemon_instance_id"],
            "health-snapshot.daemon_instance_id").lower()
        if _UUID.fullmatch(instance_id) is None or instance_id == "0" * 32:
            raise ToolbusIpcProtocolError("health-snapshot daemon身份无效")
        health = _json_object(data["health"], "health-snapshot.health")
        _exact_fields(health, {
            "contract_version", "source", "overall", "sample_sequence",
            "sample_time_ms", "node_id", "producer_generation", "metrics",
        }, "health-snapshot.health")
        version = _json_integer(
            health["contract_version"], "health-snapshot.contract_version",
            minimum=1, maximum=0xFFFF)
        source = _json_integer(
            health["source"], "health-snapshot.source",
            minimum=1, maximum=0xFF)
        overall = _json_integer(
            health["overall"], "health-snapshot.overall", maximum=0xFF)
        sequence = _json_integer(
            health["sample_sequence"], "health-snapshot.sample_sequence",
            minimum=1, maximum=0xFFFFFFFFFFFFFFFF)
        sample_time_ms = _json_integer(
            health["sample_time_ms"], "health-snapshot.sample_time_ms",
            maximum=0xFFFFFFFFFFFFFFFF)
        node_id = _json_integer(
            health["node_id"], "health-snapshot.node_id", maximum=0xFFFFFFFF)
        generation = _json_integer(
            health["producer_generation"],
            "health-snapshot.producer_generation",
            minimum=1, maximum=0xFFFFFFFFFFFFFFFF)
        raw_metrics = _json_array(
            health["metrics"], "health-snapshot.metrics")
        if len(raw_metrics) < 1 or len(raw_metrics) > 48:
            raise ToolbusIpcProtocolError(
                "health-snapshot指标数量必须位于1～48")
        encoded_metrics = bytearray()
        for index, raw in enumerate(raw_metrics):
            item = _json_object(raw, f"health-snapshot.metrics[{index}]")
            _exact_fields(item, {"metric_id", "availability", "unit", "value"},
                          f"health-snapshot.metrics[{index}]")
            metric_id = _json_integer(
                item["metric_id"], "health-snapshot.metric_id",
                minimum=1, maximum=0xFFFF)
            availability = _json_integer(
                item["availability"], "health-snapshot.availability",
                minimum=1, maximum=0xFF)
            unit = _json_integer(
                item["unit"], "health-snapshot.unit",
                minimum=1, maximum=0xFF)
            value = _json_integer(
                item["value"], "health-snapshot.value",
                maximum=0xFFFFFFFFFFFFFFFF)
            encoded_metrics.extend(struct.pack(
                "<HBBQ", metric_id, availability, unit, value))
        wire = struct.pack(
            "<HBBQQIQHH", version, source, overall, sequence,
            sample_time_ms, node_id, generation, len(raw_metrics), 0)
        wire += bytes(encoded_metrics)
        # 先用一次无状态严格解码封闭 JSON→wire 转换，再交给 Provider
        # 的有状态可信路由注册表。
        try:
            TrustedToolbusdHealthProjection(generation).ingest(wire)
        except HealthProjectionError as error:
            raise ToolbusIpcProtocolError(
                f"health-snapshot语义无效：{error}") from error
        return {
            "ipc_version": ipc_version,
            "daemon_instance_id": instance_id,
            "producer_generation": generation,
            "wire": wire,
        }

    @staticmethod
    def _json_node_health_snapshot(output: str, expected_node_id: int) -> dict:
        data = RemoteCliIpcClient._document(output, "node-health-snapshot")
        _exact_fields(data, {"health"}, "node-health-snapshot.data")
        health = _json_object(data["health"], "node-health-snapshot.health")
        _exact_fields(health, {
            "contract_version", "source", "overall", "sample_sequence",
            "sample_time_ms", "node_id", "producer_generation", "metrics",
        }, "node-health-snapshot.health")
        version = _json_integer(health["contract_version"],
                                "node-health-snapshot.contract_version",
                                minimum=1, maximum=0xFFFF)
        source = _json_integer(health["source"],
                               "node-health-snapshot.source",
                               minimum=1, maximum=0xFF)
        overall = _json_integer(health["overall"],
                                "node-health-snapshot.overall", maximum=0xFF)
        sequence = _json_integer(health["sample_sequence"],
                                 "node-health-snapshot.sample_sequence",
                                 minimum=1, maximum=0xFFFFFFFFFFFFFFFF)
        sample_time_ms = _json_integer(health["sample_time_ms"],
                                       "node-health-snapshot.sample_time_ms",
                                       maximum=0xFFFFFFFFFFFFFFFF)
        node_id = _json_integer(health["node_id"],
                                "node-health-snapshot.node_id",
                                minimum=1, maximum=127)
        generation = _json_integer(health["producer_generation"],
                                   "node-health-snapshot.producer_generation",
                                   minimum=1, maximum=0xFFFFFFFFFFFFFFFF)
        if node_id != expected_node_id:
            raise ToolbusIpcProtocolError(
                "node-health-snapshot返回了错误的节点ID")
        if source not in {1, 2}:
            raise ToolbusIpcProtocolError(
                "node-health-snapshot来源必须是MCU或Remote Core")
        raw_metrics = _json_array(health["metrics"],
                                  "node-health-snapshot.metrics")
        if not 1 <= len(raw_metrics) <= 48:
            raise ToolbusIpcProtocolError(
                "node-health-snapshot指标数量必须位于1～48")
        encoded_metrics = bytearray()
        for index, raw in enumerate(raw_metrics):
            item = _json_object(raw, f"node-health-snapshot.metrics[{index}]")
            _exact_fields(item, {"metric_id", "availability", "unit", "value"},
                          f"node-health-snapshot.metrics[{index}]")
            encoded_metrics.extend(struct.pack(
                "<HBBQ",
                _json_integer(item["metric_id"], "node-health-snapshot.metric_id",
                              minimum=1, maximum=0xFFFF),
                _json_integer(item["availability"],
                              "node-health-snapshot.availability",
                              minimum=1, maximum=0xFF),
                _json_integer(item["unit"], "node-health-snapshot.unit",
                              minimum=1, maximum=0xFF),
                _json_integer(item["value"], "node-health-snapshot.value",
                              maximum=0xFFFFFFFFFFFFFFFF)))
        wire = struct.pack("<HBBQQIQHH", version, source, overall, sequence,
                           sample_time_ms, node_id, generation,
                           len(raw_metrics), 0) + bytes(encoded_metrics)
        try:
            TrustedNodeHealthProjection(source, node_id, generation).ingest(wire)
        except HealthProjectionError as error:
            raise ToolbusIpcProtocolError(
                f"node-health-snapshot语义无效：{error}") from error
        return {"source": source, "node_id": node_id,
                "producer_generation": generation, "wire": wire}

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

    def daemon_identity(
            self, *, deadline: MonotonicDeadline | None = None) -> str:
        output = self._run("daemon-identity", deadline=deadline)
        if self.structured_output:
            return self._json_daemon_identity(output)
        lines = self._lines(output, "daemon-identity")
        if len(lines) != 1:
            raise ToolbusIpcProtocolError(
                "daemon-identity必须恰好返回一行")
        fields = lines[0]
        _required(fields, {"ipc_version", "instance_id"},
                  "daemon-identity")
        if set(fields) != {"ipc_version", "instance_id"}:
            raise ToolbusIpcProtocolError("daemon-identity包含未知字段")
        if _integer(fields["ipc_version"], "daemon-identity.ipc_version",
                    minimum=1, maximum=0xFFFF) != 1:
            raise ToolbusIpcProtocolError(
                "daemon-identity IPC版本不受支持")
        instance_id = fields["instance_id"].lower()
        if not _UUID.fullmatch(instance_id) or instance_id == "0" * 32:
            raise ToolbusIpcProtocolError(
                "daemon-identity.instance_id必须是非零128位十六进制")
        return instance_id

    @staticmethod
    def _control_id(value: str, name: str) -> str:
        if not isinstance(value, str) or not _UUID.fullmatch(value) or \
                value == "0" * 32:
            raise ToolbusIpcProtocolError(f"{name}必须是非零128位十六进制")
        return value.lower()

    def runtime_control_acquire(
            self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int,
            ttl_ms: int, *,
            deadline: MonotonicDeadline | None = None) -> None:
        output = self._run(
            "runtime-control-acquire", node_id=node_id,
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       self._control_id(expected_node_uuid, "预期节点UUID"),
                       owner_key_id, str(resource_id), str(ttl_ms)),
            deadline=deadline)
        if not self.structured_output:
            if output.strip() != "ok":
                raise ToolbusIpcProtocolError("控制租约登记返回值无效")
            return
        data = self._document(output, "runtime-control-acquire")
        _exact_fields(data, set(), "runtime-control-acquire.data")

    def runtime_gpio_write(
            self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int,
            idempotency_key: str, value: bool, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if type(value) is not bool:
            raise ToolbusIpcProtocolError("GPIO目标电平必须是布尔值")
        output = self._run(
            "runtime-gpio-write", node_id=node_id,
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       self._control_id(expected_node_uuid, "预期节点UUID"),
                       owner_key_id, str(resource_id), idempotency_key,
                       "1" if value else "0"), deadline=deadline)
        if not self.structured_output:
            raise ToolbusIpcProtocolError("GPIO写控制要求结构化remote-cli输出")
        data = self._document(output, "runtime-gpio-write")
        _exact_fields(data, {"object_id", "value", "replayed"},
                      "runtime-gpio-write.data")
        returned_value = _json_boolean(
            data["value"], "runtime-gpio-write.value")
        if returned_value != value:
            raise ToolbusIpcProtocolError("GPIO写结果电平与请求不一致")
        return {
            "object_id": _json_integer(
                data["object_id"], "runtime-gpio-write.object_id",
                minimum=1, maximum=0xFFFFFFFF),
            "value": returned_value,
            "replayed": _json_boolean(
                data["replayed"], "runtime-gpio-write.replayed"),
        }

    def runtime_control_release(
            self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, *,
            deadline: MonotonicDeadline | None = None) -> None:
        output = self._run(
            "runtime-control-release",
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       owner_key_id), deadline=deadline)
        if not self.structured_output:
            if output.strip() != "ok":
                raise ToolbusIpcProtocolError("控制租约释放返回值无效")
            return
        data = self._document(output, "runtime-control-release")
        _exact_fields(data, set(), "runtime-control-release.data")

    @staticmethod
    def _operation_id(value: str) -> str:
        if not isinstance(value, str) or \
                _OPERATION_ID.fullmatch(value) is None or value == "0" * 64:
            raise ToolbusIpcProtocolError(
                "operation_id必须是非零64位小写十六进制")
        return value

    def runtime_gpio_write_operation(
            self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int, idempotency_key: str, value: bool, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if type(value) is not bool:
            raise ToolbusIpcProtocolError("GPIO目标电平必须是布尔值")
        output = self._run(
            "runtime-gpio-write-operation", node_id=node_id,
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       self._control_id(expected_node_uuid, "预期节点UUID"),
                       owner_key_id, str(resource_id), idempotency_key,
                       "1" if value else "0"), deadline=deadline)
        if not self.structured_output:
            raise ToolbusIpcProtocolError("操作账本要求结构化remote-cli输出")
        return self._json_operation_outcome(
            output, "runtime-gpio-write-operation",
            expected_kind="gpio_write", expected_value=value)

    def runtime_pwm_acquire(self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int, ttl_ms: int, *,
            deadline: MonotonicDeadline | None = None) -> None:
        output = self._run("runtime-pwm-acquire", node_id=node_id,
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       self._control_id(expected_node_uuid, "预期节点UUID"),
                       owner_key_id, str(resource_id), str(ttl_ms)), deadline=deadline)
        data = self._document(output, "runtime-pwm-acquire")
        _exact_fields(data, set(), "runtime-pwm-acquire.data")

    def runtime_pwm_configure_operation(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str,
            node_id: int, resource_id: int, idempotency_key: str,
            frequency_hz: int, duty: int, active_low: bool, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if type(frequency_hz) is not int or frequency_hz <= 0 or frequency_hz > 0xFFFFFFFF or \
                type(duty) is not int or duty < 0 or duty > 10000 or type(active_low) is not bool:
            raise ToolbusIpcProtocolError("PWM配置参数无效")
        output = self._run("runtime-pwm-configure-operation", node_id=node_id,
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       self._control_id(expected_node_uuid, "预期节点UUID"), owner_key_id,
                       str(resource_id), idempotency_key, str(frequency_hz), str(duty),
                       "1" if active_low else "0"), deadline=deadline)
        outcome = self._json_operation_outcome(output, "runtime-pwm-configure-operation",
                                               expected_kind="pwm_configure")
        if outcome["state"] == "committed" and (outcome["frequency_hz"], outcome["duty"],
                outcome["active_low"]) != (frequency_hz, duty, active_low):
            raise ToolbusIpcProtocolError("PWM提交结果与请求配置不一致")
        return outcome

    def runtime_pwm_stop_operation(self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int, idempotency_key: str, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        output = self._run("runtime-pwm-stop-operation", node_id=node_id,
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       self._control_id(expected_node_uuid, "预期节点UUID"), owner_key_id,
                       str(resource_id), idempotency_key), deadline=deadline)
        return self._json_operation_outcome(output, "runtime-pwm-stop-operation",
                                            expected_kind="pwm_stop")

    def runtime_bus_reset_acquire(self, daemon_instance_id: str, lease_id: str,
            expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int, ttl_ms: int, *,
            deadline: MonotonicDeadline | None = None) -> None:
        output = self._run("runtime-bus-reset-acquire", node_id=node_id,
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       self._control_id(expected_node_uuid, "预期节点UUID"),
                       owner_key_id, str(resource_id), str(ttl_ms)), deadline=deadline)
        data = self._document(output, "runtime-bus-reset-acquire")
        _exact_fields(data, set(), "runtime-bus-reset-acquire.data")

    def runtime_bus_resource_reset_operation(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str,
            node_id: int, resource_id: int, idempotency_key: str, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        output = self._run("runtime-bus-resource-reset-operation", node_id=node_id,
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       self._control_id(expected_node_uuid, "预期节点UUID"),
                       owner_key_id, str(resource_id), idempotency_key), deadline=deadline)
        return self._json_operation_outcome(
            output, "runtime-bus-resource-reset-operation",
            expected_kind="bus_resource_reset")

    def runtime_timed_bitstream_acquire(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str,
            node_id: int, resource_id: int, ttl_ms: int, *,
            deadline: MonotonicDeadline | None = None) -> None:
        output = self._run("runtime-timed-bitstream-acquire", node_id=node_id,
            arguments=(self._control_id(daemon_instance_id,"daemon实例ID"),
                       self._control_id(lease_id,"控制租约ID"),
                       self._control_id(expected_node_uuid,"预期节点UUID"), owner_key_id,
                       str(resource_id),str(ttl_ms)),deadline=deadline)
        data=self._document(output,"runtime-timed-bitstream-acquire")
        _exact_fields(data,set(),"runtime-timed-bitstream-acquire.data")

    def runtime_timed_bitstream_configure_operation(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int, idempotency_key: str, bit_period_ns: int,
            zero_high_ns: int, one_high_ns: int, reset_time_us: int, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        values=(bit_period_ns,zero_high_ns,one_high_ns,reset_time_us)
        if any(type(v) is not int or not 1 <= v <= 0xFFFFFFFF for v in values):
            raise ToolbusIpcProtocolError("定时位流配置参数无效")
        output=self._run("runtime-timed-bitstream-configure-operation",node_id=node_id,
            arguments=(self._control_id(daemon_instance_id,"daemon实例ID"),self._control_id(lease_id,"控制租约ID"),
            self._control_id(expected_node_uuid,"预期节点UUID"),owner_key_id,str(resource_id),idempotency_key,
            *(str(v) for v in values)),deadline=deadline)
        return self._json_operation_outcome(output,"runtime-timed-bitstream-configure-operation",
                                            expected_kind="timed_bitstream_configure")

    def runtime_timed_bitstream_frame_operation(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int, idempotency_key: str, bit_count: int, data: bytes, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if type(bit_count) is not int or not 1 <= bit_count <= 16176 or \
                not isinstance(data,bytes) or len(data)!=(bit_count+7)//8:
            raise ToolbusIpcProtocolError("定时位流帧参数无效")
        output=self._run("runtime-timed-bitstream-frame-operation",node_id=node_id,
            arguments=(self._control_id(daemon_instance_id,"daemon实例ID"),self._control_id(lease_id,"控制租约ID"),
            self._control_id(expected_node_uuid,"预期节点UUID"),owner_key_id,str(resource_id),idempotency_key,
            str(bit_count),data.hex()),deadline=deadline)
        return self._json_operation_outcome(output,"runtime-timed-bitstream-frame-operation",
                                            expected_kind="timed_bitstream_frame")

    def runtime_timed_bitstream_stop_operation(self, daemon_instance_id: str,
            lease_id: str, expected_node_uuid: str, owner_key_id: str, node_id: int,
            resource_id: int, idempotency_key: str, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        output=self._run("runtime-timed-bitstream-stop-operation",node_id=node_id,
            arguments=(self._control_id(daemon_instance_id,"daemon实例ID"),self._control_id(lease_id,"控制租约ID"),
            self._control_id(expected_node_uuid,"预期节点UUID"),owner_key_id,str(resource_id),idempotency_key),deadline=deadline)
        return self._json_operation_outcome(output,"runtime-timed-bitstream-stop-operation",
                                            expected_kind="timed_bitstream_stop")

    def runtime_control_release_operation(
            self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        output = self._run(
            "runtime-control-release-operation",
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       self._control_id(lease_id, "控制租约ID"),
                       owner_key_id), deadline=deadline)
        if not self.structured_output:
            raise ToolbusIpcProtocolError("操作账本要求结构化remote-cli输出")
        return self._json_operation_outcome(
            output, "runtime-control-release-operation",
            expected_kind="control_release")

    def runtime_operation_status(
            self, daemon_instance_id: str, owner_key_id: str,
            operation_id: str, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        operation_id = self._operation_id(operation_id)
        output = self._run(
            "runtime-operation-status",
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       owner_key_id, operation_id), deadline=deadline)
        if not self.structured_output:
            raise ToolbusIpcProtocolError("操作账本查询要求结构化remote-cli输出")
        outcome = self._json_operation_outcome(
            output, "runtime-operation-status",
            expected_operation_id=operation_id, allow_unknown_kind=True)
        if not outcome["replayed"]:
            raise ToolbusIpcProtocolError("操作状态查询必须标记replayed")
        return outcome

    def runtime_operation_lookup(
            self, daemon_instance_id: str, owner_key_id: str,
            kind: str, lease_id: str, idempotency_key: str, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if kind not in _OPERATION_KINDS:
            raise ToolbusIpcProtocolError("操作定位kind无效")
        if kind == "control_release" and idempotency_key != "release:v1":
            raise ToolbusIpcProtocolError(
                "control_release定位必须使用release:v1")
        output = self._run(
            "runtime-operation-lookup",
            arguments=(self._control_id(daemon_instance_id, "daemon实例ID"),
                       owner_key_id, kind,
                       self._control_id(lease_id, "控制租约ID"),
                       idempotency_key), deadline=deadline)
        if not self.structured_output:
            raise ToolbusIpcProtocolError("操作账本定位要求结构化remote-cli输出")
        outcome = self._json_operation_outcome(
            output, "runtime-operation-lookup", expected_kind=kind)
        if not outcome["replayed"]:
            raise ToolbusIpcProtocolError("操作定位查询必须标记replayed")
        return outcome

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

    def runtime_snapshot(
            self, maximum_resources: int, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if maximum_resources < 1 or maximum_resources > 128:
            raise ToolbusIpcProtocolError(
                "Runtime快照资源上限必须位于1～128")
        process_timeout_ms = int(
            self.timeout_seconds * 1000) if deadline is None else \
            deadline.remaining_milliseconds(
                cap=int(self.timeout_seconds * 1000))
        headroom_ms = min(100, max(1, process_timeout_ms // 10))
        timeout_ms = min(
            5000, max(1, process_timeout_ms - headroom_ms))
        output = self._run(
            "runtime-snapshot",
            arguments=(str(maximum_resources), str(timeout_ms)),
            deadline=deadline)
        if not self.structured_output:
            raise ToolbusIpcProtocolError(
                "Runtime单次快照要求结构化remote-cli输出")
        return self._json_runtime_snapshot(output)

    def health_snapshot(
            self, *, deadline: MonotonicDeadline | None = None) -> dict:
        if not self.structured_output:
            raise ToolbusIpcProtocolError(
                "健康快照要求结构化remote-cli输出")
        return self._json_health_snapshot(
            self._run("health-snapshot", deadline=deadline))

    def logical_recording_status(
            self, *, deadline: MonotonicDeadline | None = None) -> dict:
        if not self.structured_output:
            raise ToolbusIpcProtocolError("逻辑录制状态要求结构化remote-cli输出")
        command = "logical-recording-status"
        data = self._document(self._run(command, deadline=deadline), command)
        _exact_fields(data, {"configured", "active", "evidence_scope",
                             "output_name", "event_count", "maximum_events",
                             "maximum_file_bytes"}, command + ".data")
        configured = _json_boolean(data["configured"], command + ".configured")
        active = _json_boolean(data["active"], command + ".active")
        evidence_scope = _json_string(
            data["evidence_scope"], command + ".evidence_scope")
        if evidence_scope != "logical_link_frames":
            raise ToolbusIpcProtocolError("逻辑录制证据范围不受支持")
        output_name = _json_string(data["output_name"], command + ".output_name")
        if len(output_name.encode("utf-8")) > 255 or any(
                ord(char) < 0x20 for char in output_name):
            raise ToolbusIpcProtocolError("逻辑录制输出名无效")
        return {
            "configured": configured, "active": active,
            "evidence_scope": evidence_scope,
            "event_count": _json_integer(
                data["event_count"], command + ".event_count"),
            "maximum_events": _json_integer(
                data["maximum_events"], command + ".maximum_events"),
            "maximum_file_bytes": _json_integer(
                data["maximum_file_bytes"], command + ".maximum_file_bytes"),
        }

    def node_health_snapshot(
            self, node_id: int, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if not self.structured_output:
            raise ToolbusIpcProtocolError("节点健康快照要求结构化remote-cli输出")
        return self._json_node_health_snapshot(
            self._run("node-health-snapshot", node_id=node_id,
                      deadline=deadline), node_id)


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
                 refresh_wait_timeout_ms: int = 5000,
                 maximum_clock_error_bound_ns: int = 250_000,
                 maximum_clock_sample_age_ms: int = 1_000,
                 maximum_node_health_sample_age_ms: int = 5_000):
        if maximum_resources_per_snapshot < 1:
            raise ValueError("每次快照资源查询上限必须大于0")
        if cache_ttl_ms < 0 or cache_ttl_ms > 60_000:
            raise ValueError("快照缓存时间必须位于0～60000毫秒")
        if maximum_concurrent_status_queries < 1 or \
                maximum_concurrent_status_queries > 32:
            raise ValueError("资源状态查询并发必须位于1～32")
        if refresh_wait_timeout_ms < 1 or refresh_wait_timeout_ms > 60_000:
            raise ValueError("快照刷新等待时间必须位于1～60000毫秒")
        if maximum_clock_error_bound_ns < 1 or \
                maximum_clock_error_bound_ns > 1_000_000_000:
            raise ValueError("时钟估计误差告警阈值必须位于1～1000000000纳秒")
        if maximum_clock_sample_age_ms < 1 or \
                maximum_clock_sample_age_ms > 60_000:
            raise ValueError("时钟样本年龄告警阈值必须位于1～60000毫秒")
        if maximum_node_health_sample_age_ms < 1 or \
                maximum_node_health_sample_age_ms > 60_000:
            raise ValueError("节点健康样本年龄上限必须位于1～60000毫秒")
        self.client = client
        self.clock_ms = clock_ms or (lambda: time.monotonic_ns() // 1_000_000)
        self.maximum_resources_per_snapshot = maximum_resources_per_snapshot
        self.cache_ttl_ms = cache_ttl_ms
        self.maximum_concurrent_status_queries = \
            maximum_concurrent_status_queries
        self.refresh_wait_timeout_ms = refresh_wait_timeout_ms
        self.maximum_clock_error_bound_ns = maximum_clock_error_bound_ns
        self.maximum_clock_sample_age_ms = maximum_clock_sample_age_ms
        self.maximum_node_health_sample_age_ms = \
            maximum_node_health_sample_age_ms
        # 旧版逐资源查询无法把 deadline 传入回调，因此执行线程只能在回调
        # 自己返回后回收。整个 Provider 共用一个有界执行器和一个活动代次，
        # 防止连续超时为仍在运行的旧调用不断创建新线程池。
        self._status_executor = ThreadPoolExecutor(
            max_workers=maximum_concurrent_status_queries,
            thread_name_prefix="runtime-status")
        self._status_condition = threading.Condition()
        self._status_inflight: list | None = None
        self._cache_condition = threading.Condition()
        self._cached_snapshot: dict | None = None
        self._cache_stored_at_ms: int | None = None
        self._cached_error: str | None = None
        self._error_stored_at_ms: int | None = None
        self._refreshing = False
        self._health_lock = threading.Lock()
        self._health_daemon_instance_id: str | None = None
        self._health_generation: int | None = None
        self._health_projection: TrustedToolbusdHealthProjection | None = None
        self._node_health_lock = threading.Lock()
        self._node_health_routes: dict[str, dict] = {}
        self._operation_lock = threading.Lock()
        self._operation_inflight: dict[tuple[str, ...], Future] = {}
        self._operation_inflight_capacity = 128
        self._operation_executor = ThreadPoolExecutor(
            max_workers=8, thread_name_prefix="runtime-operation-query")

    def runtime_capabilities(self) -> dict:
        structured_output = bool(
            getattr(self.client, "structured_output", True))
        snapshot_available = structured_output and callable(
            getattr(self.client, "runtime_snapshot", None))
        return {
            "clock_sync_quality": {
                "available": snapshot_available,
                "source": (
                    "runtime_snapshot_v2" if snapshot_available
                    else ("runtime_snapshot_unavailable"
                          if structured_output else "legacy_text")),
                "estimate_kind": (
                    "host_model_estimate" if snapshot_available
                    else "unavailable"),
                "maximum_error_bound_ns":
                    self.maximum_clock_error_bound_ns,
                "maximum_sample_age_ms":
                    self.maximum_clock_sample_age_ms,
            },
            "toolbusd_health_snapshot": {
                "available": structured_output and callable(
                    getattr(self.client, "health_snapshot", None)),
                "contract_version": 1,
                "source": "local_toolbusd_ipc",
            },
            "node_health_snapshot": {
                "available": structured_output and callable(
                    getattr(self.client, "node_health_snapshot", None)),
                "contract_version": 1,
                "sources": ["mcu", "remote_core"],
                "maximum_unchanged_sample_age_ms":
                    self.maximum_node_health_sample_age_ms,
            },
        }

    def operational_status(
            self, *, deadline: MonotonicDeadline | None = None) -> dict:
        identity_reader = getattr(self.client, "daemon_identity", None)
        if not callable(identity_reader):
            return {"availability": "unknown", "connection": "unknown"}
        try:
            # 只用成功与否证明本次连接；实例身份本身不进入 Web 响应。
            call_with_deadline(identity_reader, deadline=deadline)
        except (ToolbusIpcError, RequestDeadlineExceeded):
            return {"availability": "unavailable",
                    "connection": "unavailable"}
        reader = getattr(self.client, "logical_recording_status", None)
        recording = {"availability": "unknown", "configured": None,
                     "active": None, "event_count": None,
                     "maximum_events": None, "maximum_file_bytes": None,
                     "evidence_scope": None}
        if callable(reader) and bool(getattr(self.client, "structured_output", True)):
            try:
                recording = {"availability": "available", **call_with_deadline(
                    reader, deadline=deadline)}
            except (ToolbusIpcError, RequestDeadlineExceeded):
                recording["availability"] = "unavailable"
        return {"availability": "available", "connection": "connected",
                "logical_recording": recording}

    def _node_health(self, uuid: str, numeric_id: int, captured_at_ms: int,
                     *, deadline: MonotonicDeadline | None) -> dict:
        reader = getattr(self.client, "node_health_snapshot", None)
        if not bool(getattr(self.client, "structured_output", True)) or \
                not callable(reader):
            return {"availability": "unknown", "reason":
                    "node_health_ipc_unavailable", "snapshot": None,
                    "sample_age_ms": None}
        try:
            source = call_with_deadline(reader, numeric_id, deadline=deadline)
        except (ToolbusIpcError, NotImplementedError, StopIteration) as error:
            # 测试/嵌入式适配器可用 NotImplementedError（旧的有限命令
            # runner 可能以 StopIteration）明确表示尚未提供该可选只读命令。
            return {"availability": "unavailable", "reason":
                    "node_health_read_failed", "snapshot": None,
                    "sample_age_ms": None, "detail": str(error)}
        with self._node_health_lock:
            route = self._node_health_routes.get(uuid)
            generation = int(source["producer_generation"])
            source_kind = int(source["source"])
            if route is None:
                route = {
                    "node_id": numeric_id, "source": source_kind,
                    "generation": generation, "retired": set(),
                    "projection": TrustedNodeHealthProjection(
                        source_kind, numeric_id, generation),
                    "sequence": 0, "observed_at_ms": captured_at_ms,
                }
                self._node_health_routes[uuid] = route
            elif route["node_id"] != numeric_id or route["source"] != source_kind:
                return {"availability": "unavailable", "reason":
                        "node_health_route_changed", "snapshot": None,
                        "sample_age_ms": None}
            elif generation != route["generation"]:
                if generation in route["retired"]:
                    return {"availability": "unavailable", "reason":
                            "node_health_retired_generation", "snapshot": None,
                            "sample_age_ms": None}
                route["retired"].add(route["generation"])
                route["generation"] = generation
                route["projection"] = TrustedNodeHealthProjection(
                    source_kind, numeric_id, generation)
                route["sequence"] = 0
                route["observed_at_ms"] = captured_at_ms
            try:
                projected = route["projection"].ingest(source["wire"])
            except HealthProjectionError as error:
                return {"availability": "unavailable", "reason":
                        "node_health_sample_rejected", "snapshot": None,
                        "sample_age_ms": None, "detail": str(error)}
            sequence = int(projected["sample_sequence"])
            if sequence != route["sequence"]:
                route["sequence"] = sequence
                route["observed_at_ms"] = captured_at_ms
            age_ms = max(0, captured_at_ms - int(route["observed_at_ms"]))
            if age_ms > self.maximum_node_health_sample_age_ms:
                return {"availability": "unavailable", "reason":
                        "node_health_sample_stale", "snapshot": None,
                        "sample_age_ms": age_ms}
            return {"availability": "available", "reason": None,
                    "snapshot": projected, "sample_age_ms": age_ms}

    def health_snapshot(
            self, *, deadline: MonotonicDeadline | None = None) -> dict:
        reader = getattr(self.client, "health_snapshot", None)
        if not bool(getattr(self.client, "structured_output", True)) or \
                not callable(reader):
            raise RuntimeProviderError("toolbusd 健康快照 IPC 不可用")
        # 串行覆盖调用和状态切换，防止旧 daemon 的迟到结果反向覆盖新世代。
        if deadline is None:
            acquired = self._health_lock.acquire()
        else:
            acquired = self._health_lock.acquire(
                timeout=deadline.remaining_seconds())
        if not acquired:
            raise RequestDeadlineExceeded("健康快照请求期限已耗尽")
        try:
            try:
                source = call_with_deadline(reader, deadline=deadline)
            except ToolbusIpcProtocolError as error:
                raise RuntimeProviderError(
                    f"toolbusd 健康快照协议不兼容：{error}") from error
            except ToolbusIpcError as error:
                raise RuntimeProviderError(
                    f"toolbusd 健康快照不可用：{error}") from error
            instance_id = str(source["daemon_instance_id"])
            generation = int(source["producer_generation"])
            if self._health_daemon_instance_id == instance_id:
                if self._health_generation != generation or \
                        self._health_projection is None:
                    raise RuntimeProviderError(
                        "同一toolbusd实例的健康生产者代际发生变化")
            else:
                self._health_daemon_instance_id = instance_id
                self._health_generation = generation
                self._health_projection = \
                    TrustedToolbusdHealthProjection(generation)
            try:
                projected = self._health_projection.ingest(source["wire"])
            except HealthProjectionError as error:
                raise RuntimeProviderError(
                    f"toolbusd 健康快照被可信边界拒绝：{error}") from error
            return {"daemon_instance_id": instance_id, **projected}
        finally:
            self._health_lock.release()

    @property
    def gpio_control_available(self) -> bool:
        """只有新版结构化客户端完整提供 operation ledger 时才声明可写。"""
        return bool(getattr(self.client, "structured_output", True)) and all(
            callable(getattr(self.client, name, None)) for name in (
                "runtime_control_acquire", "runtime_gpio_write_operation",
                "runtime_control_release_operation",
                "runtime_operation_status", "runtime_operation_lookup"))

    @property
    def pwm_control_available(self) -> bool:
        return bool(getattr(self.client, "structured_output", True)) and all(
            callable(getattr(self.client, name, None)) for name in (
                "runtime_pwm_acquire", "runtime_pwm_configure_operation",
                "runtime_pwm_stop_operation", "runtime_operation_status",
                "runtime_operation_lookup"))

    @property
    def timed_bitstream_control_available(self) -> bool:
        return bool(getattr(self.client,"structured_output",True)) and all(
            callable(getattr(self.client,name,None)) for name in (
                "runtime_timed_bitstream_acquire",
                "runtime_timed_bitstream_configure_operation",
                "runtime_timed_bitstream_frame_operation",
                "runtime_timed_bitstream_stop_operation",
                "runtime_operation_status","runtime_operation_lookup"))

    @property
    def bus_reset_control_available(self) -> bool:
        return bool(getattr(self.client, "structured_output", True)) and all(
            callable(getattr(self.client, name, None)) for name in (
                "runtime_bus_reset_acquire",
                "runtime_bus_resource_reset_operation",
                "runtime_operation_status", "runtime_operation_lookup"))

    def _operation_singleflight(
            self, key: tuple[str, ...], operation: Callable[[], dict], *,
            deadline: MonotonicDeadline | None) -> dict:
        """同一查询只启动一个CLI；短等待者超时不会取消共享查询。"""
        created = False
        with self._operation_lock:
            future = self._operation_inflight.get(key)
            if future is None:
                if len(self._operation_inflight) >= \
                        self._operation_inflight_capacity:
                    raise RuntimeProviderOperationError(
                        "backend_unavailable", category="transport",
                        retryable=True, possibly_committed=False)
                future = self._operation_executor.submit(operation)
                self._operation_inflight[key] = future
                created = True

        if created:
            def remove(completed: Future, *,
                       operation_key: tuple[str, ...] = key,
                       submitted: Future = future) -> None:
                del completed
                with self._operation_lock:
                    current = self._operation_inflight.get(operation_key)
                    if current is submitted:
                        self._operation_inflight.pop(operation_key, None)

            # add_done_callback 对已完成 Future 会同步调用；必须在互斥锁外注册，
            # 否则极快的本地 CLI/Fake 会重入同一非递归锁而死锁。
            future.add_done_callback(remove)
        if future is None:
            raise RuntimeProviderError("操作查询单飞状态无效")
        timeout = None if deadline is None else deadline.remaining_seconds()
        try:
            return copy.deepcopy(future.result(timeout=timeout))
        except FutureTimeout as error:
            raise RequestDeadlineExceeded(
                "操作查询未在统一期限内完成") from error

    @staticmethod
    def _operation_query_error(error: ToolbusIpcError
                               ) -> RuntimeProviderOperationError:
        mapped = _provider_operation_error(error)
        # status/lookup 均为只读；即使响应损坏或传输断开，也不存在提交未知。
        if mapped.possibly_committed:
            return RuntimeProviderOperationError(
                mapped.code, category=mapped.category,
                retryable=False, possibly_committed=False,
                detail=mapped.detail,
                invalidates_global_operational=
                mapped.invalidates_global_operational)
        return mapped

    def _resolve_gpio_target(
            self, node_id: str, resource_id: str, *,
            deadline: MonotonicDeadline | None = None
    ) -> tuple[str, int, int]:
        read = self.read_snapshot(deadline=deadline)
        node = next((item for item in read.snapshot["nodes"]
                     if item["node_id"] == node_id), None)
        if node is None:
            raise RuntimeProviderOperationError(
                "target_rejected", category="target", retryable=False,
                possibly_committed=False)
        if node["state"] != "online":
            raise RuntimeProviderOperationError(
                "target_rejected", category="target", retryable=True,
                possibly_committed=False)
        resource = next((item for item in node["resources"]
                         if item["resource_id"] == resource_id), None)
        if resource is None:
            raise RuntimeProviderOperationError(
                "target_rejected", category="target", retryable=False,
                possibly_committed=False)
        if resource["kind"] != "gpio" or not resource["available"]:
            raise RuntimeProviderOperationError(
                "target_rejected", category="target", retryable=False,
                possibly_committed=False)
        numeric_node = node["runtime"].get("bus_node_id")
        if type(numeric_node) is not int or not 1 <= numeric_node <= 127:
            raise RuntimeProviderOperationError(
                "protocol_incompatible", category="protocol",
                retryable=False, possibly_committed=False)
        match = re.fullmatch(r"resource-([0-9a-f]{8})", resource_id)
        if match is None:
            raise RuntimeProviderOperationError(
                "protocol_incompatible", category="protocol",
                retryable=False, possibly_committed=False)
        node_match = re.fullmatch(r"node-([0-9a-f]{32})", node_id)
        if node_match is None:
            raise RuntimeProviderOperationError(
                "protocol_incompatible", category="protocol",
                retryable=False, possibly_committed=False)
        return node_match.group(1), numeric_node, int(match.group(1), 16)

    def _resolve_pwm_target(self, node_id: str, resource_id: str, *,
                            deadline: MonotonicDeadline | None = None
                            ) -> tuple[str, int, int]:
        # PWM 控制只需要已经校验过的 UUID、总线节点号和资源类型，不应为
        # 每次写操作重新读取所有资源状态。完整快照可能包含数十次状态查询，
        # 会无谓消耗控制期限，甚至让没有主动心跳的节点跨过离线窗口。
        # 缓存仅用作结构目录；在线性、UUID、代次及合同仍由 toolbusd Gate
        # 在登记租约和执行操作时重新权威校验。
        with self._cache_condition:
            snapshot = copy.deepcopy(self._cached_snapshot)
        if snapshot is None:
            snapshot = self.read_snapshot(deadline=deadline).snapshot
        node = next((item for item in snapshot["nodes"] if item["node_id"] == node_id), None)
        resource = None if node is None else next((item for item in node["resources"]
                                                   if item["resource_id"] == resource_id), None)
        if node is None or node["state"] != "online" or resource is None or \
                resource["kind"] != "pwm" or not resource["available"]:
            raise RuntimeProviderOperationError("target_rejected", category="target",
                retryable=False, possibly_committed=False)
        numeric_node = node["runtime"].get("bus_node_id")
        resource_match = re.fullmatch(r"resource-([0-9a-f]{8})", resource_id)
        node_match = re.fullmatch(r"node-([0-9a-f]{32})", node_id)
        if type(numeric_node) is not int or not 1 <= numeric_node <= 127 or \
                resource_match is None or node_match is None:
            raise RuntimeProviderOperationError("protocol_incompatible", category="protocol",
                retryable=False, possibly_committed=False)
        return node_match.group(1), numeric_node, int(resource_match.group(1), 16)

    def _resolve_timed_bitstream_target(self, node_id: str, resource_id: str, *,
            deadline: MonotonicDeadline | None = None) -> tuple[str,int,int]:
        with self._cache_condition:
            snapshot=copy.deepcopy(self._cached_snapshot)
        if snapshot is None: snapshot=self.read_snapshot(deadline=deadline).snapshot
        node=next((item for item in snapshot["nodes"] if item["node_id"]==node_id),None)
        resource=None if node is None else next((item for item in node["resources"] if item["resource_id"]==resource_id),None)
        if node is None or node["state"]!="online" or resource is None or \
                resource["kind"]!="timed-bitstream" or not resource["available"]:
            raise RuntimeProviderOperationError("target_rejected",category="target",retryable=False,possibly_committed=False)
        numeric=node["runtime"].get("bus_node_id"); rm=re.fullmatch(r"resource-([0-9a-f]{8})",resource_id); nm=re.fullmatch(r"node-([0-9a-f]{32})",node_id)
        if type(numeric) is not int or not 1<=numeric<=127 or rm is None or nm is None:
            raise RuntimeProviderOperationError("protocol_incompatible",category="protocol",retryable=False,possibly_committed=False)
        return nm.group(1),numeric,int(rm.group(1),16)

    def _resolve_bus_reset_target(self, node_id: str, resource_id: str, *,
            deadline: MonotonicDeadline | None = None) -> tuple[str, int, int]:
        with self._cache_condition:
            snapshot = copy.deepcopy(self._cached_snapshot)
        if snapshot is None:
            snapshot = self.read_snapshot(deadline=deadline).snapshot
        node = next((item for item in snapshot["nodes"]
                     if item["node_id"] == node_id), None)
        resource = None if node is None else next((item for item in node["resources"]
            if item["resource_id"] == resource_id), None)
        if node is None or node["state"] != "online" or resource is None or \
                resource["kind"] not in {"i2c_device", "spi_device"} or \
                not resource["available"]:
            raise RuntimeProviderOperationError("target_rejected", category="target",
                retryable=False, possibly_committed=False)
        numeric = node["runtime"].get("bus_node_id")
        rm = re.fullmatch(r"resource-([0-9a-f]{8})", resource_id)
        nm = re.fullmatch(r"node-([0-9a-f]{32})", node_id)
        if type(numeric) is not int or not 1 <= numeric <= 127 or rm is None or nm is None:
            raise RuntimeProviderOperationError("protocol_incompatible", category="protocol",
                retryable=False, possibly_committed=False)
        return nm.group(1), numeric, int(rm.group(1), 16)

    def _invalidate_snapshot_cache(self) -> None:
        with self._cache_condition:
            self._cached_snapshot = None
            self._cache_stored_at_ms = None

    def gpio_control_acquire(
            self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, node_id: str, resource_id: str,
            remaining_ttl_ms: Callable[[], int], *,
            deadline: MonotonicDeadline | None = None) -> None:
        if not self.gpio_control_available:
            raise RuntimeProviderError("toolbusd GPIO控制IPC不可用")
        try:
            node_uuid, numeric_node, numeric_resource = \
                self._resolve_gpio_target(
                    node_id, resource_id, deadline=deadline)
            ttl_ms = call_with_deadline(
                remaining_ttl_ms, deadline=deadline)
            # 用不存在的规范ID只读探测 ledger IPC；旧 daemon 必须在任何
            # 本地租约登记前失败关闭，不能静默降级到仅RAM幂等。
            call_with_deadline(
                self.client.runtime_operation_status,
                daemon_instance_id, owner_key_id, "f" * 64,
                deadline=deadline)
        except RuntimeProviderOperationError:
            raise
        except RequestDeadlineExceeded as error:
            # 尚未调用写命令，期限失败确定未提交，可安全重放。
            raise RuntimeProviderOperationError(
                "deadline_exceeded", category="timeout", retryable=True,
                possibly_committed=False) from error
        except ToolbusIpcError as error:
            raise self._operation_query_error(error) from error
        except RuntimeProviderError as error:
            # 快照读取、缓存或目标解析失败均发生在 daemon acquire 之前；
            # 明确标记为确定未提交，让 HTTP 层回滚本次本地租约。
            raise RuntimeProviderOperationError(
                "backend_unavailable", category="transport", retryable=True,
                possibly_committed=False, detail=str(error)) from error
        try:
            call_with_deadline(
                self.client.runtime_control_acquire,
                daemon_instance_id, lease_id, node_uuid, owner_key_id,
                numeric_node, numeric_resource, ttl_ms,
                deadline=deadline)
        except RequestDeadlineExceeded:
            raise
        except ToolbusIpcError as error:
            raise _provider_operation_error(error) from error

    def gpio_control_write(
            self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, node_id: str, resource_id: str,
            idempotency_key: str, value: bool, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if not self.gpio_control_available:
            raise RuntimeProviderError("toolbusd GPIO控制IPC不可用")
        try:
            node_uuid, numeric_node, numeric_resource = \
                self._resolve_gpio_target(
                    node_id, resource_id, deadline=deadline)
        except RuntimeProviderOperationError:
            raise
        except RequestDeadlineExceeded as error:
            raise RuntimeProviderOperationError(
                "deadline_exceeded", category="timeout", retryable=True,
                possibly_committed=False) from error
        except RuntimeProviderError as error:
            # 目标预检尚未触发实际 GPIO I/O，失败可安全直接重试。
            raise RuntimeProviderOperationError(
                "backend_unavailable", category="transport", retryable=True,
                possibly_committed=False, detail=str(error)) from error
        try:
            return call_with_deadline(
                self.client.runtime_gpio_write_operation,
                daemon_instance_id, lease_id, node_uuid, owner_key_id,
                numeric_node, numeric_resource, idempotency_key, value,
                deadline=deadline)
        except RequestDeadlineExceeded:
            raise
        except ToolbusIpcError as error:
            raise _provider_operation_error(error) from error

    def gpio_control_release(self, daemon_instance_id: str, lease_id: str,
                             owner_key_id: str, *,
                             deadline: MonotonicDeadline | None = None) -> dict:
        if not self.gpio_control_available:
            raise RuntimeProviderError("toolbusd GPIO控制IPC不可用")
        try:
            return call_with_deadline(
                self.client.runtime_control_release_operation,
                daemon_instance_id, lease_id, owner_key_id,
                deadline=deadline)
        except RequestDeadlineExceeded:
            raise
        except ToolbusIpcError as error:
            raise _provider_operation_error(error) from error

    def pwm_control_acquire(self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, node_id: str, resource_id: str,
            remaining_ttl_ms: Callable[[], int], *,
            deadline: MonotonicDeadline | None = None) -> None:
        if not self.pwm_control_available:
            raise RuntimeProviderError("toolbusd PWM控制IPC不可用")
        try:
            uuid, numeric_node, numeric_resource = self._resolve_pwm_target(
                node_id, resource_id, deadline=deadline)
            ttl_ms = call_with_deadline(remaining_ttl_ms, deadline=deadline)
            call_with_deadline(self.client.runtime_operation_status,
                daemon_instance_id, owner_key_id, "f" * 64, deadline=deadline)
            call_with_deadline(self.client.runtime_pwm_acquire,
                daemon_instance_id, lease_id, uuid, owner_key_id,
                numeric_node, numeric_resource, ttl_ms, deadline=deadline)
        except RequestDeadlineExceeded:
            raise
        except ToolbusIpcError as error:
            raise _provider_operation_error(error) from error

    def pwm_control_configure(self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, node_id: str, resource_id: str,
            idempotency_key: str, frequency_hz: int, duty: int,
            active_low: bool, *, deadline: MonotonicDeadline | None = None) -> dict:
        if not self.pwm_control_available:
            raise RuntimeProviderError("toolbusd PWM控制IPC不可用")
        try:
            uuid, numeric_node, numeric_resource = self._resolve_pwm_target(
                node_id, resource_id, deadline=deadline)
            outcome = call_with_deadline(self.client.runtime_pwm_configure_operation,
                daemon_instance_id, lease_id, uuid, owner_key_id, numeric_node,
                numeric_resource, idempotency_key, frequency_hz, duty, active_low,
                deadline=deadline)
            self._invalidate_snapshot_cache()
            return outcome
        except RequestDeadlineExceeded:
            raise
        except ToolbusIpcError as error:
            raise _provider_operation_error(error) from error

    def pwm_control_stop(self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, node_id: str, resource_id: str,
            idempotency_key: str, *, deadline: MonotonicDeadline | None = None) -> dict:
        if not self.pwm_control_available:
            raise RuntimeProviderError("toolbusd PWM控制IPC不可用")
        try:
            uuid, numeric_node, numeric_resource = self._resolve_pwm_target(
                node_id, resource_id, deadline=deadline)
            outcome = call_with_deadline(self.client.runtime_pwm_stop_operation,
                daemon_instance_id, lease_id, uuid, owner_key_id, numeric_node,
                numeric_resource, idempotency_key, deadline=deadline)
            self._invalidate_snapshot_cache()
            return outcome
        except RequestDeadlineExceeded:
            raise
        except ToolbusIpcError as error:
            raise _provider_operation_error(error) from error

    def timed_bitstream_control_acquire(self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, node_id: str, resource_id: str,
            remaining_ttl_ms: Callable[[],int], *, deadline: MonotonicDeadline | None=None) -> None:
        if not self.timed_bitstream_control_available: raise RuntimeProviderError("toolbusd定时位流控制IPC不可用")
        try:
            uuid,node,resource=self._resolve_timed_bitstream_target(node_id,resource_id,deadline=deadline)
            ttl=call_with_deadline(remaining_ttl_ms,deadline=deadline)
            call_with_deadline(self.client.runtime_timed_bitstream_acquire,daemon_instance_id,lease_id,uuid,
                owner_key_id,node,resource,ttl,deadline=deadline)
        except RequestDeadlineExceeded: raise
        except ToolbusIpcError as error: raise _provider_operation_error(error) from error

    def timed_bitstream_control_configure(self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str,node_id: str,resource_id: str,idempotency_key: str,
            bit_period_ns: int,zero_high_ns: int,one_high_ns: int,reset_time_us: int,*,deadline=None)->dict:
        try:
            uuid,node,resource=self._resolve_timed_bitstream_target(node_id,resource_id,deadline=deadline)
            outcome=call_with_deadline(self.client.runtime_timed_bitstream_configure_operation,
                daemon_instance_id,lease_id,uuid,owner_key_id,node,resource,idempotency_key,
                bit_period_ns,zero_high_ns,one_high_ns,reset_time_us,deadline=deadline)
            self._invalidate_snapshot_cache(); return outcome
        except RequestDeadlineExceeded: raise
        except ToolbusIpcError as error: raise _provider_operation_error(error) from error

    def timed_bitstream_control_frame(self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str,node_id: str,resource_id: str,idempotency_key: str,
            bit_count: int,data: bytes,*,deadline=None)->dict:
        try:
            uuid,node,resource=self._resolve_timed_bitstream_target(node_id,resource_id,deadline=deadline)
            outcome=call_with_deadline(self.client.runtime_timed_bitstream_frame_operation,
                daemon_instance_id,lease_id,uuid,owner_key_id,node,resource,idempotency_key,
                bit_count,data,deadline=deadline)
            self._invalidate_snapshot_cache(); return outcome
        except RequestDeadlineExceeded: raise
        except ToolbusIpcError as error: raise _provider_operation_error(error) from error

    def timed_bitstream_control_stop(self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str,node_id: str,resource_id: str,idempotency_key: str,*,deadline=None)->dict:
        try:
            uuid,node,resource=self._resolve_timed_bitstream_target(node_id,resource_id,deadline=deadline)
            outcome=call_with_deadline(self.client.runtime_timed_bitstream_stop_operation,
                daemon_instance_id,lease_id,uuid,owner_key_id,node,resource,idempotency_key,deadline=deadline)
            self._invalidate_snapshot_cache(); return outcome
        except RequestDeadlineExceeded: raise
        except ToolbusIpcError as error: raise _provider_operation_error(error) from error

    def bus_reset_control_acquire(self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, node_id: str, resource_id: str,
            remaining_ttl_ms: Callable[[], int], *, deadline=None) -> None:
        if not self.bus_reset_control_available:
            raise RuntimeProviderError("toolbusd总线复位控制IPC不可用")
        try:
            uuid, node, resource = self._resolve_bus_reset_target(
                node_id, resource_id, deadline=deadline)
            ttl_ms = call_with_deadline(remaining_ttl_ms, deadline=deadline)
            call_with_deadline(self.client.runtime_operation_status,
                daemon_instance_id, owner_key_id, "f" * 64, deadline=deadline)
            call_with_deadline(self.client.runtime_bus_reset_acquire,
                daemon_instance_id, lease_id, uuid, owner_key_id, node,
                resource, ttl_ms, deadline=deadline)
        except RequestDeadlineExceeded:
            raise
        except ToolbusIpcError as error:
            raise _provider_operation_error(error) from error

    def bus_reset_control_execute(self, daemon_instance_id: str, lease_id: str,
            owner_key_id: str, node_id: str, resource_id: str,
            idempotency_key: str, *, deadline=None) -> dict:
        if not self.bus_reset_control_available:
            raise RuntimeProviderError("toolbusd总线复位控制IPC不可用")
        try:
            uuid, node, resource = self._resolve_bus_reset_target(
                node_id, resource_id, deadline=deadline)
            outcome = call_with_deadline(
                self.client.runtime_bus_resource_reset_operation,
                daemon_instance_id, lease_id, uuid, owner_key_id, node,
                resource, idempotency_key, deadline=deadline)
            self._invalidate_snapshot_cache()
            return outcome
        except RequestDeadlineExceeded:
            raise
        except ToolbusIpcError as error:
            raise _provider_operation_error(error) from error

    def operation_status(
            self, daemon_instance_id: str, owner_key_id: str,
            operation_id: str, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if not self.gpio_control_available:
            raise RuntimeProviderOperationError(
                "protocol_incompatible", category="protocol",
                retryable=False, possibly_committed=False,
                invalidates_global_operational=True)

        def invoke() -> dict:
            backend_deadline = MonotonicDeadline.after_seconds(
                self.refresh_wait_timeout_ms / 1000.0)
            try:
                return call_with_deadline(
                    self.client.runtime_operation_status,
                    daemon_instance_id, owner_key_id, operation_id,
                    deadline=backend_deadline)
            except RequestDeadlineExceeded:
                raise
            except ToolbusIpcError as error:
                raise self._operation_query_error(error) from error

        return self._operation_singleflight(
            ("status", daemon_instance_id, owner_key_id, operation_id),
            invoke, deadline=deadline)

    def operation_lookup(
            self, daemon_instance_id: str, owner_key_id: str, kind: str,
            lease_id: str, idempotency_key: str, *,
            deadline: MonotonicDeadline | None = None) -> dict:
        if not self.gpio_control_available:
            raise RuntimeProviderOperationError(
                "protocol_incompatible", category="protocol",
                retryable=False, possibly_committed=False,
                invalidates_global_operational=True)

        def invoke() -> dict:
            backend_deadline = MonotonicDeadline.after_seconds(
                self.refresh_wait_timeout_ms / 1000.0)
            try:
                return call_with_deadline(
                    self.client.runtime_operation_lookup,
                    daemon_instance_id, owner_key_id, kind, lease_id,
                    idempotency_key, deadline=backend_deadline)
            except RequestDeadlineExceeded:
                raise
            except ToolbusIpcError as error:
                raise self._operation_query_error(error) from error

        return self._operation_singleflight(
            ("lookup", daemon_instance_id, owner_key_id, kind, lease_id,
             idempotency_key), invoke, deadline=deadline)

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

    def _read_resource_status(
            self, client: ToolbusIpcClient, node_id: int,
            descriptor: dict, deadline: MonotonicDeadline | None = None
    ) -> tuple[dict | None, ToolbusIpcError | None]:
        try:
            return call_with_deadline(
                client.resource_status, node_id,
                int(descriptor["resource_id"]), deadline=deadline), None
        except ToolbusIpcProtocolError:
            raise
        except ToolbusIpcError as error:
            return None, error

    def _read_resource_statuses(
            self, client: ToolbusIpcClient, node_id: int,
            descriptors: list[dict], deadline: MonotonicDeadline | None
    ) -> list[tuple[dict | None, ToolbusIpcError | None]]:
        """共享绝对期限，并确保旧代任务退出前不再提交新一代。"""
        if deadline is not None:
            deadline.check()
        if not descriptors:
            return []

        with self._status_condition:
            while self._status_inflight is not None:
                if all(future.done()
                       for future in self._status_inflight):
                    self._status_inflight = None
                    break
                timeout = None if deadline is None else \
                    deadline.remaining_seconds()
                self._status_condition.wait(timeout)
                if deadline is not None:
                    deadline.check()

            # 空列表是“正在发布新代次”的哨兵；当前持有条件锁，其他调用
            # 不会观察到一半提交的代次。
            futures = []
            self._status_inflight = futures
            try:
                for descriptor in descriptors:
                    futures.append(self._status_executor.submit(
                        self._read_resource_status, client, node_id,
                        descriptor, deadline))
            except BaseException:
                # submit 本身也可能在部分任务已经开始后失败；这些任务同样
                # 必须负责在最后一个退出时解锁代次。
                for future in futures:
                    future.add_done_callback(self._status_future_done)
                for future in futures:
                    future.cancel()
                if all(future.done() for future in futures):
                    self._status_inflight = None
                self._status_condition.notify_all()
                raise
            for future in futures:
                future.add_done_callback(self._status_future_done)

        try:
            results = []
            for future in futures:
                timeout = None if deadline is None else \
                    deadline.remaining_seconds()
                try:
                    results.append(future.result(timeout=timeout))
                except FutureTimeout as error:
                    raise RequestDeadlineExceeded(
                        "资源状态并发查询期限已耗尽") from error
            if deadline is not None:
                deadline.check()
        except BaseException:
            for future in futures:
                future.cancel()
            with self._status_condition:
                if self._status_inflight is futures and all(
                        item.done() for item in futures):
                    self._status_inflight = None
                self._status_condition.notify_all()
            raise
        return results

    def _status_future_done(self, _future) -> None:
        """最后一个旧代任务退出后，允许下一次刷新发布新代次。"""
        with self._status_condition:
            futures = self._status_inflight
            if futures is not None and futures and all(
                    future.done() for future in futures):
                self._status_inflight = None
            self._status_condition.notify_all()

    def _clock_quality_alerts(self, node_id: str, numeric_id: int,
                              clock: dict,
                              captured_at_ms: int) -> list[dict]:
        if not bool(clock["source_available"]):
            return [self._alert(
                node_id, f"clock-observability-{numeric_id}",
                "clock_sync_observability_unavailable",
                "当前数据源不提供主机时钟模型质量观测，不能据此判断节点同步状态",
                captured_at_ms, severity="info")]
        if not bool(clock["registered"]):
            return [self._alert(
                node_id, f"clock-unregistered-{numeric_id}",
                "clock_sync_unregistered",
                "节点尚未注册主机时钟同步模型",
                captured_at_ms, severity="warning")]

        result: list[dict] = []
        state = str(clock["state"])
        if state == "unsynced":
            result.append(self._alert(
                node_id, f"clock-unsynced-{numeric_id}",
                "clock_sync_unsynced",
                "节点主机时钟模型未同步，不能用于跨板运动准入",
                captured_at_ms, severity="warning"))
        elif state == "degraded":
            result.append(self._alert(
                node_id, f"clock-degraded-{numeric_id}",
                "clock_sync_degraded",
                "节点主机时钟模型已降级，不能用于新的跨板运动准入",
                captured_at_ms, severity="warning"))

        if bool(clock["estimate_valid"]):
            error_bound_ns = int(clock["error_bound_ns"])
            if error_bound_ns > self.maximum_clock_error_bound_ns:
                result.append(self._alert(
                    node_id, f"clock-error-bound-{numeric_id}",
                    "clock_sync_error_bound_exceeded",
                    f"主机时钟模型估计误差上界{error_bound_ns}纳秒，超过Runtime告警阈值"
                    f"{self.maximum_clock_error_bound_ns}纳秒",
                    captured_at_ms, severity="warning"))
            sample_age_ns = self._clock_age_at_capture_ns(
                clock, captured_at_ms)
            age_threshold_ns = self.maximum_clock_sample_age_ms * 1_000_000
            if sample_age_ns > age_threshold_ns:
                result.append(self._alert(
                    node_id, f"clock-sample-stale-{numeric_id}",
                    "clock_sync_sample_stale",
                    f"主机时钟模型入选样本年龄{sample_age_ns}纳秒，超过Runtime告警阈值"
                    f"{self.maximum_clock_sample_age_ms}毫秒",
                    captured_at_ms, severity="warning"))
        return result

    @staticmethod
    def _bus_health_detail(value: dict | None) -> dict:
        """把稳定状态码投影为带单位的 Web 模型；缺失累计值明确未知。"""
        if value is None or not value["last_status_valid"]:
            return {"availability": "unknown", "last_status": "unknown",
                    "last_result_age_ms": None, "consecutive_failures": 0,
                    "peak_consecutive_failures": 0,
                    "cumulative_failures": None,
                    "cumulative_availability": "unavailable"}
        status_names = ("ok", "nack", "timeout", "busy", "fault",
                        "limit_exceeded")
        now_us = time.monotonic_ns() // 1000
        last_us = int(value["last_result_time_us"])
        age_ms = max(0, (now_us - last_us) // 1000) if last_us <= now_us else 0
        return {"availability": "available",
                "last_status": status_names[int(value["last_status"])],
                "last_result_age_ms": age_ms,
                "consecutive_failures": int(value["consecutive_failures"]),
                "peak_consecutive_failures": int(
                    value["peak_consecutive_failures"]),
                "cumulative_failures": None,
                "cumulative_availability": "unavailable"}

    @staticmethod
    def _clock_age_at_capture_ns(clock: dict, captured_at_ms: int) -> int:
        reported_age_ns = int(clock["sample_age_ns"])
        last_sample_ns = int(clock["last_sample_host_time_ns"])
        captured_ns = captured_at_ms * 1_000_000
        if captured_ns < last_sample_ns:
            return reported_age_ns
        return max(reported_age_ns, captured_ns - last_sample_ns)

    def get_snapshot(self) -> dict:
        return self.read_snapshot().snapshot

    def read_snapshot(
            self, *, deadline: MonotonicDeadline | None = None) -> SnapshotRead:
        """合并并发刷新，并返回缓存年龄；刷新失败不提供陈旧回退。"""
        if deadline is not None:
            deadline.check()
        now_ms = self._clock_value()
        with self._cache_condition:
            cached = self._cached_read(now_ms)
            if cached is not None:
                return cached
            self._raise_cached_failure(now_ms)
            if self._refreshing:
                wait_expires = time.monotonic() + \
                    self.refresh_wait_timeout_ms / 1000.0
                while self._refreshing:
                    remaining = wait_expires - time.monotonic()
                    if deadline is not None:
                        remaining = min(
                            remaining, deadline.remaining_seconds())
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
            snapshot = self._build_snapshot(deadline=deadline)
            stored_at_ms = self._clock_value()
            if deadline is not None:
                deadline.check()
        except RequestDeadlineExceeded:
            with self._cache_condition:
                self._refreshing = False
                self._cache_condition.notify_all()
            raise
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
        if self.cache_ttl_ms == 0 or age_ms > self.cache_ttl_ms or \
                self._cache_crossed_clock_stale_threshold(
                    self._cached_snapshot, now_ms):
            return None
        return SnapshotRead(
            snapshot=copy.deepcopy(self._cached_snapshot),
            cache_status="hit",
            age_ms=max(0, now_ms - int(
                self._cached_snapshot["captured_at_ms"])),
            cache_ttl_ms=self.cache_ttl_ms,
        )

    def _cache_crossed_clock_stale_threshold(
            self, snapshot: dict, now_ms: int) -> bool:
        """检查原本未陈旧的模型是否在缓存期间跨过告警阈值。"""
        threshold_ns = self.maximum_clock_sample_age_ms * 1_000_000
        captured_at_ms = int(snapshot["captured_at_ms"])
        elapsed_ns = max(0, now_ms - captured_at_ms) * 1_000_000
        for node in snapshot["nodes"]:
            clock = node["runtime"].get("clock_sync")
            if not isinstance(clock, dict) or \
                    not bool(clock.get("source_available")) or \
                    not bool(clock.get("estimate_valid")):
                continue
            sample_age_ns = self._clock_age_at_capture_ns(
                clock, captured_at_ms)
            if sample_age_ns <= threshold_ns and \
                    sample_age_ns + elapsed_ns > threshold_ns:
                return True
        return False

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

    def _build_snapshot(
            self, *, deadline: MonotonicDeadline | None = None) -> dict:
        captured_at_ms = self._clock_value()
        source_client: ToolbusIpcClient = self.client
        source_sequence: int | None = None
        source_clocks: dict[int, dict] = {}
        source_bus_health: dict[tuple[int, int], dict] = {}
        snapshot_reader = getattr(self.client, "runtime_snapshot", None)
        structured_output = getattr(self.client, "structured_output", True)
        if callable(snapshot_reader) and structured_output:
            try:
                source = call_with_deadline(
                    snapshot_reader, self.maximum_resources_per_snapshot,
                    deadline=deadline)
            except ToolbusIpcProtocolError as error:
                raise RuntimeProviderError(
                    f"toolbusd IPC协议不兼容：{error}") from error
            except ToolbusIpcError as error:
                raise RuntimeProviderError(
                    f"toolbusd IPC不可用：{error}") from error
            source_client = _RuntimeSnapshotView(source)
            source_sequence = int(source["sequence"])
            source_clocks = {
                int(clock["node_id"]): copy.deepcopy(clock)
                for clock in source["clocks"]
            }
            source_bus_health = {
                (int(item["node_id"]), int(item["resource_id"])):
                    copy.deepcopy(item)
                for item in source["bus_health"]
            }
            captured_at_ms = self._clock_value()
        try:
            traffic = call_with_deadline(
                source_client.traffic_status, deadline=deadline)
            source_nodes = call_with_deadline(
                source_client.list_nodes, deadline=deadline)
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
                node_health = self._node_health(
                    uuid, numeric_id, captured_at_ms, deadline=deadline)
                health_snapshot = node_health.get("snapshot")
                if node_health["availability"] == "available" and \
                        isinstance(health_snapshot, dict) and \
                        health_snapshot.get("overall") in {"degraded", "fault"}:
                    state = "degraded"
                    alerts.append(self._alert(
                        node_id, f"node-health-{numeric_id}",
                        "node_health_degraded",
                        "节点健康快照报告降级或故障", captured_at_ms,
                        severity=("error" if health_snapshot["overall"] ==
                                  "fault" else "warning")))
                elif node_health["availability"] == "unavailable":
                    alerts.append(self._alert(
                        node_id, f"node-health-unavailable-{numeric_id}",
                        "node_health_unavailable",
                        "节点健康快照不可用，不能据缺失数据判断节点健康",
                        captured_at_ms, severity="warning"))
            else:
                node_health = {
                    "availability": "unavailable" if online else "unknown",
                    "reason": "node_not_ready" if online else "node_offline",
                    "snapshot": None, "sample_age_ms": None,
                }
            if online and ready:
                try:
                    descriptors = call_with_deadline(
                        source_client.list_resources, numeric_id,
                        deadline=deadline)
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
                    status_results = self._read_resource_statuses(
                        source_client, numeric_id, descriptors, deadline)
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
                            "bus_health": self._bus_health_detail(
                                source_bus_health.get(
                                    (numeric_id, raw_resource_id))),
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
            clock_quality = source_clocks.get(numeric_id)
            if clock_quality is None:
                clock_runtime = {
                    "source_available": False,
                    "registered": None,
                    "estimate_valid": False,
                    "state": "unknown",
                    "boot_epoch": None,
                    "model_generation": None,
                    "sample_count": 0,
                    "selected_sample_count": 0,
                    "rate_deviation_ppb": None,
                    "drift_uncertainty_ppm": None,
                    "minimum_network_rtt_ns": None,
                    "error_bound_ns": None,
                    "sample_age_ns": None,
                    "last_sample_host_time_ns": None,
                }
            else:
                clock_runtime = copy.deepcopy(clock_quality)
                clock_runtime.pop("node_id", None)
                clock_runtime["source_available"] = True
            alerts.extend(self._clock_quality_alerts(
                node_id, numeric_id, clock_runtime, captured_at_ms))
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
                    "health_snapshot": node_health,
                    "clock_sync": clock_runtime,
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

"""Studio 到 Runtime PWM 的最小认证代理。"""

from __future__ import annotations

import ipaddress
import json
import socket
from dataclasses import dataclass
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen


class RuntimePwmProxyError(RuntimeError):
    def __init__(self, message: str, status: int = 502):
        super().__init__(message)
        self.status = status


@dataclass(frozen=True)
class RuntimePwmProxyResponse:
    status: int
    document: dict


class RuntimePwmProxy:
    """仅转发 PWM configure/stop 和只读 snapshot，不开放通用反向代理。"""

    MAXIMUM_BODY_BYTES = 4096
    MAXIMUM_RESPONSE_BYTES = 1024 * 1024
    _CONFIGURE_FIELDS = frozenset({
        "lease_id", "node_id", "resource_id", "idempotency_key",
        "frequency_hz", "duty", "active_low",
    })
    _STOP_FIELDS = frozenset({
        "lease_id", "node_id", "resource_id", "idempotency_key",
    })
    _BITSTREAM_COMMON = frozenset({
        "lease_id", "node_id", "resource_id", "idempotency_key"})
    _BITSTREAM_CONFIGURE = _BITSTREAM_COMMON | frozenset({
        "bit_period_ns", "zero_high_ns", "one_high_ns", "reset_time_us"})
    _BITSTREAM_FRAME = _BITSTREAM_COMMON | frozenset({"bit_count", "data"})

    def __init__(self, upstream: str, api_key: str, *, timeout_seconds: float = 3.0):
        parsed = urlsplit(upstream)
        if parsed.scheme != "http" or parsed.username is not None or \
                parsed.password is not None or parsed.query or parsed.fragment or \
                parsed.path not in {"", "/"} or parsed.hostname is None:
            raise ValueError("Runtime上游必须是无凭据、无路径的本机HTTP地址")
        try:
            if not ipaddress.ip_address(parsed.hostname).is_loopback:
                raise ValueError("Runtime上游只允许回环地址")
        except ValueError as error:
            raise ValueError("Runtime上游只允许数字回环地址") from error
        if parsed.port is None:
            raise ValueError("Runtime上游必须显式指定端口")
        if not api_key or len(api_key) > 512 or any(char.isspace() for char in api_key):
            raise ValueError("Runtime API key无效")
        if not 0.1 <= timeout_seconds <= 10.0:
            raise ValueError("Runtime代理超时必须位于0.1～10秒")
        host = f"[{parsed.hostname}]" if ":" in parsed.hostname else parsed.hostname
        self._base = f"http://{host}:{parsed.port}"
        self._api_key = api_key
        self._timeout = float(timeout_seconds)

    @classmethod
    def from_key_file(cls, upstream: str, key_file: Path, *,
                      timeout_seconds: float = 3.0) -> "RuntimePwmProxy":
        try:
            raw = key_file.read_bytes()
        except OSError as error:
            raise ValueError(f"无法读取Runtime API key文件：{error}") from error
        if len(raw) > 513:
            raise ValueError("Runtime API key文件过大")
        try:
            key = raw.decode("utf-8").strip()
        except UnicodeDecodeError as error:
            raise ValueError("Runtime API key文件必须是UTF-8") from error
        return cls(upstream, key, timeout_seconds=timeout_seconds)

    @staticmethod
    def validate_body(body: bytes, *, stop: bool) -> dict:
        if not body or len(body) > RuntimePwmProxy.MAXIMUM_BODY_BYTES:
            raise RuntimePwmProxyError("PWM请求体为空或超过4096字节", 400)
        try:
            value = json.loads(body)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise RuntimePwmProxyError("PWM请求体不是合法JSON", 400) from error
        expected = RuntimePwmProxy._STOP_FIELDS if stop else \
            RuntimePwmProxy._CONFIGURE_FIELDS
        if not isinstance(value, dict) or set(value) != expected:
            raise RuntimePwmProxyError("PWM请求字段不完整或包含未知字段", 400)
        return value

    def _request(self, path: str, *, body: dict | None = None) -> RuntimePwmProxyResponse:
        encoded = None if body is None else json.dumps(
            body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        headers = {"Authorization": "Bearer " + self._api_key,
                   "Accept": "application/json"}
        if encoded is not None:
            headers["Content-Type"] = "application/json"
        request = Request(self._base + path, data=encoded, headers=headers,
                          method="GET" if encoded is None else "POST")
        try:
            response = urlopen(request, timeout=self._timeout)
        except HTTPError as error:
            response = error
        except (URLError, TimeoutError, socket.timeout, OSError) as error:
            raise RuntimePwmProxyError("Runtime服务暂不可用") from error
        try:
            raw = response.read(self.MAXIMUM_RESPONSE_BYTES + 1)
            if len(raw) > self.MAXIMUM_RESPONSE_BYTES:
                raise RuntimePwmProxyError("Runtime响应超过大小上限")
            document = json.loads(raw)
            if not isinstance(document, dict):
                raise ValueError
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            raise RuntimePwmProxyError("Runtime返回了无效JSON") from error
        finally:
            response.close()
        return RuntimePwmProxyResponse(response.status, document)

    def configure(self, body: bytes) -> RuntimePwmProxyResponse:
        return self._request("/api/v1/control/pwm/configure",
                             body=self.validate_body(body, stop=False))

    def stop(self, body: bytes) -> RuntimePwmProxyResponse:
        return self._request("/api/v1/control/pwm/stop",
                             body=self.validate_body(body, stop=True))

    def snapshot(self) -> RuntimePwmProxyResponse:
        return self._request("/api/v1/snapshot")

    def timed_bitstream(self, operation: str, body: bytes) -> RuntimePwmProxyResponse:
        expected = {"configure": self._BITSTREAM_CONFIGURE,
                    "frame": self._BITSTREAM_FRAME,
                    "stop": self._BITSTREAM_COMMON}.get(operation)
        if expected is None or not body or len(body) > self.MAXIMUM_BODY_BYTES:
            raise RuntimePwmProxyError("定时位流请求无效或超过4096字节", 400)
        try:
            value = json.loads(body)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise RuntimePwmProxyError("定时位流请求不是合法JSON", 400) from error
        if not isinstance(value, dict) or set(value) != expected:
            raise RuntimePwmProxyError("定时位流请求字段不完整或包含未知字段", 400)
        if operation == "frame":
            bits, data = value["bit_count"], value["data"]
            if type(bits) is not int or not 1 <= bits <= 6144 or \
                    not isinstance(data, str) or len(data) != ((bits + 7) // 8) * 2 or \
                    any(char not in "0123456789abcdef" for char in data):
                raise RuntimePwmProxyError("WS2812帧必须是最多256像素的小写完整字节流", 400)
        return self._request(f"/api/v1/control/timed-bitstream/{operation}",
                             body=value)

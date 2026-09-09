"""Runtime HTTP API 的有界 API 密钥认证配置。"""

from __future__ import annotations

import hmac
import json
from pathlib import Path
from typing import Any


AUTH_CONFIG_SCHEMA_VERSION = 1
MAXIMUM_AUTH_CONFIG_BYTES = 16 * 1024
MAXIMUM_API_KEYS = 16
MINIMUM_API_KEY_BYTES = 32
MAXIMUM_API_KEY_BYTES = 128


class AuthConfigurationError(ValueError):
    """认证配置不可安全使用。"""


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise AuthConfigurationError(f"认证配置包含重复字段：{key}")
        value[key] = item
    return value


class ApiKeyAuthenticator:
    """保存固定上限的密钥，并以常量时间原语逐项比较。"""

    def __init__(self, api_keys: list[str] | tuple[str, ...]):
        if not isinstance(api_keys, (list, tuple)):
            raise AuthConfigurationError("api_keys必须是数组")
        if not 1 <= len(api_keys) <= MAXIMUM_API_KEYS:
            raise AuthConfigurationError(
                f"api_keys数量必须位于1～{MAXIMUM_API_KEYS}")

        encoded: list[bytes] = []
        for index, key in enumerate(api_keys):
            if not isinstance(key, str):
                raise AuthConfigurationError(
                    f"api_keys[{index}]必须是字符串")
            try:
                raw = key.encode("ascii")
            except UnicodeEncodeError as error:
                raise AuthConfigurationError(
                    f"api_keys[{index}]必须仅包含ASCII字符") from error
            if not MINIMUM_API_KEY_BYTES <= len(raw) <= MAXIMUM_API_KEY_BYTES:
                raise AuthConfigurationError(
                    f"api_keys[{index}]长度必须位于"
                    f"{MINIMUM_API_KEY_BYTES}～{MAXIMUM_API_KEY_BYTES}字节")
            if any(byte <= 0x20 or byte >= 0x7f for byte in raw):
                raise AuthConfigurationError(
                    f"api_keys[{index}]不能包含空白或控制字符")
            if raw in encoded:
                raise AuthConfigurationError("api_keys不能包含重复密钥")
            encoded.append(raw)
        self._api_keys = tuple(encoded)

    @property
    def key_count(self) -> int:
        return len(self._api_keys)

    def verify(self, candidate: str) -> bool:
        """比较全部已配置密钥；无论匹配位置如何都不提前返回。"""
        if not isinstance(candidate, str):
            return False
        try:
            raw = candidate.encode("ascii")
        except UnicodeEncodeError:
            return False
        if len(raw) > MAXIMUM_API_KEY_BYTES:
            return False
        matched = False
        for api_key in self._api_keys:
            matched = hmac.compare_digest(raw, api_key) or matched
        return matched


def load_api_key_authenticator(path: Path) -> ApiKeyAuthenticator:
    """从有限大小、版本化且封闭的 JSON 文件加载密钥。"""
    try:
        with path.open("rb") as stream:
            raw = stream.read(MAXIMUM_AUTH_CONFIG_BYTES + 1)
    except OSError as error:
        raise AuthConfigurationError(f"无法读取认证配置：{error}") from error
    if len(raw) > MAXIMUM_AUTH_CONFIG_BYTES:
        raise AuthConfigurationError(
            f"认证配置不得超过{MAXIMUM_AUTH_CONFIG_BYTES}字节")
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_strict_object)
    except AuthConfigurationError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AuthConfigurationError("认证配置不是合法UTF-8 JSON") from error
    if not isinstance(value, dict):
        raise AuthConfigurationError("认证配置根值必须是对象")
    if set(value) != {"schema_version", "api_keys"}:
        raise AuthConfigurationError(
            "认证配置字段必须且只能包含schema_version和api_keys")
    if type(value["schema_version"]) is not int or \
            value["schema_version"] != AUTH_CONFIG_SCHEMA_VERSION:
        raise AuthConfigurationError(
            f"仅支持认证配置版本{AUTH_CONFIG_SCHEMA_VERSION}")
    return ApiKeyAuthenticator(value["api_keys"])

"""Runtime HTTP API 的有界 API 密钥认证配置。"""

from __future__ import annotations

import hashlib
import hmac
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any


AUTH_CONFIG_SCHEMA_VERSION = 2
MAXIMUM_AUTH_CONFIG_BYTES = 16 * 1024
MAXIMUM_API_KEYS = 16
MINIMUM_API_KEY_BYTES = 32
MAXIMUM_API_KEY_BYTES = 128
MAXIMUM_KEY_ID_BYTES = 64
MAXIMUM_PERMISSIONS_PER_KEY = 8
RUNTIME_READ_PERMISSION = "runtime.read"
CONTROL_LEASE_ACQUIRE_PERMISSION = "runtime.control.lease.acquire"
CONTROL_LEASE_RELEASE_PERMISSION = "runtime.control.lease.release"
CONTROL_LEASE_REVOKE_PERMISSION = "runtime.control.lease.revoke"
_KNOWN_PERMISSIONS = frozenset({
    RUNTIME_READ_PERMISSION,
    CONTROL_LEASE_ACQUIRE_PERMISSION,
    CONTROL_LEASE_RELEASE_PERMISSION,
    CONTROL_LEASE_REVOKE_PERMISSION,
})
_KEY_ID_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")


class AuthConfigurationError(ValueError):
    """认证配置不可安全使用。"""


@dataclass(frozen=True)
class ApiKeyCredential:
    """一条经校验的密钥身份与显式权限。"""

    key_id: str
    api_key: str
    permissions: frozenset[str]


@dataclass(frozen=True)
class AuthenticatedPrincipal:
    """认证成功后向授权层暴露的脱敏身份。"""

    key_id: str
    permissions: frozenset[str]


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise AuthConfigurationError(f"认证配置包含重复字段：{key}")
        value[key] = item
    return value


class ApiKeyAuthenticator:
    """保存固定上限的密钥，并以常量时间原语逐项比较。"""

    def __init__(self, credentials: list[ApiKeyCredential] |
                 tuple[ApiKeyCredential, ...]):
        if not isinstance(credentials, (list, tuple)):
            raise AuthConfigurationError("keys必须是数组")
        if not 1 <= len(credentials) <= MAXIMUM_API_KEYS:
            raise AuthConfigurationError(
                f"keys数量必须位于1～{MAXIMUM_API_KEYS}")

        encoded: list[tuple[bytes, AuthenticatedPrincipal]] = []
        raw_keys: set[bytes] = set()
        key_ids: set[str] = set()
        for index, credential in enumerate(credentials):
            if not isinstance(credential, ApiKeyCredential):
                raise AuthConfigurationError(
                    f"keys[{index}]必须是ApiKeyCredential")
            key_id = credential.key_id
            if not isinstance(key_id, str) or \
                    _KEY_ID_PATTERN.fullmatch(key_id) is None:
                raise AuthConfigurationError(
                    f"keys[{index}].key_id格式无效")
            if len(key_id.encode("ascii")) > MAXIMUM_KEY_ID_BYTES:
                raise AuthConfigurationError(
                    f"keys[{index}].key_id超过{MAXIMUM_KEY_ID_BYTES}字节")
            if key_id in key_ids:
                raise AuthConfigurationError("key_id不能重复")
            key_ids.add(key_id)

            key = credential.api_key
            if not isinstance(key, str):
                raise AuthConfigurationError(f"keys[{index}].api_key必须是字符串")
            try:
                raw = key.encode("ascii")
            except UnicodeEncodeError as error:
                raise AuthConfigurationError(
                    f"keys[{index}].api_key必须仅包含ASCII字符") from error
            if not MINIMUM_API_KEY_BYTES <= len(raw) <= MAXIMUM_API_KEY_BYTES:
                raise AuthConfigurationError(
                    f"keys[{index}].api_key长度必须位于"
                    f"{MINIMUM_API_KEY_BYTES}～{MAXIMUM_API_KEY_BYTES}字节")
            if any(byte <= 0x20 or byte >= 0x7f for byte in raw):
                raise AuthConfigurationError(
                    f"keys[{index}].api_key不能包含空白或控制字符")
            if raw in raw_keys:
                raise AuthConfigurationError("api_key不能重复")
            raw_keys.add(raw)

            permissions = credential.permissions
            if not isinstance(permissions, frozenset):
                raise AuthConfigurationError(
                    f"keys[{index}].permissions必须是权限集合")
            if len(permissions) > MAXIMUM_PERMISSIONS_PER_KEY:
                raise AuthConfigurationError(
                    f"keys[{index}].permissions超过上限")
            unknown = permissions - _KNOWN_PERMISSIONS
            if unknown:
                raise AuthConfigurationError(
                    f"keys[{index}].permissions包含未知权限")
            encoded.append((hashlib.sha256(raw).digest(), AuthenticatedPrincipal(
                key_id=key_id, permissions=permissions)))
        self._credentials = tuple(encoded)

    @property
    def key_count(self) -> int:
        return len(self._credentials)

    def authenticate(self, candidate: str) -> AuthenticatedPrincipal | None:
        """比较全部密钥，并返回不含密钥材料的稳定身份。"""
        if not isinstance(candidate, str):
            return None
        raw = candidate.encode("utf-8")
        eligible = len(raw) <= MAXIMUM_API_KEY_BYTES and \
            all(0x20 < byte < 0x7f for byte in raw)
        candidate_digest = hashlib.sha256(raw).digest()
        matched: AuthenticatedPrincipal | None = None
        for api_key_digest, principal in self._credentials:
            if hmac.compare_digest(candidate_digest, api_key_digest) and eligible:
                matched = principal
        return matched

    def verify(self, candidate: str) -> bool:
        """兼容布尔调用；授权层应使用 ``authenticate``。"""
        return self.authenticate(candidate) is not None


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
    if set(value) != {"schema_version", "keys"}:
        raise AuthConfigurationError(
            "认证配置字段必须且只能包含schema_version和keys")
    if type(value["schema_version"]) is not int or \
            value["schema_version"] != AUTH_CONFIG_SCHEMA_VERSION:
        raise AuthConfigurationError(
            f"仅支持认证配置版本{AUTH_CONFIG_SCHEMA_VERSION}")
    keys = value["keys"]
    if not isinstance(keys, list):
        raise AuthConfigurationError("keys必须是数组")
    if not 1 <= len(keys) <= MAXIMUM_API_KEYS:
        raise AuthConfigurationError(
            f"keys数量必须位于1～{MAXIMUM_API_KEYS}")
    credentials: list[ApiKeyCredential] = []
    for index, entry in enumerate(keys):
        if not isinstance(entry, dict):
            raise AuthConfigurationError(f"keys[{index}]必须是对象")
        if set(entry) != {"key_id", "api_key", "permissions"}:
            raise AuthConfigurationError(
                f"keys[{index}]字段必须且只能包含key_id、api_key和permissions")
        permissions = entry["permissions"]
        if not isinstance(permissions, list):
            raise AuthConfigurationError(
                f"keys[{index}].permissions必须是数组")
        if len(permissions) > MAXIMUM_PERMISSIONS_PER_KEY:
            raise AuthConfigurationError(
                f"keys[{index}].permissions超过上限")
        if any(not isinstance(permission, str) for permission in permissions):
            raise AuthConfigurationError(
                f"keys[{index}].permissions必须只包含字符串")
        if len(set(permissions)) != len(permissions):
            raise AuthConfigurationError(
                f"keys[{index}].permissions不能重复")
        credentials.append(ApiKeyCredential(
            key_id=entry["key_id"],
            api_key=entry["api_key"],
            permissions=frozenset(permissions)))
    return ApiKeyAuthenticator(credentials)

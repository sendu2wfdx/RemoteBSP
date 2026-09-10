"""Runtime 本机 TLS 反向代理部署基线的离线工件与预检。"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
import secrets
import ssl
import stat
from pathlib import Path


SCHEMA_VERSION = 1
KIND = "remotebsp-runtime-tls-baseline"
FIELDS = {"schema_version", "kind", "runtime_bind", "proxy_bind",
          "certificate_file", "private_key_file", "forwarded_headers",
          "client_address_source"}


class TlsDeploymentError(RuntimeError):
    pass


def _numeric_loopback(value: str, field: str) -> str:
    try:
        address = ipaddress.ip_address(value)
    except ValueError as error:
        raise TlsDeploymentError(f"{field} 必须是数字 IP 地址") from error
    if not address.is_loopback:
        raise TlsDeploymentError(f"{field} 必须是回环地址")
    return address.compressed


def _secure_regular(path: Path, *, private: bool) -> None:
    if not path.is_absolute():
        raise TlsDeploymentError("证书和私钥必须使用绝对路径")
    try:
        if path.is_symlink():
            raise TlsDeploymentError(f"路径不能是符号链接：{path}")
        info = path.stat()
    except OSError as error:
        raise TlsDeploymentError(f"无法读取 TLS 文件：{path}") from error
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise TlsDeploymentError(f"TLS 文件必须是单链接普通文件：{path}")
    if os.name == "posix" and info.st_uid != os.geteuid():
        raise TlsDeploymentError(f"TLS 文件所有者不是当前服务用户：{path}")
    permissions = stat.S_IMODE(info.st_mode)
    if private and permissions != 0o600:
        raise TlsDeploymentError("TLS 私钥权限必须严格为0600")
    if not private and permissions & 0o022:
        raise TlsDeploymentError("TLS 证书不能允许组或其他用户写入")


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _atomic_write(path: Path, data: bytes) -> None:
    temporary = path.parent / f".{path.name}.{secrets.token_hex(8)}.tmp"
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, path)
        path.chmod(0o600)
    finally:
        temporary.unlink(missing_ok=True)


def create_artifact(output: Path, *, runtime_bind: str, proxy_bind: str,
                    certificate_file: Path, private_key_file: Path) -> dict:
    document = {
        "schema_version": SCHEMA_VERSION, "kind": KIND,
        "runtime_bind": _numeric_loopback(runtime_bind, "runtime_bind"),
        "proxy_bind": _numeric_loopback(proxy_bind, "proxy_bind"),
        "certificate_file": str(certificate_file),
        "private_key_file": str(private_key_file),
        # Runtime 不消费任何代理注入身份或客户端地址头。
        "forwarded_headers": "strip_all",
        "client_address_source": "direct_peer_only",
    }
    encoded = (json.dumps(document, ensure_ascii=False, sort_keys=True,
                          separators=(",", ":"), allow_nan=False) + "\n").encode()
    output = Path(output)
    output.parent.mkdir(mode=0o700, parents=False, exist_ok=True)
    if output.parent.is_symlink():
        raise TlsDeploymentError("TLS 基线目录不能是符号链接")
    parent_info = output.parent.stat()
    if os.name == "posix" and (parent_info.st_uid != os.geteuid() or
                               stat.S_IMODE(parent_info.st_mode) & 0o077):
        raise TlsDeploymentError("TLS 基线目录必须由服务用户独占")
    _atomic_write(output, encoded)
    _atomic_write(output.with_suffix(output.suffix + ".sha256"),
                  (_digest(encoded) + "\n").encode("ascii"))
    if os.name == "posix":
        directory_fd = os.open(output.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    return document


def _preflight(path: Path) -> tuple[dict, ssl.SSLContext]:
    path = Path(path)
    _secure_regular(path, private=True)
    digest_path = path.with_suffix(path.suffix + ".sha256")
    _secure_regular(digest_path, private=True)
    raw = path.read_bytes()
    expected = digest_path.read_text(encoding="ascii").strip()
    if len(expected) != 64 or expected != _digest(raw):
        raise TlsDeploymentError("TLS 基线工件摘要不匹配，可能缺失或被篡改")
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise TlsDeploymentError("TLS 基线工件不是有效 JSON") from error
    if not isinstance(document, dict) or set(document) != FIELDS or \
            document.get("schema_version") != SCHEMA_VERSION or \
            document.get("kind") != KIND:
        raise TlsDeploymentError("TLS 基线工件 schema 或字段无效")
    _numeric_loopback(document["runtime_bind"], "runtime_bind")
    _numeric_loopback(document["proxy_bind"], "proxy_bind")
    if document["forwarded_headers"] != "strip_all" or \
            document["client_address_source"] != "direct_peer_only":
        raise TlsDeploymentError("反向代理信任边界必须拒绝所有转发身份头")
    certificate = Path(document["certificate_file"])
    private_key = Path(document["private_key_file"])
    _secure_regular(certificate, private=False)
    _secure_regular(private_key, private=True)
    try:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(certificate), str(private_key))
    except (OSError, ssl.SSLError) as error:
        raise TlsDeploymentError("证书/私钥不可加载或不匹配") from error
    return document, context


def preflight(path: Path) -> dict:
    document, _ = _preflight(path)
    return {"ok": True, "schema_version": SCHEMA_VERSION,
            "runtime_bind": document["runtime_bind"],
            "proxy_bind": document["proxy_bind"],
            "forwarded_headers": "strip_all", "tls_minimum": "1.2"}


def prepare_server_context(path: Path, expected_bind: str) -> ssl.SSLContext:
    """预检并返回已经装载密钥材料的服务端上下文，消除检查后再读漂移。"""
    document, context = _preflight(path)
    if document["proxy_bind"] != _numeric_loopback(expected_bind, "--host"):
        raise TlsDeploymentError("--host 与 TLS 基线 proxy_bind 不一致")
    return context


def main() -> int:
    parser = argparse.ArgumentParser(description="Runtime TLS 部署基线工件")
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create")
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--runtime-bind", default="127.0.0.1")
    create.add_argument("--proxy-bind", default="127.0.0.1")
    create.add_argument("--certificate-file", type=Path, required=True)
    create.add_argument("--private-key-file", type=Path, required=True)
    check = sub.add_parser("preflight")
    check.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = (create_artifact(args.output, runtime_bind=args.runtime_bind,
                                  proxy_bind=args.proxy_bind,
                                  certificate_file=args.certificate_file,
                                  private_key_file=args.private_key_file)
                  if args.command == "create" else preflight(args.config))
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
        return 0
    except TlsDeploymentError as error:
        print(json.dumps({"ok": False, "error": str(error)}, ensure_ascii=False))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

"""Runtime 控制审计的离线链头导出、验证和显式密钥轮换。"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import re
import stat
import time
from pathlib import Path

from .control_audit_journal import ControlAuditError, ControlAuditJournal

ANCHOR_FORMAT = 1
MAXIMUM_TRUSTED_KEYS = 16
_DOMAIN = b"RemoteBSP/control-audit/anchor/v1\0"
_HEX64 = re.compile(r"[0-9a-f]{64}")


class ControlAuditAdminError(RuntimeError):
    """离线审计管理操作不满足安全合同。"""


def _canonical(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode()


def _load_key(path: Path) -> bytes:
    before = os.lstat(path)
    if not stat.S_ISREG(before.st_mode) or before.st_uid != os.geteuid() or \
            before.st_mode & 0o077 or before.st_nlink != 1:
        raise ControlAuditAdminError("审计密钥必须是当前用户独占的0600普通单链接文件")
    flags = os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    try:
        after = os.fstat(fd)
        if (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
            raise ControlAuditAdminError("审计密钥在读取期间被替换")
        key = os.read(fd, 65)
    finally:
        os.close(fd)
    if not 32 <= len(key) <= 64:
        raise ControlAuditAdminError("审计密钥长度必须位于32～64字节")
    return key


def _key_id(key: bytes) -> str:
    return hashlib.sha256(key).hexdigest()[:16]


def _write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise OSError("写入审计管理文件没有取得进展")
        view = view[written:]


def export_anchor(journal_dir: Path, key_file: Path, output: Path, *,
                  label: str = "manual") -> dict[str, object]:
    """离线验证完整 journal 后，以独占创建方式导出链头证据。"""
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", label):
        raise ControlAuditAdminError("锚点标签格式无效")
    key = _load_key(key_file)
    with ControlAuditJournal(journal_dir, key_file) as journal:
        head = journal.chain_head()
    body: dict[str, object] = {
        "format": ANCHOR_FORMAT,
        "label": label,
        "key_id": head.key_id,
        "record_count": head.record_count,
        "last_sequence": head.last_sequence,
        "last_mac": head.last_mac,
        "active_segment": head.active_segment,
        "total_bytes": head.total_bytes,
        "exported_at_ms_untrusted": time.time_ns() // 1_000_000,
        "time_trust": "untrusted_host_clock",
    }
    document = dict(body)
    document["hmac"] = hmac.new(key, _DOMAIN + _canonical(body),
                                hashlib.sha256).hexdigest()
    data = _canonical(document) + b"\n"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | \
        getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(output, flags, 0o600)
    try:
        _write_all(fd, data)
        os.fsync(fd)
    finally:
        os.close(fd)
    return document


def verify_anchor(anchor_file: Path, trusted_key_files: list[Path]) -> dict[str, object]:
    """用最多16把显式信任密钥验证一个外部保存的本地锚点副本。"""
    if not 1 <= len(trusted_key_files) <= MAXIMUM_TRUSTED_KEYS:
        raise ControlAuditAdminError("可信审计密钥数量必须位于1～16")
    keys: dict[str, bytes] = {}
    for path in trusted_key_files:
        key = _load_key(path)
        identifier = _key_id(key)
        if identifier in keys:
            raise ControlAuditAdminError("可信审计密钥重复")
        keys[identifier] = key
    try:
        document = json.loads(anchor_file.read_bytes())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ControlAuditAdminError("链头锚点无法解析") from exc
    expected = {"format", "label", "key_id", "record_count", "last_sequence",
                "last_mac", "active_segment", "total_bytes",
                "exported_at_ms_untrusted", "time_trust", "hmac"}
    if type(document) is not dict or set(document) != expected:
        raise ControlAuditAdminError("链头锚点字段集合无效")
    signature = document.pop("hmac")
    key = keys.get(document.get("key_id"))
    if key is None or not isinstance(signature, str) or \
            _HEX64.fullmatch(signature) is None:
        raise ControlAuditAdminError("链头锚点使用未受信密钥或HMAC格式无效")
    actual = hmac.new(key, _DOMAIN + _canonical(document), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(signature, actual):
        raise ControlAuditAdminError("链头锚点HMAC不匹配")
    if document["format"] != ANCHOR_FORMAT or \
            document["time_trust"] != "untrusted_host_clock" or \
            _HEX64.fullmatch(str(document["last_mac"])) is None or any(
                type(document[name]) is not int or document[name] < 0
                for name in ("record_count", "last_sequence", "active_segment",
                             "total_bytes", "exported_at_ms_untrusted")) or \
            document["record_count"] != document["last_sequence"]:
        raise ControlAuditAdminError("链头锚点边界字段无效")
    document["hmac"] = signature
    return document


def rotate_key(journal_dir: Path, current_key_file: Path, new_key_file: Path,
               archive_root: Path) -> dict[str, object]:
    """停机轮换：封存旧世代、输出双钥交接收据并创建空的新世代。"""
    old_key = _load_key(current_key_file)
    new_key = _load_key(new_key_file)
    if hmac.compare_digest(old_key, new_key):
        raise ControlAuditAdminError("新旧审计密钥不能相同")
    archive_root.mkdir(mode=0o700, parents=False, exist_ok=True)
    status = os.lstat(archive_root)
    if not stat.S_ISDIR(status.st_mode) or status.st_uid != os.geteuid() or \
            status.st_mode & 0o077:
        raise ControlAuditAdminError("轮换归档目录必须归当前用户所有且权限为0700")
    with ControlAuditJournal(journal_dir, current_key_file) as journal:
        health = journal.health_snapshot()
        if health.dangling_intents:
            raise ControlAuditAdminError("存在未决控制意图，拒绝轮换")
        head = journal.chain_head()
    generation = f"generation-{head.last_sequence:016d}-{head.key_id}"
    destination = archive_root / generation
    if destination.exists():
        raise ControlAuditAdminError("目标审计世代归档已经存在")
    new_key_id = _key_id(new_key)
    body: dict[str, object] = {
        "format": 1, "old_generation": generation,
        "old_key_id": head.key_id, "new_key_id": new_key_id,
        "old_last_sequence": head.last_sequence, "old_last_mac": head.last_mac,
        "new_last_sequence": 0, "rotated_at_ms_untrusted": time.time_ns() // 1_000_000,
        "time_trust": "untrusted_host_clock"}
    receipt = dict(body)
    payload = b"RemoteBSP/control-audit/key-rotation/v1\0" + _canonical(body)
    receipt["old_hmac"] = hmac.new(old_key, payload, hashlib.sha256).hexdigest()
    receipt["new_hmac"] = hmac.new(new_key, payload, hashlib.sha256).hexdigest()
    receipt_path = archive_root / f"rotation-{head.key_id}-to-{new_key_id}.json"
    if receipt_path.exists():
        raise ControlAuditAdminError("该新旧密钥组合的轮换收据已经存在")
    pending_receipt = archive_root / f".{receipt_path.name}.pending-{os.getpid()}"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
    fd = os.open(pending_receipt, flags, 0o600)
    try:
        _write_all(fd, _canonical(receipt) + b"\n")
        os.fsync(fd)
    finally:
        os.close(fd)
    try:
        os.replace(journal_dir, destination)
        try:
            with ControlAuditJournal(journal_dir, new_key_file) as fresh:
                if fresh.chain_head().key_id != new_key_id:
                    raise ControlAuditAdminError("新世代密钥标识不一致")
        except Exception:
            os.replace(destination, journal_dir)
            raise
        os.replace(pending_receipt, receipt_path)
        directory_fd = os.open(archive_root, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except Exception:
        try:
            pending_receipt.unlink()
        except OSError:
            pass
        raise
    return receipt


def verify_rotation(receipt_file: Path,
                    trusted_key_files: list[Path]) -> dict[str, object]:
    """要求新旧两把密钥同时受信并验证交接收据。"""
    if not 2 <= len(trusted_key_files) <= MAXIMUM_TRUSTED_KEYS:
        raise ControlAuditAdminError("轮换验证需要2～16把可信审计密钥")
    keys: dict[str, bytes] = {}
    for path in trusted_key_files:
        key = _load_key(path)
        identifier = _key_id(key)
        if identifier in keys:
            raise ControlAuditAdminError("可信审计密钥重复")
        keys[identifier] = key
    try:
        document = json.loads(receipt_file.read_bytes())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ControlAuditAdminError("密钥轮换收据无法解析") from exc
    expected = {"format", "old_generation", "old_key_id", "new_key_id",
                "old_last_sequence", "old_last_mac", "new_last_sequence",
                "rotated_at_ms_untrusted", "time_trust", "old_hmac", "new_hmac"}
    if type(document) is not dict or set(document) != expected:
        raise ControlAuditAdminError("密钥轮换收据字段集合无效")
    old_signature = document.pop("old_hmac")
    new_signature = document.pop("new_hmac")
    old_key = keys.get(document.get("old_key_id"))
    new_key = keys.get(document.get("new_key_id"))
    if old_key is None or new_key is None or old_key is new_key or any(
            not isinstance(value, str) or _HEX64.fullmatch(value) is None
            for value in (old_signature, new_signature)):
        raise ControlAuditAdminError("轮换收据缺少新旧受信密钥或HMAC格式无效")
    payload = b"RemoteBSP/control-audit/key-rotation/v1\0" + _canonical(document)
    if not hmac.compare_digest(old_signature, hmac.new(
            old_key, payload, hashlib.sha256).hexdigest()) or \
            not hmac.compare_digest(new_signature, hmac.new(
                new_key, payload, hashlib.sha256).hexdigest()):
        raise ControlAuditAdminError("密钥轮换收据双向HMAC不匹配")
    if document["format"] != 1 or document["time_trust"] != \
            "untrusted_host_clock" or _HEX64.fullmatch(
                str(document["old_last_mac"])) is None or any(
                    type(document[name]) is not int or document[name] < 0
                    for name in ("old_last_sequence", "new_last_sequence",
                                 "rotated_at_ms_untrusted")) or \
            document["new_last_sequence"] != 0:
        raise ControlAuditAdminError("密钥轮换收据边界字段无效")
    document["old_hmac"] = old_signature
    document["new_hmac"] = new_signature
    return document


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    export = sub.add_parser("anchor-export", help="验证日志并导出本地链头证据")
    export.add_argument("--journal-dir", type=Path, required=True)
    export.add_argument("--key-file", type=Path, required=True)
    export.add_argument("--output", type=Path, required=True)
    export.add_argument("--label", default="manual")
    verify = sub.add_parser("anchor-verify", help="用有界可信密钥集合验证链头证据")
    verify.add_argument("--anchor", type=Path, required=True)
    verify.add_argument("--trusted-key-file", type=Path, action="append", required=True)
    verify_rotation_parser = sub.add_parser(
        "rotation-verify", help="以新旧受信密钥验证轮换交接收据")
    verify_rotation_parser.add_argument("--receipt", type=Path, required=True)
    verify_rotation_parser.add_argument(
        "--trusted-key-file", type=Path, action="append", required=True)
    rotate = sub.add_parser("key-rotate", help="停机封存旧世代并启用新审计密钥")
    rotate.add_argument("--journal-dir", type=Path, required=True)
    rotate.add_argument("--current-key-file", type=Path, required=True)
    rotate.add_argument("--new-key-file", type=Path, required=True)
    rotate.add_argument("--archive-root", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "anchor-export":
            result = export_anchor(args.journal_dir, args.key_file, args.output,
                                   label=args.label)
        elif args.command == "anchor-verify":
            result = verify_anchor(args.anchor, args.trusted_key_file)
        elif args.command == "key-rotate":
            result = rotate_key(args.journal_dir, args.current_key_file,
                                args.new_key_file, args.archive_root)
        else:
            result = verify_rotation(args.receipt, args.trusted_key_file)
    except (ControlAuditError, ControlAuditAdminError, OSError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

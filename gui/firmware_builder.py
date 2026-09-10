#!/usr/bin/env python3
"""RemoteBSP Studio 固件构建与产物归档后端。"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import shutil
import subprocess
import threading
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Sequence

from project_config import (
    ProjectConfigError,
    ProjectConfigResult,
    generate_project_config,
)
from project_contract import canonical_project_bytes, prepare_project


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"
DEFAULT_BUILD_ROOT = FIRMWARE / "build" / "studio"
DEFAULT_OUTPUT_ROOT = FIRMWARE / "out" / "studio"
TOOLCHAIN_FILE = FIRMWARE / "cmake" / "arm-none-eabi-toolchain.cmake"
_BUILD_LOCK = threading.Lock()


class FirmwareBuildError(RuntimeError):
    """Studio 固件构建失败。"""


@dataclass(frozen=True)
class BuildArtifact:
    filename: str
    size: int
    sha256: str


@dataclass(frozen=True)
class FirmwareBuildResult:
    build_id: str
    board_id: str
    firmware_target: str
    config_sha256: str
    output_dir: Path
    artifacts: tuple[BuildArtifact, ...]
    record: dict


TARGET_ARTIFACTS = {
    "mellow-fly-d5": "remotebsp-stm32f072rbt6",
    "weact-bluepill-plus": "remotebsp-stm32f103cbt6",
    "weact-stm32g431cbu6-core": "remotebsp-stm32g431cbu6",
}
_MEMORY_LINE = re.compile(
    r"^\s*(RAM|FLASH):\s+(\d+)\s+(B|KB)\s+(\d+)\s+(B|KB)\s+([0-9.]+)%",
    re.MULTILINE)
_FIRMWARE_SOURCE_ROOTS = (
    "firmware", "firmware/cmake", "cmake", "protocol", "device_params")
_MAX_BUILD_RECORD_BYTES = 128 * 1024
_VENDOR_SOURCE_PATHS = {
    "cmsis-core": ("Core/Include/",),
    "cmsis-device-f0": ("Include/",),
    "cmsis-device-f1": ("Include/",),
    "cmsis-device-g4": ("Include/",),
    "stm32f0xx-hal-driver": ("Inc/", "Src/"),
    "stm32f1xx-hal-driver": ("Inc/", "Src/"),
    "stm32g4xx-hal-driver": ("Inc/", "Src/"),
    "stm32-mw-usb-device": ("Core/",),
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _memory_report(output: str) -> dict[str, dict[str, int | float]]:
    def to_bytes(value: str, unit: str) -> int:
        return int(value) * (1024 if unit == "KB" else 1)

    report: dict[str, dict[str, int | float]] = {}
    for region, used, used_unit, capacity, capacity_unit, percent in \
            _MEMORY_LINE.findall(output):
        report[region.lower()] = {
            "used_bytes": to_bytes(used, used_unit),
            "capacity_bytes": to_bytes(capacity, capacity_unit),
            "used_percent": float(percent),
        }
    return report


def _default_runner(command: Sequence[str], timeout: int) -> str:
    try:
        completed = subprocess.run(
            list(command), cwd=ROOT, check=False, capture_output=True,
            text=True, encoding="utf-8", errors="replace", timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise FirmwareBuildError(f"启动构建工具失败：{error}") from error
    output = (completed.stdout or "") + (completed.stderr or "")
    if completed.returncode != 0:
        tail = output[-12000:]
        raise FirmwareBuildError(
            f"构建命令失败（退出码{completed.returncode}）：\n{tail}")
    return output


def _git_revision() -> tuple[str, bool]:
    try:
        revision = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True,
            capture_output=True, text=True, timeout=5).stdout.strip()
        dirty = bool(subprocess.run(
            ["git", "status", "--porcelain"], cwd=ROOT, check=True,
            capture_output=True, text=True, timeout=5).stdout.strip())
        return revision, dirty
    except (OSError, subprocess.SubprocessError):
        return "unknown", True


def _tool_versions() -> dict[str, str]:
    versions: dict[str, str] = {}
    for name, command in (
            ("cmake", ["cmake", "--version"]),
            ("ninja", ["ninja", "--version"]),
            ("arm_none_eabi_gcc", ["arm-none-eabi-gcc", "--version"])):
        try:
            output = subprocess.run(
                command, cwd=ROOT, check=True, capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=5).stdout
            versions[name] = output.splitlines()[0].strip()
        except (OSError, subprocess.SubprocessError, IndexError):
            versions[name] = "unknown"
    return versions


def _vendor_dependency_identity(dependency: Path) -> bytes:
    """生成依赖内容身份；不依赖 NTFS 上不稳定的 Git stat 缓存。"""
    try:
        revision = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=dependency, check=True,
            capture_output=True, timeout=5).stdout.strip()
        staged = subprocess.run(
            ["git", "diff", "--cached", "--raw", "-z", "HEAD", "--"],
            cwd=dependency, check=True, capture_output=True,
            timeout=30).stdout
        untracked = subprocess.run(
            ["git", "ls-files", "-z", "--others", "--exclude-standard"],
            cwd=dependency, check=True, capture_output=True,
            timeout=30).stdout
        indexed = subprocess.run(
            ["git", "ls-files", "-z", "--stage"], cwd=dependency,
            check=True, capture_output=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError) as error:
        raise FirmwareBuildError(
            f"无法识别固件依赖版本：{dependency.name}") from error

    prefixes = _VENDOR_SOURCE_PATHS.get(dependency.name, ())
    identity = hashlib.sha256(b"RemoteBSP-vendor-source-v1\0")
    identity.update(revision)
    identity.update(b"\0index\0")
    identity.update(staged)

    def include(name: str) -> bool:
        return any(name.startswith(prefix) for prefix in prefixes) and \
            name.lower().endswith((".c", ".h", ".s", ".ld", ".inc"))

    def add_file(raw_name: bytes, label: bytes) -> None:
        name = raw_name.decode("utf-8", errors="surrogateescape")
        if not include(name):
            return
        path = dependency / name
        try:
            resolved = path.resolve(strict=True)
            if path.is_symlink() or dependency.resolve() not in \
                    resolved.parents or not path.is_file():
                raise FirmwareBuildError(
                    f"固件依赖源码不是普通文件：{dependency.name}/{name}")
            content = path.read_bytes()
        except OSError as error:
            raise FirmwareBuildError(
                f"无法读取固件依赖源码：{dependency.name}/{name}") from error
        identity.update(label)
        identity.update(len(raw_name).to_bytes(4, "big"))
        identity.update(raw_name)
        identity.update(len(content).to_bytes(8, "big"))
        identity.update(content)

    for entry in (item for item in indexed.split(b"\0") if item):
        try:
            metadata, raw_name = entry.split(b"\t", 1)
            mode, _expected_oid, stage = metadata.split(b" ", 2)
        except (ValueError, UnicodeError) as error:
            raise FirmwareBuildError(
                f"固件依赖索引格式无效：{dependency.name}") from error
        if stage != b"0":
            raise FirmwareBuildError(
                f"固件依赖索引存在未合并项：{dependency.name}")
        if mode not in (b"100644", b"100755") and include(
                raw_name.decode("utf-8", errors="surrogateescape")):
            raise FirmwareBuildError(
                f"固件依赖源码类型无效：{dependency.name}")
        add_file(raw_name, b"\0tracked\0")
    for raw_name in (item for item in untracked.split(b"\0") if item):
        add_file(raw_name, b"\0untracked\0")
    return revision + b":" + identity.hexdigest().encode("ascii")


def _firmware_source_sha256() -> str:
    """哈希仓库内固件输入，并纳入嵌套供应商提交版本。"""
    try:
        listed = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others",
             "--exclude-standard", "--", *_FIRMWARE_SOURCE_ROOTS],
            cwd=ROOT, check=True, capture_output=True, timeout=15).stdout
    except (OSError, subprocess.SubprocessError) as error:
        raise FirmwareBuildError("无法枚举固件源码，不能生成可审计构建ID") from error
    digest = hashlib.sha256(b"RemoteBSP-firmware-source-v1\0")
    root = ROOT.resolve()
    for raw_name in sorted(name for name in listed.split(b"\0") if name):
        name = raw_name.decode("utf-8", errors="surrogateescape")
        path = ROOT / name
        try:
            resolved = path.resolve(strict=True)
            if path.is_symlink() or root not in resolved.parents or \
                    not path.is_file():
                raise FirmwareBuildError(f"固件源码不是仓库内普通文件：{name}")
            digest.update(len(raw_name).to_bytes(4, "big"))
            digest.update(raw_name)
            digest.update(path.stat().st_size.to_bytes(8, "big"))
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(65536), b""):
                    digest.update(chunk)
        except OSError as error:
            raise FirmwareBuildError(f"无法读取固件源码：{name}") from error

    vendor_root = FIRMWARE / "vendor"
    if vendor_root.is_dir():
        for dependency in sorted(vendor_root.iterdir(), key=lambda item: item.name):
            if dependency.name not in _VENDOR_SOURCE_PATHS or \
                    not (dependency / ".git").exists():
                continue
            revision = _vendor_dependency_identity(dependency)
            name = dependency.name.encode("utf-8")
            digest.update(len(name).to_bytes(4, "big"))
            digest.update(name)
            digest.update(revision)
    return digest.hexdigest()


def _safe_build_id(board_id: str, config_sha256: str) -> str:
    board = re.sub(r"[^a-z0-9-]", "-", board_id.lower()).strip("-")
    return f"{board}-{config_sha256[:16]}"


def build_firmware_project(
        project: dict, catalog: dict, *, jobs: int = 32,
        build_root: Path = DEFAULT_BUILD_ROOT,
        output_root: Path = DEFAULT_OUTPUT_ROOT,
        runner: Callable[[Sequence[str], int], str] = _default_runner,
        timeout: int = 300) -> FirmwareBuildResult:
    """校验工程、构建固件，并归档可复现产物。"""
    if jobs < 1 or jobs > 64:
        raise FirmwareBuildError("并行任务数必须位于1～64")
    prepared = prepare_project(project)
    generated: ProjectConfigResult = generate_project_config(
        prepared.document, catalog)
    artifact_base = TARGET_ARTIFACTS.get(generated.firmware_target)
    if artifact_base is None:
        raise FirmwareBuildError("生成结果没有对应的固件构建目标")

    config_bytes = generated.config.encode("utf-8")
    resource_bytes = generated.static_resource_header.encode("utf-8")
    config_sha256 = hashlib.sha256(config_bytes).hexdigest()
    revision, dirty = _git_revision()
    source_sha256 = _firmware_source_sha256()
    tool_versions = _tool_versions()
    firmware_input_sha256 = hashlib.sha256(
        config_bytes + b"\0" + resource_bytes + b"\0" +
        source_sha256.encode("ascii") + b"\0" + revision.encode("ascii") +
        b"\0" + json.dumps(tool_versions, sort_keys=True,
                             separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    build_id = _safe_build_id(generated.board_id, firmware_input_sha256)
    build_dir = build_root / build_id
    output_dir = output_root / build_id
    config_path = output_dir / "firmware.config"
    resource_path = output_dir / "remotebsp_static_resources.h"
    project_path = output_dir / "studio-project.json"

    with _BUILD_LOCK:
        build_dir.mkdir(parents=True, exist_ok=True)
        output_dir.mkdir(parents=True, exist_ok=True)
        config_path.write_bytes(config_bytes)
        resource_path.write_bytes(resource_bytes)
        project_path.write_bytes(canonical_project_bytes(prepared.document))

        configure = [
            "cmake", "-S", str(FIRMWARE), "-B", str(build_dir), "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN_FILE}",
            f"-DRBSP_CONFIG={config_path}",
            f"-DRBSP_STATIC_RESOURCE_TABLE={resource_path}",
            f"-DRBSP_FIRMWARE_INPUT_SHA256={firmware_input_sha256}",
        ]
        compile_command = [
            "cmake", "--build", str(build_dir), "--parallel", str(jobs),
        ]
        configure_log = runner(configure, timeout)
        build_log = runner(compile_command, timeout)
        final_revision, final_dirty = _git_revision()
        if _firmware_source_sha256() != source_sha256 or \
                final_revision != revision or final_dirty != dirty:
            raise FirmwareBuildError("构建期间源码或Git状态发生变化，产物未归档")
        log_path = output_dir / "build.log"
        log_path.write_text(
            "$ " + " ".join(configure) + "\n" + configure_log + "\n" +
            "$ " + " ".join(compile_command) + "\n" + build_log,
            encoding="utf-8")

        for suffix in ("elf", "bin", "hex", "map"):
            source = build_dir / f"{artifact_base}.{suffix}"
            if not source.is_file():
                raise FirmwareBuildError(f"构建成功但缺少产物：{source.name}")
            shutil.copy2(source, output_dir / f"firmware.{suffix}")

        artifact_paths = [project_path, config_path, resource_path, log_path] + [
            output_dir / f"firmware.{suffix}"
            for suffix in ("elf", "bin", "hex", "map")
        ]
        artifacts = tuple(BuildArtifact(
            path.name, path.stat().st_size, _sha256(path))
            for path in artifact_paths)
        record = {
            "schema_version": 1,
            "build_id": build_id,
            "built_at_utc": datetime.now(timezone.utc).isoformat(),
            "board_id": generated.board_id,
            "firmware_target": generated.firmware_target,
            "resource_count": generated.resource_count,
            "project_schema_version": prepared.schema_version,
            "project_original_schema_version":
                prepared.original_schema_version,
            "project_migrations": list(prepared.migrations),
            "project_summary": prepared.summary,
            "project_sha256": prepared.sha256,
            "config_sha256": config_sha256,
            "static_resource_sha256": generated.static_resource_sha256,
            "firmware_source_sha256": source_sha256,
            "firmware_input_sha256": firmware_input_sha256,
            "git_revision": revision,
            "git_dirty": dirty,
            "parallel_jobs": jobs,
            "tool_versions": tool_versions,
            "memory": _memory_report(build_log),
            "artifacts": [artifact.__dict__ for artifact in artifacts],
        }
        record_path = output_dir / "build-record.json"
        record_path.write_text(
            json.dumps(record, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8")
        record_artifact = BuildArtifact(
            record_path.name, record_path.stat().st_size, _sha256(record_path))
        artifacts += (record_artifact,)

    return FirmwareBuildResult(
        build_id, generated.board_id, generated.firmware_target,
        config_sha256, output_dir, artifacts, record)


def resolve_artifact(build_id: str, filename: str,
                     output_root: Path = DEFAULT_OUTPUT_ROOT) -> Path:
    """只解析构建记录列出且大小、哈希一致的普通文件。"""
    if not re.fullmatch(r"[a-z0-9-]{8,96}", build_id):
        raise FirmwareBuildError("构建ID无效")
    if Path(filename).name != filename or not filename:
        raise FirmwareBuildError("产物名称无效")
    directory = output_root / build_id
    record_path = directory / "build-record.json"
    try:
        if record_path.is_symlink() or not record_path.is_file() or \
                record_path.resolve(strict=True).parent != directory.resolve():
            raise FirmwareBuildError("构建记录不是目录内普通文件")
        if record_path.stat().st_size > _MAX_BUILD_RECORD_BYTES:
            raise FirmwareBuildError("构建记录超过128 KiB上限")
        record = json.loads(record_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FirmwareBuildError("构建记录不存在或已经损坏") from error
    if not isinstance(record, dict):
        raise FirmwareBuildError("构建记录必须是JSON对象")
    if record.get("build_id") != build_id:
        raise FirmwareBuildError("构建记录与目录ID不一致")
    if filename == "build-record.json":
        return record_path
    entries = record.get("artifacts")
    if not isinstance(entries, list):
        raise FirmwareBuildError("构建记录产物清单无效")
    matches = [item for item in entries
               if isinstance(item, dict) and item.get("filename") == filename]
    if len(matches) != 1:
        raise FirmwareBuildError("产物不在构建记录中")
    path = directory / filename
    if path.is_symlink() or not path.is_file() or \
            path.resolve(strict=True).parent != directory.resolve():
        raise FirmwareBuildError("产物文件不存在")
    expected_size = matches[0].get("size")
    expected_sha256 = matches[0].get("sha256")
    if isinstance(expected_size, bool) or not isinstance(expected_size, int) \
            or expected_size < 0 or not isinstance(expected_sha256, str) \
            or not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise FirmwareBuildError("构建记录产物校验信息无效")
    if path.stat().st_size != expected_size or _sha256(path) != expected_sha256:
        raise FirmwareBuildError("产物大小或SHA-256与构建记录不一致")
    return path


def _default_project(board: dict) -> dict:
    return {
        "schema_version": 2,
        "board_id": board["id"],
        "gpio": {"resources": copy.deepcopy(board.get("gpio_defaults", []))},
        "uart": {"ports": copy.deepcopy(board.get("uart_defaults", []))},
        "motion": {"axes": copy.deepcopy(board.get("motion_defaults", []))},
        "pwm": {"channels": copy.deepcopy([
            item for item in board.get("waveform", {}).get("pwm", [])
            if item.get("enabled")
        ])},
        "timed_bitstream": {"ws2812": copy.deepcopy([
            item for item in board.get("waveform", {}).get("ws2812", [])
            if item.get("enabled")
        ])},
        "i2c": {"buses": [], "devices": []},
        "spi": {"buses": [], "devices": []},
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="构建并归档RemoteBSP Studio板卡专用固件")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--project", type=Path, help="Studio工程JSON")
    source.add_argument("--all-board-defaults", action="store_true",
                        help="构建目录中三块板卡的默认工程")
    parser.add_argument(
        "--catalog", type=Path,
        default=Path(__file__).parent / "data" / "pin_catalog.json",
        help="板卡能力目录")
    parser.add_argument("--jobs", type=int, default=32,
                        help="并行任务数，默认32")
    args = parser.parse_args()
    try:
        catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
        if args.all_board_defaults:
            projects = [_default_project(board)
                        for board in catalog.get("boards", [])]
        else:
            projects = [json.loads(args.project.read_text(encoding="utf-8"))]
        for project in projects:
            result = build_firmware_project(
                project, catalog, jobs=args.jobs)
            print(f"{result.board_id}：构建完成，产物目录 {result.output_dir}")
    except (OSError, json.JSONDecodeError, ProjectConfigError,
            FirmwareBuildError, TypeError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

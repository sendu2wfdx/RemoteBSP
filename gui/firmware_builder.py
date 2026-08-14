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
    generated: ProjectConfigResult = generate_project_config(project, catalog)
    artifact_base = TARGET_ARTIFACTS.get(generated.firmware_target)
    if artifact_base is None:
        raise FirmwareBuildError("生成结果没有对应的固件构建目标")

    config_bytes = generated.config.encode("utf-8")
    config_sha256 = hashlib.sha256(config_bytes).hexdigest()
    build_id = _safe_build_id(generated.board_id, config_sha256)
    build_dir = build_root / build_id
    output_dir = output_root / build_id
    config_path = output_dir / "firmware.config"
    project_path = output_dir / "studio-project.json"

    with _BUILD_LOCK:
        build_dir.mkdir(parents=True, exist_ok=True)
        output_dir.mkdir(parents=True, exist_ok=True)
        config_path.write_bytes(config_bytes)
        project_path.write_text(
            json.dumps(project, ensure_ascii=False, sort_keys=True, indent=2) +
            "\n", encoding="utf-8")

        configure = [
            "cmake", "-S", str(FIRMWARE), "-B", str(build_dir), "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN_FILE}",
            f"-DRBSP_CONFIG={config_path}",
        ]
        compile_command = [
            "cmake", "--build", str(build_dir), "--parallel", str(jobs),
        ]
        configure_log = runner(configure, timeout)
        build_log = runner(compile_command, timeout)
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

        artifact_paths = [project_path, config_path, log_path] + [
            output_dir / f"firmware.{suffix}"
            for suffix in ("elf", "bin", "hex", "map")
        ]
        artifacts = tuple(BuildArtifact(
            path.name, path.stat().st_size, _sha256(path))
            for path in artifact_paths)
        revision, dirty = _git_revision()
        record = {
            "schema_version": 1,
            "build_id": build_id,
            "built_at_utc": datetime.now(timezone.utc).isoformat(),
            "board_id": generated.board_id,
            "firmware_target": generated.firmware_target,
            "resource_count": generated.resource_count,
            "project_sha256": _sha256(project_path),
            "config_sha256": config_sha256,
            "git_revision": revision,
            "git_dirty": dirty,
            "parallel_jobs": jobs,
            "tool_versions": _tool_versions(),
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
    """只解析构建记录列出的普通文件名，拒绝目录穿越。"""
    if not re.fullmatch(r"[a-z0-9-]{8,96}", build_id):
        raise FirmwareBuildError("构建ID无效")
    if Path(filename).name != filename or not filename:
        raise FirmwareBuildError("产物名称无效")
    directory = output_root / build_id
    record_path = directory / "build-record.json"
    try:
        record = json.loads(record_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FirmwareBuildError("构建记录不存在或已经损坏") from error
    allowed = {item.get("filename") for item in record.get("artifacts", [])}
    allowed.add("build-record.json")
    if filename not in allowed:
        raise FirmwareBuildError("产物不在构建记录中")
    path = directory / filename
    if not path.is_file():
        raise FirmwareBuildError("产物文件不存在")
    return path


def _default_project(board: dict) -> dict:
    return {
        "schema_version": 1,
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

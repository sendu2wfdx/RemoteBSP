#!/usr/bin/env python3
"""校验 RemoteBSP 固件声明的 Flash 布局及 ELF 实际装载区间。"""

from __future__ import annotations

import argparse
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path


FLASH_BASE = 0x08000000


@dataclass(frozen=True)
class Board:
    key: str
    flash_size: int
    page_size: int
    boot_config: str


BOARDS = {
    "CONFIG_BOARD_STM32F072RBT6": Board(
        "f072", 128 * 1024, 2 * 1024,
        "katapult_stm32f072_mellow_fly_d5_dual.config"),
    "CONFIG_BOARD_STM32F103CBT6": Board(
        "f103", 128 * 1024, 1024,
        "katapult_stm32f103_weact_bluepill_plus_dual.config"),
    "CONFIG_BOARD_STM32G431CBU6": Board(
        "g431", 128 * 1024, 2 * 1024,
        "katapult_stm32g431_weact_core_dual.config"),
}


def fail(message: str) -> None:
    raise SystemExit(f"Flash 布局门禁失败：{message}")


def read_config(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.+)", line.strip())
        if match:
            result[match.group(1)] = match.group(2)
            continue
        match = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line.strip())
        if match:
            result[match.group(1)] = "n"
    return result


def parse_int(value: str, field: str) -> int:
    try:
        return int(value, 0)
    except ValueError:
        fail(f"{field} 不是合法整数：{value}")


def map_symbols(path: Path) -> dict[str, int]:
    wanted = {
        "__rbsp_health_epoch_flash_start__",
        "__rbsp_health_epoch_flash_end__",
        "__rbsp_device_param_flash_start__",
        "__rbsp_device_param_flash_end__",
        "__rbsp_motion_epoch_flash_start__",
        "__rbsp_motion_epoch_flash_end__",
    }
    values: dict[str, int] = {}
    pattern = re.compile(r"^\s*(0x[0-9a-fA-F]+)\s+.*\b(" +
                         "|".join(re.escape(x) for x in wanted) + r")\b")
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pattern.search(line)
        if match:
            values[match.group(2)] = int(match.group(1), 16)
    missing = wanted - values.keys()
    if missing:
        fail(f"链接 map 缺少布局符号：{', '.join(sorted(missing))}")
    return values


def loaded_flash_sections(elf: Path) -> list[tuple[str, int, int]]:
    try:
        output = subprocess.check_output(
            ["arm-none-eabi-objdump", "-h", str(elf)], text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        fail(f"无法读取 ELF 段表：{exc}")
    lines = output.splitlines()
    sections: list[tuple[str, int, int]] = []
    row = re.compile(
        r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+"
        r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)")
    for index, line in enumerate(lines[:-1]):
        match = row.match(line)
        if not match:
            continue
        name, size_hex, _vma_hex, lma_hex = match.groups()
        flags = lines[index + 1]
        if "ALLOC" not in flags or "LOAD" not in flags:
            continue
        start = int(lma_hex, 16)
        size = int(size_hex, 16)
        if size and FLASH_BASE <= start < FLASH_BASE + 128 * 1024:
            sections.append((name, start, start + size))
    if not sections:
        fail("ELF 中没有可识别的 Flash 装载段")
    return sections


def check_interval(name: str, start: int, end: int, page: int) -> None:
    if start > end:
        fail(f"{name} 区间反向")
    if start % page or end % page:
        fail(f"{name} 未按 {page} 字节擦除页对齐")


def verify(config_path: Path, elf: Path, map_path: Path,
           bootloader_dir: Path) -> None:
    config = read_config(config_path)
    selected = [board for symbol, board in BOARDS.items()
                if config.get(symbol) == "y"]
    if len(selected) != 1:
        fail("配置必须且只能选择一款受支持的 STM32")
    board = selected[0]
    flash_end = FLASH_BASE + board.flash_size
    app_offset = parse_int(config.get("CONFIG_APPLICATION_FLASH_OFFSET", ""),
                           "CONFIG_APPLICATION_FLASH_OFFSET")
    app_start = FLASH_BASE + app_offset
    # 该选项在 Kconfig 中默认启用，defconfig 可只记录显式关闭项。
    params_enabled = config.get("CONFIG_REMOTEBSP_DEVICE_PARAMS", "y") == "y"
    param_size = 2 * board.page_size if params_enabled else 0
    health_size = 2 * board.page_size
    motion_size = 2 * board.page_size
    health_start = flash_end - param_size - health_size
    motion_start = health_start - motion_size
    param_start = flash_end - param_size

    regions = [
        ("Bootloader", FLASH_BASE, app_start),
        ("应用", app_start, motion_start),
        ("motion epoch 双页", motion_start, health_start),
        ("health epoch 双页", health_start, param_start),
        ("device params 双页", param_start, flash_end),
    ]
    for name, start, end in regions:
        check_interval(name, start, end, board.page_size)
    for left, right in zip(regions, regions[1:]):
        if left[2] != right[1]:
            fail(f"{left[0]} 与 {right[0]} 边界不连续")
    if health_size != 2 * board.page_size:
        fail("health epoch 未保留两个擦除页")
    if params_enabled and param_size != 2 * board.page_size:
        fail("device params 未保留两个擦除页")
    if app_start >= health_start:
        fail("应用 Flash 区间为空或越过持久化区域")

    expected_symbols = {
        "__rbsp_health_epoch_flash_start__": health_start,
        "__rbsp_health_epoch_flash_end__": param_start,
        "__rbsp_device_param_flash_start__": param_start,
        "__rbsp_device_param_flash_end__": flash_end,
        "__rbsp_motion_epoch_flash_start__": motion_start,
        "__rbsp_motion_epoch_flash_end__": health_start,
    }
    actual_symbols = map_symbols(map_path)
    for name, expected in expected_symbols.items():
        if actual_symbols[name] != expected:
            fail(f"{name} 为 0x{actual_symbols[name]:08x}，预期 0x{expected:08x}")

    sections = loaded_flash_sections(elf)
    for name, start, end in sections:
        if start < app_start or end > motion_start:
            fail(f"ELF 段 {name} [0x{start:08x}, 0x{end:08x}) 越过应用区间")
    vector = [section for section in sections if section[0] == ".isr_vector"]
    if len(vector) != 1 or vector[0][1] != app_start:
        fail("ELF 中断向量表未从应用 Flash 起始地址装载")

    if app_offset:
        boot_config_path = bootloader_dir / "configs" / board.boot_config
        boot_config = read_config(boot_config_path)
        boot_end = parse_int(
            boot_config.get("CONFIG_FLASH_APPLICATION_END_ADDRESS", ""),
            "CONFIG_FLASH_APPLICATION_END_ADDRESS")
        if boot_end != motion_start:
            fail(f"Katapult 写入上限 0x{boot_end:08x} 未停在 motion epoch 前 "
                 f"0x{motion_start:08x}")

    print(f"Flash 布局通过：{board.key}，应用 [0x{app_start:08x}, "
          f"0x{motion_start:08x})，motion [0x{motion_start:08x}, "
          f"0x{health_start:08x})，health [0x{health_start:08x}, "
          f"0x{param_start:08x})，参数 [0x{param_start:08x}, "
          f"0x{flash_end:08x})")


def main() -> int:
    parser = argparse.ArgumentParser(description="校验 STM32 固件 Flash 链接布局")
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--map", dest="map_path", type=Path, required=True)
    parser.add_argument("--bootloader-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "bootloader")
    args = parser.parse_args()
    for path in (args.config, args.elf, args.map_path):
        if not path.is_file():
            fail(f"文件不存在：{path}")
    verify(args.config.resolve(), args.elf.resolve(), args.map_path.resolve(),
           args.bootloader_dir.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""校验 CI 固件矩阵的裁剪、身份注入、产物和内存预算。"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    raise SystemExit(f"固件矩阵门禁失败：{message}")


def sizes(elf: Path) -> tuple[int, int]:
    output = subprocess.check_output(
        ["arm-none-eabi-size", str(elf)], text=True).splitlines()
    if len(output) != 2:
        fail(f"无法解析尺寸：{elf}")
    columns = output[1].split()
    return int(columns[0]) + int(columns[1]), int(columns[1]) + int(columns[2])


def symbols(elf: Path) -> str:
    return subprocess.check_output(
        ["arm-none-eabi-nm", "--defined-only", str(elf)], text=True)


def check_build(name: str, artifact: str, flash_limit: int,
                ram_limit: int) -> Path:
    directory = ROOT / "build" / name
    elf = directory / f"{artifact}.elf"
    for suffix in ("elf", "bin", "hex", "map"):
        if not (directory / f"{artifact}.{suffix}").is_file():
            fail(f"{name} 缺少 {suffix} 产物")
    flash, ram = sizes(elf)
    if flash > flash_limit:
        fail(f"{name} Flash {flash} 超过预算 {flash_limit}")
    if ram > ram_limit:
        fail(f"{name} RAM {ram} 超过预算 {ram_limit}")
    print(f"{name}: Flash {flash}/{flash_limit}, RAM {ram}/{ram_limit}")
    return elf


def main() -> int:
    artifact = "remotebsp-stm32g431cbu6"
    base = check_build("g431-weact-core", artifact, 56_000, 29_000)
    bus = check_build("g431-weact-core-bus-hal", artifact, 57_000, 21_000)
    # Studio 默认工程同时覆盖 GPIO/UART/PWM/WS2812/运动资源，预算高于基础预设。
    studio = check_build("g431-weact-core-studio-identity", artifact,
                         78_000, 29_000)

    base_symbols = symbols(base)
    bus_symbols = symbols(bus)
    for symbol in ("rbsp_g431_bus_init", "HAL_I2C_Init", "HAL_SPI_Init"):
        if symbol in base_symbols:
            fail(f"BUS 关闭后仍链接 {symbol}")
        if symbol not in bus_symbols:
            fail(f"BUS 专用固件未链接 {symbol}")

    fixture = ROOT / "build" / "ci-studio-input" / "identity.json"
    identity = json.loads(fixture.read_text(encoding="utf-8"))
    studio_bytes = studio.read_bytes()
    for field in ("project_sha256", "config_sha256",
                  "firmware_input_sha256"):
        value = identity[field]
        if value.encode("ascii") not in studio_bytes:
            fail(f"Studio 固件未嵌入 {field}")

    cache = (ROOT / "build" / "g431-weact-core-studio-identity" /
             "CMakeCache.txt").read_text(encoding="utf-8")
    expected_config = str(
        (ROOT / "build" / "ci-studio-input" / "firmware.config").resolve())
    expected_resources = str((
        ROOT / "build" / "ci-studio-input" /
        "remotebsp_static_resources.h").resolve())
    if f"RBSP_CONFIG:FILEPATH={expected_config}" not in cache:
        fail("Studio 构建使用的配置路径发生漂移")
    if f"RBSP_STATIC_RESOURCE_TABLE:FILEPATH={expected_resources}" not in cache:
        fail("Studio 静态资源表路径发生漂移")
    if (f"RBSP_FIRMWARE_INPUT_SHA256:STRING="
            f"{identity['firmware_input_sha256']}") not in cache:
        fail("Studio 固件输入身份发生漂移")
    print("可选裁剪、Studio 身份注入、配置路径和产物矩阵校验通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

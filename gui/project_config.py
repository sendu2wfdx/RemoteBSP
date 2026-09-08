#!/usr/bin/env python3
"""把 Studio 工程转换为可直接构建的固件 `.config`。"""

from __future__ import annotations

import os
import threading
import argparse
import json
from dataclasses import dataclass
from pathlib import Path

import kconfiglib

from project_contract import prepare_project


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"
_KCONFIG_LOCK = threading.Lock()


class ProjectConfigError(ValueError):
    """Studio 工程无法转换为静态固件配置。"""


@dataclass(frozen=True)
class ProjectConfigResult:
    config: str
    board_id: str
    firmware_target: str
    resource_count: int
    project_sha256: str
    project_schema_version: int
    original_schema_version: int
    migrations: tuple[str, ...]
    summary: dict


BOARD_CONFIGS = {
    "mellow-fly-d5-v1": (
        "configs/stm32f072_mellow_fly_d5_defconfig",
        "mellow-fly-d5",
    ),
    "weact-bluepill-plus-v1": (
        "configs/stm32f103_weact_bluepill_plus_defconfig",
        "weact-bluepill-plus",
    ),
    "weact-g431-core-v10": (
        "configs/stm32g431_weact_core_defconfig",
        "weact-stm32g431cbu6-core",
    ),
}


def _items(draft: dict, group: str, key: str) -> list[dict]:
    value = draft.get(group, {}).get(key, [])
    if not isinstance(value, list) or not all(isinstance(item, dict)
                                               for item in value):
        raise ProjectConfigError(f"{group}.{key}必须是对象数组")
    return value


def _find(items: list[dict], key: str, value: object) -> dict | None:
    return next((item for item in items if item.get(key) == value), None)


def _pin_symbol(pin: object) -> str:
    if not isinstance(pin, str) or len(pin) not in (3, 4) or \
            not pin.startswith("P") or not pin[1].isalpha() or \
            not pin[2:].isdigit() or int(pin[2:]) > 15:
        raise ProjectConfigError(f"引脚名称无效：{pin}")
    return pin.upper()


def _validate_and_collect(draft: dict, catalog: dict) -> tuple[dict, dict]:
    if not isinstance(draft, dict) or draft.get("schema_version") != 1:
        raise ProjectConfigError("工程schema_version必须为1")
    board = _find(catalog.get("boards", []), "id", draft.get("board_id"))
    if board is None or board["id"] not in BOARD_CONFIGS:
        raise ProjectConfigError("工程引用了未知或不支持生成固件的板卡")

    gpio = _items(draft, "gpio", "resources")
    uart = _items(draft, "uart", "ports")
    axes = _items(draft, "motion", "axes")
    pwm = _items(draft, "pwm", "channels")
    strips = _items(draft, "timed_bitstream", "ws2812")
    implemented_uart_ports = {
        int(item["port"])
        for item in board.get("uart", {}).get("endpoints", [])
        if item.get("backend_status") == "implemented"
    }
    if len(uart) > len(implemented_uart_ports) or \
            len(pwm) > 1 or len(strips) > 1:
        raise ProjectConfigError(
            f"当前板卡最多支持{len(implemented_uart_ports)}路硬件UART、"
            "1路PWM和1路WS2812")
    if len(axes) > 5:
        raise ProjectConfigError("当前单板静态固件最多支持5个运动轴")

    board_pins = set(board.get("pins", []))
    reserved = {item["pin"] for item in board.get("reserved", [])}
    used: dict[str, str] = {}

    def claim(pin_value: object, owner: str, allow_reserved: bool = False) -> str:
        pin = _pin_symbol(pin_value)
        if pin not in board_pins:
            raise ProjectConfigError(f"{owner}使用了板卡未公开的引脚{pin}")
        if pin in reserved and not allow_reserved:
            raise ProjectConfigError(f"{owner}使用了板级保留引脚{pin}")
        if pin in used:
            raise ProjectConfigError(f"{owner}与{used[pin]}重复使用{pin}")
        used[pin] = owner
        return pin

    interfaces = {item["pin"]: item for item in board.get(
        "gpio_interfaces", [])}
    for index, item in enumerate(gpio):
        owner = f"GPIO {index + 1}"
        pin = claim(item.get("pin"), owner)
        interface = interfaces.get(pin)
        if interface is None:
            raise ProjectConfigError(f"{owner}不是板卡允许配置的数字IO")
        if item.get("direction") not in interface.get("allowed_directions", []):
            raise ProjectConfigError(f"{owner}方向不符合板级电路约束")
        if item.get("pull", "none") not in interface.get("allowed_pulls", []):
            raise ProjectConfigError(f"{owner}上下拉不符合板级电路约束")

    uart_catalog = board.get("uart", {}).get("endpoints", [])
    for index, item in enumerate(uart):
        endpoint = _find(uart_catalog, "endpoint_id", item.get("endpoint_id"))
        if endpoint is None or endpoint.get("backend_status") != "implemented":
            raise ProjectConfigError(f"UART {index + 1}硬件端点尚未实现")
        baud = int(item.get("baud_rate", 0))
        if baud < int(endpoint.get("minimum_baud_rate", 1)) or \
                baud > int(endpoint.get("maximum_baud_rate", 0)):
            raise ProjectConfigError(f"UART {index + 1}波特率超出端点范围")
        if item.get("direction_pin"):
            raise ProjectConfigError("普通UART的RS-485方向控制后端尚未实现")
        claim(endpoint["rx_pin"], f"UART {index + 1} RX", True)
        claim(endpoint["tx_pin"], f"UART {index + 1} TX", True)

    pwm_catalog = board.get("waveform", {}).get("pwm", [])
    for index, item in enumerate(pwm):
        endpoint = _find(pwm_catalog, "endpoint_id", item.get("endpoint_id"))
        if endpoint is None or endpoint.get("backend_status") != "implemented":
            raise ProjectConfigError(f"PWM {index + 1}硬件端点尚未实现")
        frequency = int(item.get("frequency_hz", 0))
        if frequency < 1_000 or frequency > 2_000_000:
            raise ProjectConfigError(
                f"PWM {index + 1}频率必须位于1000～2000000 Hz")
        claim(item.get("pin") or endpoint["pin"], f"PWM {index + 1}")

    strip_catalog = board.get("waveform", {}).get("ws2812", [])
    for index, item in enumerate(strips):
        endpoint = _find(strip_catalog, "endpoint_id", item.get("endpoint_id"))
        if endpoint is None or endpoint.get("backend_status") != "implemented":
            raise ProjectConfigError(f"WS2812 {index + 1}硬件端点尚未实现")
        pixels = int(item.get("pixel_count", 0))
        if pixels <= 0 or pixels > int(endpoint.get("max_pixels", 0)):
            raise ProjectConfigError(f"WS2812 {index + 1}灯珠数量超出端点范围")
        claim(item.get("pin") or endpoint["pin"], f"WS2812 {index + 1}")

    for index, axis in enumerate(axes):
        owner = f"轴 {index + 1}"
        claim(axis.get("step"), owner + " STEP")
        claim(axis.get("dir"), owner + " DIR")
        source = axis.get("enable_source")
        if source is None:
            claim(axis.get("enable"), owner + " EN")
        else:
            source = int(source)
            if source < 0 or source >= index:
                raise ProjectConfigError(f"{owner}共享EN引用无效")
        if axis.get("limit"):
            claim(axis["limit"], owner + " LIMIT")
        driver = axis.get("driver_type") or (
            "tmc2209_uart" if axis.get("tmc_uart") else "none")
        if driver == "tmc2209_uart":
            claim(axis.get("tmc_uart"), owner + " TMC UART")
            address = int(axis.get("tmc_address", 0))
            if address < 0 or address > 3:
                raise ProjectConfigError(f"{owner}的TMC2209地址必须位于0～3")
        elif driver != "none":
            raise ProjectConfigError(f"{owner}驱动类型暂不支持：{driver}")
        rate = int(axis.get("maximum_step_rate_hz", 10_000))
        if rate < 1_000 or rate > 250_000:
            raise ProjectConfigError(
                f"{owner}最大STEP频率必须位于1000～250000 Hz")

    total_rate = sum(int(axis.get("maximum_step_rate_hz", 10_000))
                     for axis in axes)
    if total_rate > 500_000:
        raise ProjectConfigError("全部轴的STEP频率预算之和不能超过500000 Hz")

    return board, {"gpio": gpio, "uart": uart, "axes": axes,
                   "pwm": pwm, "strips": strips}


def generate_project_config(draft: dict, catalog: dict) -> ProjectConfigResult:
    """校验 Studio 工程并生成完整、可由 Kconfig 再校验的 `.config`。"""
    prepared = prepare_project(draft)
    draft = prepared.document
    board, resources = _validate_and_collect(draft, catalog)
    base_relative, target = BOARD_CONFIGS[board["id"]]

    with _KCONFIG_LOCK:
        previous = Path.cwd()
        try:
            os.chdir(FIRMWARE)
            kconf = kconfiglib.Kconfig("Kconfig", warn=False)
            kconf.load_config(str(FIRMWARE / base_relative))

            expected_y: list[str] = []

            def set_value(name: str, value: object) -> None:
                symbol = kconf.syms.get(name)
                if symbol is None:
                    raise ProjectConfigError(f"固件Kconfig缺少{name}")
                text = ("y" if value is True else "n" if value is False
                        else str(value))
                symbol.set_value(text)
                if text == "y":
                    expected_y.append(name)

            set_value("REMOTEBSP_DEVICE_PARAMS", True)
            set_value("REMOTEBSP_STATIC_GPIO_MAP", True)
            for name in ("WEACT_BLUEPILL_PLUS_PB2_BREATHING_LED",
                         "WEACT_G431_PC6_PWM_BREATHING_LED"):
                if name in kconf.syms:
                    set_value(name, False)

            lists = {"OUTPUT_LOW": [], "OUTPUT_HIGH": [],
                     "INPUT_FLOATING": [], "INPUT_PULLUP": [],
                     "INPUT_PULLDOWN": []}
            for item in resources["gpio"]:
                pin = _pin_symbol(item["pin"])
                if item["direction"] == "output":
                    key = "OUTPUT_HIGH" if item.get("safe_level") is True \
                        else "OUTPUT_LOW"
                else:
                    key = {"none": "INPUT_FLOATING", "up": "INPUT_PULLUP",
                           "down": "INPUT_PULLDOWN"}[item.get("pull", "none")]
                lists[key].append(pin)
            for suffix, pins in lists.items():
                set_value(f"STARTUP_GPIO_{suffix}", ",".join(pins))
            set_value("GPIO_RESOURCE_COUNT", max(1, len(resources["gpio"])))

            uart = sorted(resources["uart"], key=lambda item: item["port"])
            ports = [int(item["port"]) for item in uart]
            if ports != list(range(len(ports))):
                raise ProjectConfigError(
                    "硬件UART必须从端口0开始连续启用，不能跳过中间端口")
            set_value("HARDWARE_UART_RESOURCE_COUNT", len(uart))
            for item in uart:
                endpoint = item["endpoint_id"].upper()
                port = int(item["port"])
                if port == 0 and endpoint == "USART1_PA9_PA10":
                    set_value("UART0_PINS_PA9_PA10", True)
                elif port == 0 and endpoint == "USART1_PB6_PB7":
                    set_value("UART0_PINS_PB6_PB7", True)
                elif port == 1 and endpoint == "USART2_PA2_PA3":
                    pass
                elif port == 2 and endpoint == "USART3_PB10_PB11":
                    pass
                else:
                    raise ProjectConfigError("UART端点尚未映射到固件Kconfig")

            pwm = resources["pwm"]
            set_value("REMOTEBSP_PWM", bool(pwm))
            if pwm:
                set_value("PWM_RESOURCE_COUNT", 1)
                set_value("PWM0_PIN_" + _pin_symbol(pwm[0]["pin"]), True)
                set_value("PWM_MAX_FREQUENCY_HZ",
                          max(1000, int(pwm[0]["frequency_hz"])))

            strips = resources["strips"]
            set_value("REMOTEBSP_TIMED_BITSTREAM", bool(strips))
            if strips:
                set_value("TIMED_BITSTREAM_RESOURCE_COUNT", 1)
                set_value("TIMED_BITSTREAM0_PIN_" +
                          _pin_symbol(strips[0]["pin"]), True)
                set_value("TIMED_BITSTREAM_MAX_BITS",
                          int(strips[0]["pixel_count"]) * 24)

            axes = resources["axes"]
            set_value("REMOTEBSP_MOTION", bool(axes))
            for index in range(5):
                set_value(f"MOTION_SLOT{index}_ENABLED", index < len(axes))
            tmc_slots: list[int] = []
            for index, axis in enumerate(axes):
                prefix = f"MOTION_SLOT{index}_"
                set_value(prefix + "STEP_PIN_" + _pin_symbol(axis["step"]), True)
                set_value(prefix + "DIR_PIN_" + _pin_symbol(axis["dir"]), True)
                source = axis.get("enable_source")
                if source is None:
                    set_value(prefix + "ENABLE_OWN", True)
                    set_value(prefix + "ENABLE_PIN_" +
                              _pin_symbol(axis["enable"]), True)
                else:
                    set_value(prefix + f"ENABLE_SHARE_SLOT{int(source)}", True)
                set_value(prefix + "ENABLE_ACTIVE_LOW_SETTING",
                          bool(axis.get("enable_active_low", True)))
                set_value(prefix + "DIR_INVERTED",
                          bool(axis.get("dir_inverted", False)))
                limit = axis.get("limit")
                set_value(prefix + "LIMIT_PIN_" +
                          (_pin_symbol(limit) if limit else "NONE"), True)
                driver = axis.get("driver_type") or (
                    "tmc2209_uart" if axis.get("tmc_uart") else "none")
                if driver == "tmc2209_uart":
                    set_value(prefix + "DRIVER_TMC2209_UART", True)
                    set_value(prefix + "TMC_UART_PIN_" +
                              _pin_symbol(axis["tmc_uart"]), True)
                    tmc_slots.append(index)
                else:
                    set_value(prefix + "DRIVER_STEP_DIR", True)
            if axes:
                set_value("MOTION_MAX_AXES", len(axes))
                rates = [int(axis.get("maximum_step_rate_hz", 10000))
                         for axis in axes]
                set_value("MOTION_MAX_STEP_RATE_HZ", max(rates))
                set_value("MOTION_MAX_TOTAL_STEP_RATE_HZ", sum(rates))
            set_value("REMOTEBSP_TMC2209_UART", bool(tmc_slots))
            if tmc_slots:
                set_value("TMC2209_UART_PORT_CAPACITY", max(tmc_slots) + 1)

            temporary = FIRMWARE / ".studio-config.tmp"
            try:
                kconf.write_config(str(temporary),
                                   "# RemoteBSP Studio生成，请勿手工猜测引脚依赖\n")
                config = temporary.read_text(encoding="utf-8")
            finally:
                temporary.unlink(missing_ok=True)
        finally:
            os.chdir(previous)

    for name in expected_y:
        if f"CONFIG_{name}=y" not in config:
            raise ProjectConfigError(
                f"{name}被Kconfig依赖拒绝，通常表示引脚或外设资源冲突")
    count = sum(len(value) for value in resources.values())
    return ProjectConfigResult(
        config, board["id"], target, count, prepared.sha256,
        prepared.schema_version, prepared.original_schema_version,
        prepared.migrations, prepared.summary)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="把RemoteBSP Studio工程转换为固件Kconfig配置")
    parser.add_argument("--project", type=Path, required=True,
                        help="Studio导出的工程JSON")
    parser.add_argument("--output", type=Path, required=True,
                        help="生成的.config路径")
    parser.add_argument("--catalog", type=Path, default=Path(__file__).parent / "data" /
                        "pin_catalog.json", help="板卡引脚目录")
    args = parser.parse_args()
    try:
        project = json.loads(args.project.read_text(encoding="utf-8"))
        catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
        result = generate_project_config(project, catalog)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result.config, encoding="utf-8")
    except (OSError, json.JSONDecodeError, ProjectConfigError,
            TypeError, ValueError) as error:
        parser.error(str(error))
    print(f"已生成{result.firmware_target}配置：{args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

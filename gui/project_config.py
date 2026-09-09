#!/usr/bin/env python3
"""把 Studio 工程转换为可直接构建的固件 `.config`。"""

from __future__ import annotations

import os
import threading
import argparse
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path

import kconfiglib

from project_contract import PreparedProject, prepare_project


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"
_KCONFIG_LOCK = threading.Lock()


class ProjectConfigError(ValueError):
    """Studio 工程无法转换为静态固件配置。"""


@dataclass(frozen=True)
class ProjectConfigResult:
    config: str
    static_resource_header: str
    static_resource_sha256: str
    board_id: str
    firmware_target: str
    resource_count: int
    project_sha256: str
    project_schema_version: int
    original_schema_version: int
    migrations: tuple[str, ...]
    summary: dict


STATIC_RESOURCE_SCHEMA_VERSION = 2


@dataclass(frozen=True)
class MockBoardManifestResult:
    """Studio 工程生成的、可由 Mock MCU 直接加载的板卡清单。"""

    manifest: dict
    board_id: str
    resource_count: int
    project_sha256: str
    project_schema_version: int
    original_schema_version: int
    migrations: tuple[str, ...]
    summary: dict


@dataclass(frozen=True)
class ProjectValidationResult:
    """不假设实体 BSP 已实现的 Studio 全资源静态校验结果。"""

    board_id: str
    resource_count: int
    project_sha256: str
    project_schema_version: int
    original_schema_version: int
    migrations: tuple[str, ...]
    summary: dict


@dataclass(frozen=True)
class ValidatedProjectInventory:
    """已经通过统一静态校验、可供 Studio 派生产物使用的工程库存。"""

    prepared: PreparedProject
    board: dict
    resources: dict


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


def _endpoint_kconfig_symbol(endpoint: dict, label: str) -> str:
    value = endpoint.get("kconfig_symbol")
    if not isinstance(value, str) or re.fullmatch(r"[A-Z][A-Z0-9_]*", value) \
            is None:
        raise ProjectConfigError(f"{label}缺少合法的Kconfig端点绑定")
    return value


def _pin_symbol(pin: object) -> str:
    if not isinstance(pin, str) or len(pin) not in (3, 4) or \
            not pin.startswith("P") or not pin[1].isalpha() or \
            not pin[2:].isdigit() or int(pin[2:]) > 15:
        raise ProjectConfigError(f"引脚名称无效：{pin}")
    return pin.upper()


def _bounded_integer(item: dict, key: str, label: str, minimum: int,
                     maximum: int, default: int) -> int:
    value = item.get(key, default)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProjectConfigError(f"{label}.{key}必须是整数")
    if value < minimum or value > maximum:
        raise ProjectConfigError(
            f"{label}.{key}超出合同范围{minimum}～{maximum}")
    return value


def _byte_array(item: dict, key: str, label: str,
                maximum_length: int) -> list[int]:
    value = item.get(key, [])
    if not isinstance(value, list) or any(
            isinstance(byte, bool) or not isinstance(byte, int) or
            byte < 0 or byte > 255 for byte in value):
        raise ProjectConfigError(f"{label}.{key}必须是字节数组")
    if len(value) > maximum_length:
        raise ProjectConfigError(f"{label}.{key}超过合同最大事务长度")
    return list(value)


def _bus_contract(item: dict, limit: dict, label: str,
                  allowed_flags: set[str]) -> dict:
    flags = item.get("flags", limit.get("flags", []))
    if not isinstance(flags, list) or not all(
            isinstance(flag, str) for flag in flags):
        raise ProjectConfigError(f"{label}.flags必须是字符串数组")
    if len(set(flags)) != len(flags) or not set(flags) <= allowed_flags:
        raise ProjectConfigError(f"{label}.flags含重复或超出端点能力")
    maximum_clock_hz = _bounded_integer(
        item, "maximum_clock_hz", label, 1,
        int(limit["maximum_clock_hz"]), int(limit["maximum_clock_hz"]))
    maximum_transfer_bytes = _bounded_integer(
        item, "maximum_transfer_bytes", label, 1,
        min(1024, int(limit["maximum_transfer_bytes"])),
        int(limit["maximum_transfer_bytes"]))
    queue_capacity = _bounded_integer(
        item, "queue_capacity", label, 1, int(limit["queue_capacity"]),
        int(limit["queue_capacity"]))
    minimum_timeout_us = _bounded_integer(
        item, "minimum_timeout_us", label,
        int(limit["minimum_timeout_us"]),
        int(limit["maximum_timeout_us"]),
        int(limit["minimum_timeout_us"]))
    maximum_timeout_us = _bounded_integer(
        item, "maximum_timeout_us", label, minimum_timeout_us,
        int(limit["maximum_timeout_us"]),
        int(limit["maximum_timeout_us"]))
    maximum_operations_per_second = _bounded_integer(
        item, "maximum_operations_per_second", label, 1,
        int(limit["maximum_operations_per_second"]),
        int(limit["maximum_operations_per_second"]))
    return {
        "flags": flags,
        "maximum_clock_hz": maximum_clock_hz,
        "maximum_transfer_bytes": maximum_transfer_bytes,
        "queue_capacity": queue_capacity,
        "minimum_timeout_us": minimum_timeout_us,
        "maximum_timeout_us": maximum_timeout_us,
        "maximum_operations_per_second": maximum_operations_per_second,
    }


def _validate_bus_resources(draft: dict, board: dict, claim) -> dict:
    groups = {
        "i2c_buses": _items(draft, "i2c", "buses"),
        "i2c_devices": _items(draft, "i2c", "devices"),
        "spi_buses": _items(draft, "spi", "buses"),
        "spi_devices": _items(draft, "spi", "devices"),
    }
    catalog = board.get("bus", {})
    internal = {
        (item.get("type"), item.get("controller"))
        for item in catalog.get("internal_controllers", [])
    }
    names: set[str] = set()
    parents: dict[tuple[str, str], dict] = {}
    controllers: set[tuple[str, int]] = set()

    for kind in ("i2c", "spi"):
        endpoints = catalog.get(kind, {}).get("endpoints", [])
        for index, item in enumerate(groups[f"{kind}_buses"]):
            label = f"{kind.upper()} BUS {index + 1}"
            name = item.get("name")
            if not isinstance(name, str) or not name or len(name) > 32:
                raise ProjectConfigError(f"{label}.name长度必须位于1～32")
            if name in names:
                raise ProjectConfigError(f"总线/设备名称重复：{name}")
            names.add(name)
            endpoint = _find(endpoints, "endpoint_id", item.get("endpoint_id"))
            if endpoint is None or endpoint.get("exposure") != "public":
                raise ProjectConfigError(f"{label}引用了未公开的总线端点")
            if (kind, endpoint.get("controller")) in internal:
                raise ProjectConfigError(f"{label}引用了内部转换器占用的控制器")
            if endpoint.get("backend_status") != "mock_only":
                raise ProjectConfigError(f"{label}端点状态不适用于数字孪生")
            pins = ([endpoint["scl_pin"], endpoint["sda_pin"]]
                    if kind == "i2c" else
                    [endpoint["sck_pin"], endpoint["miso_pin"],
                     endpoint["mosi_pin"]])
            controller_key = (kind, int(endpoint["instance"]))
            if controller_key in controllers:
                raise ProjectConfigError(f"{label}重复公开同一硬件控制器")
            controllers.add(controller_key)
            for pin in pins:
                claim(pin, f"{label} {endpoint['controller']}")
            normalized = {
                "name": name,
                "kind": f"{kind}_bus",
                "endpoint_id": endpoint["endpoint_id"],
                "controller": endpoint["controller"],
                "instance": int(endpoint["instance"]),
                "contract": _bus_contract(
                    item, endpoint, label, set(endpoint.get("flags", []))),
                "endpoint": endpoint,
            }
            parents[(kind, name)] = normalized
            item.clear()
            item.update(normalized)

    i2c_addresses: set[tuple[str, int]] = set()
    for index, item in enumerate(groups["i2c_devices"]):
        label = f"I2C DEVICE {index + 1}"
        name = item.get("name")
        if not isinstance(name, str) or not name or len(name) > 32 or name in names:
            raise ProjectConfigError(f"{label}.name无效或重复")
        names.add(name)
        parent_name = item.get("parent_bus")
        parent = parents.get(("i2c", parent_name))
        if parent is None:
            raise ProjectConfigError(f"{label}父总线缺失或类型错配")
        address = _bounded_integer(item, "address", label, 1, 0x7F, 0)
        if (parent_name, address) in i2c_addresses:
            raise ProjectConfigError(f"{label}与同父总线设备地址重复")
        i2c_addresses.add((parent_name, address))
        contract = _bus_contract(
            item, parent["contract"], label, set(parent["contract"]["flags"]))
        normalized = {
            "name": name, "kind": "i2c_device",
            "parent_bus": parent_name, "address": address,
            "contract": contract,
            "initial_data": _byte_array(
                item, "initial_data", label, contract["maximum_transfer_bytes"]),
        }
        item.clear()
        item.update(normalized)

    spi_selects: set[tuple[str, str]] = set()
    for index, item in enumerate(groups["spi_devices"]):
        label = f"SPI DEVICE {index + 1}"
        name = item.get("name")
        if not isinstance(name, str) or not name or len(name) > 32 or name in names:
            raise ProjectConfigError(f"{label}.name无效或重复")
        names.add(name)
        parent_name = item.get("parent_bus")
        parent = parents.get(("spi", parent_name))
        if parent is None:
            raise ProjectConfigError(f"{label}父总线缺失或类型错配")
        chip_select_pin = _pin_symbol(item.get("chip_select_pin"))
        if chip_select_pin not in parent["endpoint"].get("chip_select_pins", []):
            raise ProjectConfigError(f"{label}片选不是端点白名单引脚")
        if (parent_name, chip_select_pin) in spi_selects:
            raise ProjectConfigError(f"{label}与同父总线设备片选重复")
        spi_selects.add((parent_name, chip_select_pin))
        claim(chip_select_pin, f"{label} CS")
        contract = _bus_contract(
            item, parent["contract"], label, set(parent["contract"]["flags"]))
        mode = _bounded_integer(item, "mode", label, 0, 3, 0)
        bits = _bounded_integer(item, "bits_per_word", label, 4, 16, 8)
        normalized = {
            "name": name, "kind": "spi_device",
            "parent_bus": parent_name, "chip_select_pin": chip_select_pin,
            "mode": mode, "bits_per_word": bits, "contract": contract,
            "deterministic_response": _byte_array(
                item, "deterministic_response", label,
                contract["maximum_transfer_bytes"]),
        }
        item.clear()
        item.update(normalized)
    return groups


def _validate_and_collect(draft: dict, catalog: dict, *,
                          allow_mock_bus: bool = False) -> tuple[dict, dict]:
    if not isinstance(draft, dict) or draft.get("schema_version") != 2:
        raise ProjectConfigError("工程schema_version必须为2")
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
        if not isinstance(item.get("active_low", False), bool):
            raise ProjectConfigError(f"{owner}.active_low必须是布尔值")
        debounce_ms = item.get("debounce_ms", 0)
        if isinstance(debounce_ms, bool) or not isinstance(debounce_ms, int) \
                or debounce_ms < 0 or debounce_ms > 60_000:
            raise ProjectConfigError(f"{owner}.debounce_ms必须位于0～60000")
        safe_level = item.get("safe_level")
        if item.get("direction") == "output":
            if not isinstance(safe_level, bool):
                raise ProjectConfigError(f"{owner}.safe_level必须是布尔值")
        elif safe_level is not None:
            raise ProjectConfigError(f"{owner}输入资源不能设置safe_level")

    uart_catalog = board.get("uart", {}).get("endpoints", [])
    for index, item in enumerate(uart):
        endpoint = _find(uart_catalog, "endpoint_id", item.get("endpoint_id"))
        if endpoint is None or endpoint.get("backend_status") != "implemented":
            raise ProjectConfigError(f"UART {index + 1}硬件端点尚未实现")
        if item.get("port") != endpoint.get("port") or \
                item.get("rx_pin") != endpoint.get("rx_pin") or \
                item.get("tx_pin") != endpoint.get("tx_pin"):
            raise ProjectConfigError(
                f"UART {index + 1}逻辑端口或引脚与端点目录不一致")
        _endpoint_kconfig_symbol(endpoint, f"UART {index + 1}")
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
        if item.get("channel") != endpoint.get("channel") or \
                item.get("pin") != endpoint.get("pin"):
            raise ProjectConfigError(
                f"PWM {index + 1}通道或引脚与端点目录不一致")
        _endpoint_kconfig_symbol(endpoint, f"PWM {index + 1}")
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
        if item.get("channel") != endpoint.get("channel") or \
                item.get("pin") != endpoint.get("pin"):
            raise ProjectConfigError(
                f"WS2812 {index + 1}通道或引脚与端点目录不一致")
        _endpoint_kconfig_symbol(endpoint, f"WS2812 {index + 1}")
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

    bus = _validate_bus_resources(draft, board, claim)
    bus_count = sum(len(items) for items in bus.values())
    if bus_count and not allow_mock_bus:
        raise ProjectConfigError(
            "I2C/SPI当前仅支持生成Mock数字孪生清单，"
            "STM32 BSP尚未验收")

    return board, {"gpio": gpio, "uart": uart, "axes": axes,
                   "pwm": pwm, "strips": strips, **bus}


def _generate_static_resource_header(board: dict, resources: dict,
                                     project_sha256: str) -> str:
    """生成实体固件直接消费的只读 GPIO、UART 与波形端点表。"""
    mode_names = {
        ("output", False): "RBSP_STARTUP_GPIO_OUTPUT_LOW",
        ("output", True): "RBSP_STARTUP_GPIO_OUTPUT_HIGH",
        ("input", "none"): "RBSP_STARTUP_GPIO_INPUT_FLOATING",
        ("input", "up"): "RBSP_STARTUP_GPIO_INPUT_PULLUP",
        ("input", "down"): "RBSP_STARTUP_GPIO_INPUT_PULLDOWN",
    }
    entries: list[tuple[int, str, str]] = []
    for item in resources["gpio"]:
        pin = _pin_symbol(item["pin"])
        encoded = (ord(pin[1]) - ord("A")) * 16 + int(pin[2:])
        if item["direction"] == "output":
            key = ("output", item.get("safe_level") is True)
        else:
            key = ("input", item.get("pull", "none"))
        entries.append((encoded, pin, mode_names[key]))
    entries.sort(key=lambda value: value[0])

    uart_entries = []
    for item in sorted(resources["uart"], key=lambda value: value["port"]):
        endpoint = _find(
            board.get("uart", {}).get("endpoints", []),
            "endpoint_id", item["endpoint_id"])
        if endpoint is None:
            raise ProjectConfigError("已校验UART端点在生成期间丢失")
        uart_entries.append((
            int(endpoint["port"]), _pin_symbol(endpoint["rx_pin"]),
            _pin_symbol(endpoint["tx_pin"]),
            int(endpoint["minimum_baud_rate"]),
            int(endpoint["maximum_baud_rate"]),
            _endpoint_kconfig_symbol(endpoint, "UART")))

    pwm_entries = []
    for item in resources["pwm"]:
        endpoint = _find(
            board.get("waveform", {}).get("pwm", []),
            "endpoint_id", item["endpoint_id"])
        if endpoint is None:
            raise ProjectConfigError("已校验PWM端点在生成期间丢失")
        pwm_entries.append((
            int(endpoint["channel"]), _pin_symbol(endpoint["pin"]),
            int(item["frequency_hz"]),
            _endpoint_kconfig_symbol(endpoint, "PWM")))
    pwm_entries.sort(key=lambda value: value[0])

    timed_entries = []
    for item in resources["strips"]:
        endpoint = _find(
            board.get("waveform", {}).get("ws2812", []),
            "endpoint_id", item["endpoint_id"])
        if endpoint is None:
            raise ProjectConfigError("已校验定时位流端点在生成期间丢失")
        timed_entries.append((
            int(endpoint["channel"]), _pin_symbol(endpoint["pin"]),
            int(item["pixel_count"]) * 24,
            _endpoint_kconfig_symbol(endpoint, "定时位流")))
    timed_entries.sort(key=lambda value: value[0])

    board_type = board["board_type"]
    if isinstance(board_type, str):
        board_type = int(board_type, 0)
    storage_count = max(1, len(entries))
    lines = [
        "/* RemoteBSP Studio生成；输入已经过统一板卡能力目录校验。 */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        '#include "remotebsp_config.h"',
        '#include "remotebsp_embedded/startup_gpio.h"',
        '#include "remotebsp_embedded/static_resources.h"',
        "",
        f"#define RBSP_STUDIO_RESOURCE_SCHEMA_VERSION {STATIC_RESOURCE_SCHEMA_VERSION}U",
        f"#define RBSP_STUDIO_RESOURCE_BOARD_TYPE UINT32_C(0x{board_type:08X})",
        f'#define RBSP_STUDIO_RESOURCE_PROJECT_SHA256 "{project_sha256}"',
        f"#define RBSP_STUDIO_GPIO_RESOURCE_COUNT {len(entries)}U",
        f"static const rbsp_startup_gpio_entry_t rbsp_studio_gpio_resources[{storage_count}] = {{",
    ]
    if entries:
        for encoded, pin, mode in entries:
            lines.append(f"    {{{encoded}U, {mode}}}, /* {pin} */")
    else:
        lines.append(
            "    {0U, RBSP_STARTUP_GPIO_INPUT_FLOATING}, /* 空表占位，不计入数量 */")
    lines.extend(["};", ""])

    def encoded_pin(pin: str) -> int:
        return (ord(pin[1]) - ord("A")) * 16 + int(pin[2:])

    lines.extend([
        f"#define RBSP_STUDIO_UART_RESOURCE_COUNT {len(uart_entries)}U",
        "#if CONFIG_HARDWARE_UART_RESOURCE_COUNT != RBSP_STUDIO_UART_RESOURCE_COUNT",
        '#error "Studio UART表与Kconfig资源数不一致"',
        "#endif",
    ])
    for *_, symbol in uart_entries:
        lines.extend([f"#if !defined(CONFIG_{symbol})",
                      '#error "Studio UART端点与Kconfig不一致"', "#endif"])
    lines.append(
        "static const rbsp_static_uart_entry_t "
        f"rbsp_studio_uart_resources[{max(1, len(uart_entries))}] = {{")
    if uart_entries:
        for port, rx_pin, tx_pin, minimum, maximum, _ in uart_entries:
            lines.append(
                f"    {{{port}U, {encoded_pin(rx_pin)}U, "
                f"{encoded_pin(tx_pin)}U, UINT32_C({minimum}), "
                f"UINT32_C({maximum})}}, /* RX={rx_pin}, TX={tx_pin} */")
    else:
        lines.append("    {0U, 0U, 0U, 1U, 1U}, /* 空表占位 */")
    lines.extend(["};", ""])

    def append_waveform_table(name: str, config_name: str,
                              values: list[tuple[int, str, int, str]]) -> None:
        macro = f"RBSP_STUDIO_{name}_RESOURCE_COUNT"
        lines.append(f"#define {macro} {len(values)}U")
        if values:
            lines.extend([
                f"#if !defined(CONFIG_REMOTEBSP_{config_name}) || "
                f"CONFIG_{name}_RESOURCE_COUNT != {macro}",
                f'#error "Studio {name}表与Kconfig资源数不一致"',
                "#endif",
            ])
            for *_, symbol in values:
                lines.extend([f"#if !defined(CONFIG_{symbol})",
                              f'#error "Studio {name}端点与Kconfig不一致"',
                              "#endif"])
        else:
            lines.extend([f"#if defined(CONFIG_REMOTEBSP_{config_name})",
                          f'#error "Kconfig启用了Studio未声明的{name}资源"',
                          "#endif"])
        variable = "pwm" if name == "PWM" else "timed_bitstream"
        lines.append(
            "static const rbsp_static_waveform_entry_t "
            f"rbsp_studio_{variable}_resources[{max(1, len(values))}] = {{")
        if values:
            for channel, pin, maximum, _ in values:
                lines.append(
                    f"    {{{channel}U, {encoded_pin(pin)}U, "
                    f"UINT32_C({maximum})}}, /* {pin} */")
        else:
            lines.append("    {0U, 0U, 1U}, /* 空表占位 */")
        lines.extend(["};", ""])

    append_waveform_table("PWM", "PWM", pwm_entries)
    append_waveform_table(
        "TIMED_BITSTREAM", "TIMED_BITSTREAM", timed_entries)
    return "\n".join(lines)


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
                endpoint = _find(
                    board.get("uart", {}).get("endpoints", []),
                    "endpoint_id", item["endpoint_id"])
                if endpoint is None:
                    raise ProjectConfigError("已校验UART端点在生成期间丢失")
                set_value(_endpoint_kconfig_symbol(endpoint, "UART"), True)

            pwm = resources["pwm"]
            set_value("REMOTEBSP_PWM", bool(pwm))
            if pwm:
                set_value("PWM_RESOURCE_COUNT", 1)
                endpoint = _find(
                    board.get("waveform", {}).get("pwm", []),
                    "endpoint_id", pwm[0]["endpoint_id"])
                if endpoint is None:
                    raise ProjectConfigError("已校验PWM端点在生成期间丢失")
                set_value(_endpoint_kconfig_symbol(endpoint, "PWM"), True)
                set_value("PWM_MAX_FREQUENCY_HZ",
                          max(1000, int(pwm[0]["frequency_hz"])))

            strips = resources["strips"]
            set_value("REMOTEBSP_TIMED_BITSTREAM", bool(strips))
            if strips:
                set_value("TIMED_BITSTREAM_RESOURCE_COUNT", 1)
                endpoint = _find(
                    board.get("waveform", {}).get("ws2812", []),
                    "endpoint_id", strips[0]["endpoint_id"])
                if endpoint is None:
                    raise ProjectConfigError(
                        "已校验定时位流端点在生成期间丢失")
                set_value(
                    _endpoint_kconfig_symbol(endpoint, "定时位流"), True)
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
    static_resource_header = _generate_static_resource_header(
        board, resources, prepared.sha256)
    static_resource_sha256 = hashlib.sha256(
        static_resource_header.encode("utf-8")).hexdigest()
    count = sum(len(value) for value in resources.values())
    return ProjectConfigResult(
        config, static_resource_header, static_resource_sha256,
        board["id"], target, count, prepared.sha256,
        prepared.schema_version, prepared.original_schema_version,
        prepared.migrations, prepared.summary)


def collect_validated_project(draft: dict, catalog: dict
                              ) -> ValidatedProjectInventory:
    """迁移并校验工程，返回派生产物共用的规范资源库存。"""
    prepared = prepare_project(draft)
    board, resources = _validate_and_collect(
        prepared.document, catalog, allow_mock_bus=True)
    return ValidatedProjectInventory(prepared, board, resources)


def validate_project(draft: dict, catalog: dict) -> ProjectValidationResult:
    """用与生成器相同的规则校验 Studio 工程，不生成固件。"""
    inventory = collect_validated_project(draft, catalog)
    prepared = inventory.prepared
    board = inventory.board
    resources = inventory.resources
    return ProjectValidationResult(
        board["id"], sum(len(items) for items in resources.values()),
        prepared.sha256, prepared.schema_version,
        prepared.original_schema_version, prepared.migrations,
        prepared.summary)


_BUS_RESOURCE_BASES = {
    "i2c_bus": 0x0B000000,
    "i2c_device": 0x0C000000,
    "spi_bus": 0x0D000000,
    "spi_device": 0x0E000000,
}


def _manifest_resource(kind: str, instance: int, resource_id: int,
                       contract: dict, device: bool) -> tuple[dict, dict]:
    group = {
        "type": kind,
        "first_instance": instance,
        "count": 1,
        "id_base": resource_id,
        "source": "native",
        "rx_capacity": contract["maximum_transfer_bytes"] if device else 0,
        "tx_capacity": contract["maximum_transfer_bytes"] if device else 0,
        "contract": {
            "access": ["read", "write"] if device else ["read"],
            "timing_resolution_ns": max(
                1, 1_000_000_000 // contract["maximum_clock_hz"]),
            "worst_case_latency_us": contract["maximum_timeout_us"],
            "maximum_operations_per_second":
                contract["maximum_operations_per_second"],
            "queue_capacity": contract["queue_capacity"],
            "maximum_rx_bits_per_second": contract["maximum_clock_hz"],
            "maximum_tx_bits_per_second": contract["maximum_clock_hz"],
        },
    }
    bus_contract = {
        "resource_id": resource_id,
        "kind": kind,
        **contract,
    }
    return group, bus_contract


def generate_mock_board_manifest(draft: dict, catalog: dict
                                 ) -> MockBoardManifestResult:
    """把 Studio 静态总线图生成 Mock 板卡描述 schema v2。

    该路径不生成 STM32 Kconfig，也不暗示实体 BSP 已实现。
    """
    prepared = prepare_project(draft)
    document = prepared.document
    board, resources = _validate_and_collect(
        document, catalog, allow_mock_bus=True)
    omitted = {
        "GPIO": resources["gpio"],
        "UART": resources["uart"],
        "motion": resources["axes"],
        "PWM": resources["pwm"],
        "WS2812": resources["strips"],
    }
    unsupported = [name for name, items in omitted.items() if items]
    if unsupported:
        raise ProjectConfigError(
            "Mock总线清单导出仅支持I2C/SPI，不会静默省略：" +
            "、".join(unsupported))
    resource_groups: list[dict] = []
    bus_resources: list[dict] = []
    bus_ids: dict[tuple[str, str], int] = {}

    for kind in ("i2c", "spi"):
        for item in resources[f"{kind}_buses"]:
            resource_kind = f"{kind}_bus"
            instance = item["instance"]
            resource_id = _BUS_RESOURCE_BASES[resource_kind] + instance
            group, contract = _manifest_resource(
                resource_kind, instance, resource_id, item["contract"], False)
            contract["parent_bus_resource_id"] = 0
            resource_groups.append(group)
            bus_resources.append(contract)
            bus_ids[(kind, item["name"])] = resource_id

    for kind in ("i2c", "spi"):
        for index, item in enumerate(resources[f"{kind}_devices"], start=1):
            resource_kind = f"{kind}_device"
            resource_id = _BUS_RESOURCE_BASES[resource_kind] + index
            group, contract = _manifest_resource(
                resource_kind, index, resource_id, item["contract"], True)
            contract["parent_bus_resource_id"] = bus_ids[
                (kind, item["parent_bus"])]
            if kind == "i2c":
                contract["i2c_address"] = item["address"]
                if item["initial_data"]:
                    contract["initial_data"] = item["initial_data"]
            else:
                contract.update({
                    "spi_mode": item["mode"],
                    "bits_per_word": item["bits_per_word"],
                    "spi_chip_select": _pin_symbol_value(
                        item["chip_select_pin"]),
                })
                if item["deterministic_response"]:
                    contract["deterministic_response"] = \
                        item["deterministic_response"]
            resource_groups.append(group)
            bus_resources.append(contract)

    capabilities = [kind for kind in ("i2c", "spi")
                    if resources[f"{kind}_buses"]]
    internal = board.get("bus", {}).get("internal_controllers", [])
    name = document.get("name", f"studio-{board['id']}")
    if not isinstance(name, str) or not name or len(name.encode("utf-8")) > 64:
        raise ProjectConfigError("Mock板卡名称长度必须位于1～64字节")
    board_type = board["board_type"]
    if isinstance(board_type, str):
        board_type = int(board_type, 0)
    manifest = {
        "schema_version": 2,
        "name": name,
        "board_type": board_type,
        "uuid": prepared.sha256[:32].upper(),
        "firmware_version": [0, 3, 0],
        "capabilities": capabilities,
        "resource_groups": resource_groups,
        "reserved_resources": [{
            "type": item["type"],
            "instance": item["instance"],
            "owner": item["owner"],
        } for item in internal],
        "bus_resources": bus_resources,
    }
    count = len(bus_resources)
    return MockBoardManifestResult(
        manifest, board["id"], count, prepared.sha256,
        prepared.schema_version, prepared.original_schema_version,
        prepared.migrations, prepared.summary)


def _pin_symbol_value(pin: str) -> int:
    """将 PA0..PZ15 转为 Mock 片选编号；仅是数字孪生稳定键。"""
    pin = _pin_symbol(pin)
    return (ord(pin[1]) - ord("A")) * 16 + int(pin[2:])


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

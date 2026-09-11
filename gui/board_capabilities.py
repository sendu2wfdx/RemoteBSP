#!/usr/bin/env python3
"""RemoteBSP Studio 板卡能力目录合同与校验器。"""

from __future__ import annotations

import re


CATALOG_SCHEMA_VERSION = 3
_PIN = re.compile(r"^P[A-Z][0-9]{1,2}$")
_SYMBOL = re.compile(r"^[A-Z][A-Z0-9_]*$")
_BACKEND_STATUS = {"implemented", "planned"}


class BoardCapabilityError(ValueError):
    """板卡能力源或其派生目录不符合机器合同。"""


def _fail(path: str, message: str) -> None:
    raise BoardCapabilityError(f"板卡能力目录 {path}：{message}")


def _objects(value: object, path: str) -> list[dict]:
    if not isinstance(value, list) or not all(isinstance(v, dict) for v in value):
        _fail(path, "必须是对象数组")
    return value


def validate_catalog(catalog: object) -> dict:
    """校验 UART、PWM、TimedBitstream 与固定占用的一阶段合同。"""
    if not isinstance(catalog, dict):
        _fail("$", "必须是对象")
    if catalog.get("schema_version") != CATALOG_SCHEMA_VERSION:
        _fail("schema_version", f"必须为{CATALOG_SCHEMA_VERSION}")
    boards = _objects(catalog.get("boards"), "boards")
    if not boards:
        _fail("boards", "不能为空")
    board_ids: set[str] = set()
    board_types: set[str] = set()
    for index, board in enumerate(boards):
        path = f"boards[{index}]"
        for key in ("id", "board_type", "label", "mcu"):
            if not isinstance(board.get(key), str) or not board[key]:
                _fail(f"{path}.{key}", "必须是非空字符串")
        if board["id"] in board_ids or board["board_type"] in board_types:
            _fail(path, "id或board_type重复")
        board_ids.add(board["id"])
        board_types.add(board["board_type"])
        pins = board.get("pins")
        if not isinstance(pins, list) or not pins or len(pins) != len(set(pins)) \
                or any(not isinstance(pin, str) or not _PIN.fullmatch(pin)
                       for pin in pins):
            _fail(f"{path}.pins", "必须是唯一且合法的引脚数组")
        pin_set = set(pins)
        reserved_pins: set[str] = set()
        for ri, item in enumerate(_objects(board.get("reserved"),
                                           f"{path}.reserved")):
            rp = f"{path}.reserved[{ri}]"
            if item.get("pin") not in pin_set or not isinstance(item.get("owner"), str) \
                    or not item["owner"]:
                _fail(rp, "必须引用板卡引脚并声明非空owner")
            if item["pin"] in reserved_pins:
                _fail(rp, "固定占用引脚重复")
            reserved_pins.add(item["pin"])

        exti = board.get("exti")
        if not isinstance(exti, dict):
            _fail(f"{path}.exti", "必须是对象")
        exti_ids: set[str] = set()
        exti_lines: set[int] = set()
        gpio_inputs = {
            item.get("pin") for item in board.get("gpio_interfaces", [])
            if "input" in item.get("allowed_directions", [])
        }
        for ei, endpoint in enumerate(_objects(
                exti.get("endpoints"), f"{path}.exti.endpoints")):
            ep = f"{path}.exti.endpoints[{ei}]"
            _validate_endpoint_identity(endpoint, ep, exti_ids)
            _validate_pins(endpoint, ("pin",), pin_set, ep)
            _validate_backend(endpoint, ep)
            line = endpoint.get("line")
            if isinstance(line, bool) or not isinstance(line, int) or \
                    not 0 <= line <= 15 or int(endpoint["pin"][2:]) != line:
                _fail(f"{ep}.line", "必须为与引脚号一致的0～15")
            if line in exti_lines:
                _fail(f"{ep}.line", "同一板卡EXTI line必须唯一")
            exti_lines.add(line)
            if endpoint["pin"] not in gpio_inputs:
                _fail(f"{ep}.pin", "必须引用允许输入的GPIO接口")
            if endpoint["pin"] in reserved_pins:
                _fail(f"{ep}.pin", "不得引用板级保留或AF占用引脚")
            if endpoint.get("enabled") is True:
                if endpoint["backend_status"] != "implemented":
                    _fail(f"{ep}.enabled", "默认端点必须已经实现")
                _validate_symbol(endpoint, ep)
            elif endpoint["backend_status"] == "planned" and \
                    "kconfig_symbol" in endpoint:
                _fail(f"{ep}.kconfig_symbol", "未实现端点不得绑定固件符号")

        uart = board.get("uart")
        if not isinstance(uart, dict):
            _fail(f"{path}.uart", "必须是对象")
        endpoints = _objects(uart.get("endpoints"), f"{path}.uart.endpoints")
        endpoint_ids: set[str] = set()
        default_ports: set[int] = set()
        for ei, endpoint in enumerate(endpoints):
            ep = f"{path}.uart.endpoints[{ei}]"
            _validate_endpoint_identity(endpoint, ep, endpoint_ids)
            _validate_pins(endpoint, ("rx_pin", "tx_pin"), pin_set, ep)
            _validate_backend(endpoint, ep)
            port = endpoint.get("port")
            if isinstance(port, bool) or not isinstance(port, int) or port < 0:
                _fail(f"{ep}.port", "必须是非负整数")
            minimum = endpoint.get("minimum_baud_rate")
            default = endpoint.get("baud_rate")
            maximum = endpoint.get("maximum_baud_rate")
            if not all(isinstance(v, int) and not isinstance(v, bool)
                       for v in (minimum, default, maximum)) \
                    or not 1 <= minimum <= default <= maximum:
                _fail(ep, "UART波特率范围无效")
            if endpoint["backend_status"] == "implemented":
                _validate_symbol(endpoint, ep)
            if endpoint.get("enabled") is True:
                if endpoint["backend_status"] != "implemented":
                    _fail(f"{ep}.enabled", "默认端点必须已经实现")
                if port in default_ports:
                    _fail(f"{ep}.port", "默认逻辑端口重复")
                default_ports.add(port)

        defaults = _objects(board.get("uart_defaults"), f"{path}.uart_defaults")
        if defaults != [item for item in endpoints if item.get("enabled") is True]:
            _fail(f"{path}.uart_defaults", "必须由enabled端点按原顺序派生")

        waveform = board.get("waveform")
        if not isinstance(waveform, dict):
            _fail(f"{path}.waveform", "必须是对象")
        for kind, pin_fields, numeric_fields in (
            ("pwm", ("pin",), ("channel", "frequency_hz")),
            ("ws2812", ("pin",), ("channel", "pixel_count", "max_pixels",
                                      "reset_time_us")),
        ):
            ids: set[str] = set()
            default_channels: set[int] = set()
            for ei, endpoint in enumerate(_objects(waveform.get(kind),
                                                   f"{path}.waveform.{kind}")):
                ep = f"{path}.waveform.{kind}[{ei}]"
                _validate_endpoint_identity(endpoint, ep, ids)
                _validate_pins(endpoint, pin_fields, pin_set, ep)
                capable = endpoint.get("capable_pins")
                if not isinstance(capable, list) or endpoint["pin"] not in capable \
                        or any(pin not in pin_set for pin in capable):
                    _fail(f"{ep}.capable_pins", "必须包含当前引脚且均属于板卡")
                _validate_backend(endpoint, ep)
                for field in numeric_fields:
                    value = endpoint.get(field)
                    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                        _fail(f"{ep}.{field}", "必须是非负整数")
                if endpoint["backend_status"] == "implemented":
                    _validate_symbol(endpoint, ep)
                elif "kconfig_symbol" in endpoint:
                    _fail(f"{ep}.kconfig_symbol", "未实现端点不得绑定固件符号")
                if endpoint.get("enabled") is True and \
                        endpoint["backend_status"] != "implemented":
                    _fail(f"{ep}.enabled", "默认端点必须已经实现")
                if endpoint.get("enabled") is True:
                    if endpoint["channel"] in default_channels:
                        _fail(f"{ep}.channel", "默认通道重复")
                    default_channels.add(endpoint["channel"])
                if endpoint["pin"] in reserved_pins and endpoint.get("enabled") is True:
                    _fail(f"{ep}.pin", "默认端点不得占用板级保留引脚")
    return catalog


def _validate_endpoint_identity(endpoint: dict, path: str, seen: set[str]) -> None:
    value = endpoint.get("endpoint_id")
    if not isinstance(value, str) or not value or value in seen:
        _fail(f"{path}.endpoint_id", "必须是同类端点内唯一的非空字符串")
    seen.add(value)


def _validate_pins(endpoint: dict, fields: tuple[str, ...], pins: set[str], path: str) -> None:
    for field in fields:
        if endpoint.get(field) not in pins:
            _fail(f"{path}.{field}", "必须引用板卡公开引脚")


def _validate_backend(endpoint: dict, path: str) -> None:
    if endpoint.get("backend_status") not in _BACKEND_STATUS:
        _fail(f"{path}.backend_status", "必须为implemented或planned")


def _validate_symbol(endpoint: dict, path: str) -> None:
    value = endpoint.get("kconfig_symbol")
    if not isinstance(value, str) or _SYMBOL.fullmatch(value) is None:
        _fail(f"{path}.kconfig_symbol", "已实现端点必须绑定合法Kconfig符号")

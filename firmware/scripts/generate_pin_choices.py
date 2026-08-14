#!/usr/bin/env python3
"""生成运动引脚 Kconfig 选择项和 GUI 共用的板卡引脚目录。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
KCONFIG_OUTPUT = ROOT / "firmware" / "Kconfig.motion_pins.generated"
CATALOG_OUTPUT = ROOT / "gui" / "data" / "pin_catalog.json"

MCUS = {
    "stm32f072rbt6": {
        "condition": "BOARD_STM32F072RBT6",
        "label": "STM32F072RBT6",
        "pins": [*(f"PA{i}" for i in range(16)), *(f"PB{i}" for i in range(16)),
                 *(f"PC{i}" for i in range(16)), "PD2"],
    },
    "stm32f103cbt6": {
        "condition": "BOARD_STM32F103CBT6",
        "label": "STM32F103CBT6",
        "pins": [*(f"PA{i}" for i in range(16)), *(f"PB{i}" for i in range(16)),
                 "PC13", "PC14", "PC15", "PD0", "PD1"],
    },
    "stm32g431cbu6": {
        "condition": "BOARD_STM32G431CBU6",
        "label": "STM32G431CBU6",
        "pins": [*(f"PA{i}" for i in range(16)), *(f"PB{i}" for i in range(16)),
                 "PC4", "PC6", "PC10", "PC11", "PC13"],
    },
}

BOARDS = {
    "mellow-fly-d5-v1": {
        "label": "STM32F072RBT6 / Mellow FLY-D5",
        "board_type": "0x00F072D5",
        "maximum_step_rate_hz": 50000,
        "mcu": "stm32f072rbt6",
        "reserved": {
            "PA11": "Katapult USB DM", "PA12": "Katapult USB DP",
            "PA13": "SWDIO", "PA14": "SWCLK", "PB8": "CAN RX", "PB9": "CAN TX",
        },
        "defaults": [
            ["PC15", "PC14", "PC2", "PC13", "PB4"],
            ["PA1", "PA0", "PA2", "PC3", "PB3"],
            ["PA5", "PA4", "PA6", "PA3", "PD2"],
            ["PB10", "PB2", "PB11", "PB1", None],
            ["PC5", "PC4", "PB0", "PA7", None],
        ],
        "gpio_interfaces": [],
        "uart": {"endpoints": []},
        "waveform": {
            "pwm": [
                {"endpoint_id": "tim3_ch1_pa6", "enabled": False,
                 "name": "pwm_0", "channel": 0, "pin": "PA6",
                 "capable_pins": ["PA6"], "timer": "TIM3_CH1",
                 "frequency_group": "TIM3", "backend_status": "implemented",
                 "frequency_hz": 20000, "default_duty_percent": 50,
                 "active_low": False},
                {"endpoint_id": "tim3_ch2_pa7", "enabled": False,
                 "channel": 1, "pin": "PA7", "capable_pins": ["PA7"],
                 "timer": "TIM3_CH2", "frequency_group": "TIM3",
                 "backend_status": "planned"},
                {"endpoint_id": "tim3_ch3_pb0", "enabled": False,
                 "channel": 2, "pin": "PB0", "capable_pins": ["PB0"],
                 "timer": "TIM3_CH3", "frequency_group": "TIM3",
                 "backend_status": "planned"},
                {"endpoint_id": "tim3_ch4_pb1", "enabled": False,
                 "channel": 3, "pin": "PB1", "capable_pins": ["PB1"],
                 "timer": "TIM3_CH4", "frequency_group": "TIM3",
                 "backend_status": "planned"},
            ],
            "ws2812": [
                {"endpoint_id": "tim1_ch1_pa8_dma1_ch2", "enabled": False,
                 "name": "strip_0", "channel": 0, "pin": "PA8",
                 "capable_pins": ["PA8"], "timer_dma": "TIM1_CH1 + DMA1_Channel2",
                 "timer_group": "TIM1", "dma_resource": "DMA1_Channel2",
                 "backend_status": "implemented", "pixel_count": 8,
                 "max_pixels": 32, "color_order": "GRB", "reset_time_us": 80},
                {"endpoint_id": "tim1_ch2_pa9_dma1_ch3", "enabled": False,
                 "channel": 1, "pin": "PA9", "capable_pins": ["PA9"],
                 "timer_dma": "TIM1_CH2 + DMA1_Channel3", "timer_group": "TIM1",
                 "dma_resource": "DMA1_Channel3", "backend_status": "planned"},
                {"endpoint_id": "tim1_ch3_pa10_dma1_ch4", "enabled": False,
                 "channel": 2, "pin": "PA10", "capable_pins": ["PA10"],
                 "timer_dma": "TIM1_CH3 + DMA1_Channel4", "timer_group": "TIM1",
                 "dma_resource": "DMA1_Channel4", "backend_status": "planned"},
            ],
        },
    },
    "weact-bluepill-plus-v1": {
        "label": "STM32F103CBT6 / WeAct BluePill Plus",
        "board_type": "0x000103CB",
        "maximum_step_rate_hz": 50000,
        "mcu": "stm32f103cbt6",
        "reserved": {
            "PA9": "USART1 TX", "PA10": "USART1 RX", "PA11": "Katapult USB DM",
            "PA12": "Katapult USB DP", "PA13": "SWDIO", "PA14": "SWCLK",
            "PB2": "板载呼吸灯", "PB8": "CAN RX", "PB9": "CAN TX",
            "PC14": "32.768kHz LSE", "PC15": "32.768kHz LSE",
            "PD0": "8MHz HSE OSC_IN", "PD1": "8MHz HSE OSC_OUT",
        },
        "defaults": [
            ["PA1", "PA2", "PA3", "PA4", None],
            ["PA5", "PA6", "PA7", "PA8", None],
            ["PA15", "PB0", "PB1", "PC13", None],
            ["PB3", "PB4", "PB5", "PB6", None],
            ["PB7", "PB10", "PB11", "PB12", None],
        ],
        "gpio_interfaces": [{
            "id": "user_button",
            "label": "用户按键",
            "pin": "PA0",
            "allowed_directions": ["input"],
            "allowed_pulls": ["down"],
            "default_direction": "input",
            "default_pull": "down",
            "active_low": False,
            "safe_level": None,
            "debounce_ms": 10,
        }],
        "uart": {
            "endpoints": [
                {
                    "endpoint_id": "usart1_pa9_pa10",
                    "enabled": True,
                    "name": "uart_0",
                    "port": 0,
                    "rx_pin": "PA10",
                    "tx_pin": "PA9",
                    "direction_pin": None,
                    "baud_rate": 115200,
                    "minimum_baud_rate": 300,
                    "maximum_baud_rate": 4500000,
                    "backend_status": "implemented",
                },
                {
                    "endpoint_id": "usart1_pb6_pb7",
                    "enabled": False,
                    "name": "uart_0",
                    "port": 0,
                    "rx_pin": "PB7",
                    "tx_pin": "PB6",
                    "direction_pin": None,
                    "baud_rate": 115200,
                    "minimum_baud_rate": 300,
                    "maximum_baud_rate": 4500000,
                    "backend_status": "implemented",
                },
            ],
        },
        "waveform": {
            "pwm": [
                {"endpoint_id": "tim3_ch1_pa6", "enabled": False,
                 "name": "pwm_0", "channel": 0, "pin": "PA6",
                 "capable_pins": ["PA6"], "timer": "TIM3_CH1",
                 "frequency_group": "TIM3", "backend_status": "implemented",
                 "frequency_hz": 20000, "default_duty_percent": 50,
                 "active_low": False},
                {"endpoint_id": "tim3_ch2_pa7", "enabled": False,
                 "channel": 1, "pin": "PA7", "capable_pins": ["PA7"],
                 "timer": "TIM3_CH2", "frequency_group": "TIM3",
                 "backend_status": "planned"},
                {"endpoint_id": "tim3_ch3_pb0", "enabled": False,
                 "channel": 2, "pin": "PB0", "capable_pins": ["PB0"],
                 "timer": "TIM3_CH3", "frequency_group": "TIM3",
                 "backend_status": "planned"},
                {"endpoint_id": "tim3_ch4_pb1", "enabled": False,
                 "channel": 3, "pin": "PB1", "capable_pins": ["PB1"],
                 "timer": "TIM3_CH4", "frequency_group": "TIM3",
                 "backend_status": "planned"},
            ],
            "ws2812": [
                {"endpoint_id": "tim1_ch1_pa8_dma1_ch2", "enabled": False,
                 "name": "strip_0", "channel": 0, "pin": "PA8",
                 "capable_pins": ["PA8"], "timer_dma": "TIM1_CH1 + DMA1_Channel2",
                 "timer_group": "TIM1", "dma_resource": "DMA1_Channel2",
                 "backend_status": "implemented", "pixel_count": 8,
                 "max_pixels": 32, "color_order": "GRB", "reset_time_us": 80},
                {"endpoint_id": "tim1_ch2_pa9_dma1_ch3", "enabled": False,
                 "channel": 1, "pin": "PA9", "capable_pins": ["PA9"],
                 "timer_dma": "TIM1_CH2 + DMA1_Channel3", "timer_group": "TIM1",
                 "dma_resource": "DMA1_Channel3", "backend_status": "planned"},
                {"endpoint_id": "tim1_ch3_pa10_dma1_ch6", "enabled": False,
                 "channel": 2, "pin": "PA10", "capable_pins": ["PA10"],
                 "timer_dma": "TIM1_CH3 + DMA1_Channel6", "timer_group": "TIM1",
                 "dma_resource": "DMA1_Channel6", "backend_status": "planned"},
            ],
        },
    },
    "weact-g431-core-v10": {
        "label": "STM32G431CBU6 / WeAct STM32G431CBU6 Core",
        "board_type": "0x000431CB",
        "maximum_step_rate_hz": 10000,
        "mcu": "stm32g431cbu6",
        "reserved": {
            "PA11": "Katapult USB DM", "PA12": "Katapult USB DP",
            "PA13": "SWDIO", "PA14": "SWCLK", "PB8": "CAN RX", "PB9": "CAN TX",
        },
        "defaults": [["PA0", "PA1", "PA2", "PA3", None]],
        "gpio_interfaces": [{
            "id": "user_button",
            "label": "用户按键",
            "pin": "PC13",
            "allowed_directions": ["input"],
            "allowed_pulls": ["down"],
            "default_direction": "input",
            "default_pull": "down",
            "active_low": False,
            "safe_level": None,
            "debounce_ms": 10,
        }],
        "uart": {"endpoints": []},
        "waveform": {
            "pwm": [
                {"endpoint_id": "tim3_ch1_pc6", "enabled": True,
                 "name": "board_led", "channel": 0, "pin": "PC6",
                 "capable_pins": ["PC6"], "timer": "TIM3_CH1",
                 "frequency_group": "TIM3", "backend_status": "implemented",
                 "frequency_hz": 10000, "default_duty_percent": 35,
                 "active_low": False},
                {"endpoint_id": "tim3_ch2_pa7", "enabled": False,
                 "channel": 1, "pin": "PA7", "capable_pins": ["PA7"],
                 "timer": "TIM3_CH2", "frequency_group": "TIM3",
                 "backend_status": "planned"},
                {"endpoint_id": "tim3_ch3_pb0", "enabled": False,
                 "channel": 2, "pin": "PB0", "capable_pins": ["PB0"],
                 "timer": "TIM3_CH3", "frequency_group": "TIM3",
                 "backend_status": "planned"},
                {"endpoint_id": "tim3_ch4_pb1", "enabled": False,
                 "channel": 3, "pin": "PB1", "capable_pins": ["PB1"],
                 "timer": "TIM3_CH4", "frequency_group": "TIM3",
                 "backend_status": "planned"},
            ],
            "ws2812": [
                {"endpoint_id": "tim1_ch1_pa8_dma1_ch1", "enabled": False,
                 "name": "strip_0", "channel": 0, "pin": "PA8",
                 "capable_pins": ["PA8"],
                 "timer_dma": "TIM1_CH1 + DMA1_Channel1 / DMAMUX",
                 "timer_group": "TIM1", "dma_resource": "DMA1_Channel1",
                 "backend_status": "implemented", "pixel_count": 16,
                 "max_pixels": 64, "color_order": "GRB", "reset_time_us": 80},
                {"endpoint_id": "tim1_ch2_pa9_dma1_ch2", "enabled": False,
                 "channel": 1, "pin": "PA9", "capable_pins": ["PA9"],
                 "timer_dma": "TIM1_CH2 + DMA1_Channel2 / DMAMUX",
                 "timer_group": "TIM1", "dma_resource": "DMA1_Channel2",
                 "backend_status": "planned"},
                {"endpoint_id": "tim1_ch3_pa10_dma1_ch3", "enabled": False,
                 "channel": 2, "pin": "PA10", "capable_pins": ["PA10"],
                 "timer_dma": "TIM1_CH3 + DMA1_Channel3 / DMAMUX",
                 "timer_group": "TIM1", "dma_resource": "DMA1_Channel3",
                 "backend_status": "planned"},
            ],
        },
    },
}

BOARD_CONDITIONS = {
    "mellow-fly-d5-v1": "BOARD_MELLOW_FLY_D5",
    "weact-bluepill-plus-v1": "BOARD_WEACT_BLUEPILL_PLUS",
    "weact-g431-core-v10": "BOARD_WEACT_G431_CORE_V10",
}

ROLES = (
    ("STEP", "STEP", False),
    ("DIR", "DIR", False),
    ("ENABLE", "EN", False),
    ("TMC_UART", "TMC2209 单线 UART", True),
    ("LIMIT", "限位/DIAG", True),
)


def pin_value(pin: str) -> int:
    return (ord(pin[1]) - ord("A")) * 16 + int(pin[2:])


def all_pins() -> list[str]:
    return sorted({pin for mcu in MCUS.values() for pin in mcu["pins"]},
                  key=pin_value)


def availability(pin: str) -> str:
    conditions = [data["condition"] for data in MCUS.values() if pin in data["pins"]]
    return " || ".join(conditions)


def fixed_reservation_conditions(pin: str) -> list[str]:
    conditions = []
    if pin in {"PA13", "PA14"}:
        conditions.append("n")
    if pin in {"PA11", "PA12"}:
        conditions.append("!CAN_PINS_PA11_PA12")
        conditions.append("!F072_RESERVE_KATAPULT_USB_PINS")
        conditions.append("!(BOARD_MELLOW_FLY_D5 || BOARD_WEACT_BLUEPILL_PLUS || BOARD_WEACT_G431_CORE_V10)")
    if pin in {"PB8", "PB9"}:
        conditions.append("!CAN_PINS_PB8_PB9")
    if pin in {"PA9", "PA10"}:
        conditions.append("!(HARDWARE_UART_RESOURCE_COUNT > 0 && UART0_PINS_PA9_PA10)")
    if pin in {"PB6", "PB7"}:
        conditions.append("!(HARDWARE_UART_RESOURCE_COUNT > 0 && UART0_PINS_PB6_PB7)")
    if pin in {"PD0", "PD1"}:
        conditions.append("!F103_CLOCK_HSE_8MHZ")
    if pin in {"PC14", "PC15"}:
        conditions.append("!BOARD_HAS_LSE_32768")
    if pin == "PC6":
        conditions.append("!WEACT_G431_PC6_PWM_BREATHING_LED")
    if pin == "PB2":
        conditions.append("!WEACT_BLUEPILL_PLUS_PB2_BREATHING_LED")
    return conditions


def generate_kconfig() -> str:
    lines = [
        "# 此文件由 firmware/scripts/generate_pin_choices.py 生成，请勿手工修改。",
        "menu \"兼容期静态步进槽映射（后续迁移到运行时资源清单）\"",
        "    depends on BOARD_STM32F072RBT6 || BOARD_STM32F103CBT6 || BOARD_STM32G431CBU6",
        "",
        "comment \"引脚按 GPIO 端口分组；已被固定外设或前面字段占用的引脚不会显示\"",
        "",
    ]
    used_stage = 0
    pins = all_pins()
    for slot in range(5):
        lines += [
            f"config MOTION_SLOT{slot}_ENABLED",
            f"    bool \"启用槽 {slot}\"",
            "    default y if BOARD_MELLOW_FLY_D5",
            "    default n",
            "",
            f"if MOTION_SLOT{slot}_ENABLED",
        ]
        for role, prompt, optional in ROLES[:2]:
            lines += generate_choice(slot, role, prompt, optional, used_stage, pins)
            used_stage += 1
        lines += generate_enable_choice(slot, used_stage, pins)
        used_stage += 1
        lines += [
            f"choice MOTION_SLOT{slot}_DRIVER",
            f"    prompt \"槽 {slot} 步进驱动器\"",
            f"    default MOTION_SLOT{slot}_DRIVER_TMC2209_UART if BOARD_MELLOW_FLY_D5",
            f"    default MOTION_SLOT{slot}_DRIVER_STEP_DIR",
            f"config MOTION_SLOT{slot}_DRIVER_STEP_DIR",
            "    bool \"通用 STEP/DIR 驱动器\"",
            f"config MOTION_SLOT{slot}_DRIVER_TMC2209_UART",
            "    bool \"TMC2209（STEP/DIR + 单线 UART）\"",
            "    select REMOTEBSP_TMC2209_UART",
            "endchoice",
            "",
        ]
        lines += generate_choice(slot, "TMC_UART", "TMC2209 单线 UART", True, used_stage, pins,
                                 f"MOTION_SLOT{slot}_DRIVER_TMC2209_UART")
        used_stage += 1
        lines += [
            f"config MOTION_SLOT{slot}_DIR_INVERTED",
            f"    bool \"槽 {slot} DIR 反相\"",
            "    default n",
        ]
        lines += generate_choice(slot, "LIMIT", "限位/DIAG", True, used_stage, pins)
        used_stage += 1
        lines += ["endif", ""]
    lines += ["endmenu", ""]
    return "\n".join(lines)


def generate_enable_choice(slot: int, used_stage: int,
                           pins: list[str]) -> list[str]:
    """生成独立 EN 或显式复用前面槽 EN 的选择和极性派生。"""
    source = f"MOTION_SLOT{slot}_ENABLE"
    result = [
        f"choice {source}_SOURCE",
        f"    prompt \"槽 {slot} EN 来源\"",
        f"    default {source}_OWN",
        f"config {source}_OWN",
        "    bool \"独立 EN 引脚\"",
    ]
    for previous in range(slot):
        result += [
            f"config {source}_SHARE_SLOT{previous}",
            f"    bool \"与槽 {previous} 共用 EN\"",
            f"    depends on MOTION_SLOT{previous}_ENABLED",
        ]
    result += ["endchoice", ""]
    shared_defaults = [
        f"    default MOTION_SLOT{previous}_ENABLE_PIN if {source}_SHARE_SLOT{previous}"
        for previous in range(slot)
    ]
    result += generate_choice(
        slot, "ENABLE", "EN", False, used_stage, pins,
        depends=f"{source}_OWN", extra_value_defaults=shared_defaults)
    result += [
        f"config MOTION_SLOT{slot}_ENABLE_SHARED",
        "    bool",
    ]
    for previous in range(slot):
        result.append(f"    default y if {source}_SHARE_SLOT{previous}")
    result += [
        f"config MOTION_SLOT{slot}_ENABLE_ACTIVE_LOW_SETTING",
        f"    bool \"槽 {slot} EN 低有效\"",
        f"    depends on {source}_OWN",
        "    default y if BOARD_MELLOW_FLY_D5",
        "    default n",
        f"config MOTION_SLOT{slot}_ENABLE_ACTIVE_LOW",
        "    bool",
    ]
    for previous in range(slot):
        result.append(
            f"    default MOTION_SLOT{previous}_ENABLE_ACTIVE_LOW if {source}_SHARE_SLOT{previous}")
    result += [
        f"    default MOTION_SLOT{slot}_ENABLE_ACTIVE_LOW_SETTING",
        "",
    ]
    return result


def generate_choice(slot: int, role: str, prompt: str, optional: bool,
                    used_stage: int, pins: list[str],
                    depends: str | None = None,
                    extra_value_defaults: list[str] | None = None) -> list[str]:
    prefix = f"MOTION_SLOT{slot}_{role}_PIN"
    result = [f"choice {prefix}_SELECT", f"    prompt \"槽 {slot} {prompt} 引脚\""]
    if depends:
        result.append(f"    depends on {depends}")
    # 已知板卡的默认值优先，通用核心板默认选择首个仍可见的引脚。
    for board_id, board in BOARDS.items():
        defaults = board["defaults"]
        if slot < len(defaults):
            index = {name: pos for pos, (name, _, _) in enumerate(ROLES)}[role]
            pin = defaults[slot][index]
            if pin is None and optional:
                result.append(f"    default {prefix}_NONE if {BOARD_CONDITIONS[board_id]}")
            elif pin:
                result.append(f"    default {prefix}_{pin} if {BOARD_CONDITIONS[board_id]}")
    if optional:
        result += [f"config {prefix}_NONE", "    bool \"未配置\""]
    for pin in pins:
        result += [f"config {prefix}_{pin}", f"    bool \"GPIO{pin[1]} / {pin}\"",
                   f"    depends on {availability(pin)}"]
        for condition in fixed_reservation_conditions(pin):
            result.append(f"    depends on {condition}")
        if used_stage > 0:
            result.append(f"    depends on !MOTION_PIN_{pin}_USED_{used_stage}")
    result += ["endchoice", "", f"config {prefix}", "    int"]
    result += extra_value_defaults or []
    if optional:
        result.append(f"    default -1 if {prefix}_NONE")
    for pin in pins:
        result.append(f"    default {pin_value(pin)} if {prefix}_{pin}")
    result += [""]
    # 逐字段累积占用状态，让后续下拉框只依赖一个隐藏符号，避免生成文件二次膨胀。
    for pin in pins:
        result += [f"config MOTION_PIN_{pin}_USED_{used_stage + 1}", "    bool"]
        if used_stage > 0:
            result.append(
                f"    default y if MOTION_PIN_{pin}_USED_{used_stage} || {prefix}_{pin}")
        else:
            result.append(f"    default y if {prefix}_{pin}")
    result += [""]
    return result


def generate_catalog() -> str:
    boards = []
    for board_id, board in BOARDS.items():
        mcu = MCUS[board["mcu"]]
        waveform = board.get("waveform", {"pwm": [], "ws2812": []})
        normalized_waveform = {}
        for kind in ("pwm", "ws2812"):
            templates = waveform.get(kind, [])
            base = templates[0] if templates else {}
            normalized_waveform[kind] = []
            for index, template in enumerate(templates):
                item = {**base, **template}
                if "name" not in template:
                    item["name"] = f"{'pwm' if kind == 'pwm' else 'strip'}_{index}"
                normalized_waveform[kind].append(item)
        boards.append({
            "id": board_id,
            "board_type": board["board_type"],
            "label": board["label"],
            "mcu": mcu["label"],
            "pins": mcu["pins"],
            "reserved": [{"pin": pin, "owner": owner} for pin, owner in board["reserved"].items()],
            "gpio_interfaces": board.get("gpio_interfaces", []),
            "gpio_defaults": [
                {
                    "name": interface["id"],
                    "pin": interface["pin"],
                    "direction": interface["default_direction"],
                    "pull": interface["default_pull"],
                    "active_low": interface["active_low"],
                    "safe_level": interface["safe_level"],
                    "debounce_ms": interface["debounce_ms"],
                }
                for interface in board.get("gpio_interfaces", [])
            ],
            "uart": board.get("uart", {"endpoints": []}),
            "uart_defaults": [
                endpoint
                for endpoint in board.get("uart", {}).get("endpoints", [])
                if endpoint.get("enabled", False)
            ],
            "waveform": normalized_waveform,
            "motion_defaults": [
                {"step": item[0], "dir": item[1], "enable": item[2],
                 "dir_inverted": False,
                 "enable_source": None, "enable_active_low": True,
                 "tmc_uart": item[3], "limit": item[4],
                 "maximum_step_rate_hz": board["maximum_step_rate_hz"]}
                for item in board["defaults"]
            ],
        })
    return json.dumps({"schema_version": 1, "boards": boards}, ensure_ascii=False, indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="只检查生成文件是否最新")
    args = parser.parse_args()
    outputs = {KCONFIG_OUTPUT: generate_kconfig(), CATALOG_OUTPUT: generate_catalog()}
    stale = [path for path, content in outputs.items()
             if not path.exists() or path.read_text(encoding="utf-8") != content]
    if args.check:
        if stale:
            print("以下生成文件需要更新：" + "、".join(str(path.relative_to(ROOT)) for path in stale))
            return 1
        return 0
    for path, content in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")
        print(f"已生成 {path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

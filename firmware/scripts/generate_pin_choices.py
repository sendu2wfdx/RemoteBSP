#!/usr/bin/env python3
"""生成运动引脚 Kconfig 选择项和 GUI 共用的板卡引脚目录。"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
KCONFIG_OUTPUT = ROOT / "firmware" / "Kconfig.motion_pins.generated"
CATALOG_OUTPUT = ROOT / "gui" / "data" / "pin_catalog.json"
sys.path.insert(0, str(ROOT / "gui"))
from board_capabilities import CATALOG_SCHEMA_VERSION, validate_catalog  # noqa: E402

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


def i2c_endpoint(endpoint_id: str, controller: str, instance: int,
                 scl_pin: str, sda_pin: str) -> dict:
    """声明经板级审核的完整 I2C AF 端点。

    backend_status=mock_only 表示 Studio 现阶段只能生成数字孪生
    清单，不能生成声称支持该总线的 STM32 固件。
    """
    return {
        "endpoint_id": endpoint_id,
        "controller": controller,
        "instance": instance,
        "scl_pin": scl_pin,
        "sda_pin": sda_pin,
        "exposure": "public",
        "backend_status": "mock_only",
        "maximum_clock_hz": 400000,
        "maximum_transfer_bytes": 64,
        "queue_capacity": 4,
        "minimum_timeout_us": 100,
        "maximum_timeout_us": 100000,
        "maximum_operations_per_second": 1000,
        "flags": ["repeated_start", "recovery"],
    }


def spi_endpoint(endpoint_id: str, controller: str, instance: int,
                 sck_pin: str, miso_pin: str, mosi_pin: str,
                 chip_select_pins: list[str], maximum_clock_hz: int) -> dict:
    """声明不允许任意拼接的完整 SPI AF 端点。"""
    return {
        "endpoint_id": endpoint_id,
        "controller": controller,
        "instance": instance,
        "sck_pin": sck_pin,
        "miso_pin": miso_pin,
        "mosi_pin": mosi_pin,
        "chip_select_pins": chip_select_pins,
        "exposure": "public",
        "backend_status": "mock_only",
        "maximum_clock_hz": maximum_clock_hz,
        "maximum_transfer_bytes": 64,
        "queue_capacity": 4,
        "minimum_timeout_us": 50,
        "maximum_timeout_us": 100000,
        "maximum_operations_per_second": 2000,
        "flags": ["full_duplex", "keep_chip_select"],
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
        "exti": {"endpoints": []},
        "uart": {"endpoints": []},
        "bus": {
            "i2c": {"endpoints": []},
            "spi": {"endpoints": []},
            "internal_controllers": [{
                "type": "spi", "controller": "SPI1", "instance": 1,
                "owner": "expanded-uart-bank-0",
            }],
        },
        "waveform": {
            "pwm": [
                {"endpoint_id": "tim3_ch1_pa6",
                 "kconfig_symbol": "PWM0_PIN_PA6", "enabled": False,
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
                {"endpoint_id": "tim1_ch1_pa8_dma1_ch2",
                 "kconfig_symbol": "TIMED_BITSTREAM0_PIN_PA8", "enabled": False,
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
            "PA11": "Katapult USB DM",
            "PA12": "Katapult USB DP", "PA13": "SWDIO", "PA14": "SWCLK",
            "PB2": "板载呼吸灯", "PB8": "CAN RX", "PB9": "CAN TX",
            "PC14": "32.768kHz LSE", "PC15": "32.768kHz LSE",
            "PD0": "8MHz HSE OSC_IN", "PD1": "8MHz HSE OSC_OUT",
        },
        "defaults": [
            ["PA1", "PA4", "PA5", "PA6", None],
            ["PA7", "PA8", "PA15", "PB0", None],
            ["PB1", "PB3", "PB4", "PB5", None],
            ["PB6", "PB7", "PB12", "PB13", None],
            ["PB14", "PB15", "PC13", None, None],
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
        "exti": {"endpoints": [{
            "endpoint_id": "exti0_pa0", "backend_status": "planned",
            "enabled": False, "pin": "PA0", "line": 0,
        }]},
        "uart": {
            "endpoints": [
                {
                    "endpoint_id": "usart1_pa9_pa10",
                    "kconfig_symbol": "UART0_PINS_PA9_PA10",
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
                    "kconfig_symbol": "UART0_PINS_PB6_PB7",
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
                {
                    "endpoint_id": "usart2_pa2_pa3",
                    "kconfig_symbol": "UART1_PINS_PA2_PA3",
                    "enabled": True,
                    "name": "uart_1",
                    "port": 1,
                    "rx_pin": "PA3",
                    "tx_pin": "PA2",
                    "direction_pin": None,
                    "baud_rate": 115200,
                    "minimum_baud_rate": 300,
                    "maximum_baud_rate": 2250000,
                    "backend_status": "implemented",
                },
                {
                    "endpoint_id": "usart3_pb10_pb11",
                    "kconfig_symbol": "UART2_PINS_PB10_PB11",
                    "enabled": True,
                    "name": "uart_2",
                    "port": 2,
                    "rx_pin": "PB11",
                    "tx_pin": "PB10",
                    "direction_pin": None,
                    "baud_rate": 115200,
                    "minimum_baud_rate": 300,
                    "maximum_baud_rate": 2250000,
                    "backend_status": "implemented",
                },
            ],
        },
        "bus": {
            "i2c": {"endpoints": [
                i2c_endpoint("i2c1_pb6_pb7", "I2C1", 1, "PB6", "PB7"),
            ]},
            "spi": {"endpoints": [
                spi_endpoint("spi1_pa5_pa6_pa7", "SPI1", 1,
                             "PA5", "PA6", "PA7", ["PA4", "PA15"],
                             18000000),
                spi_endpoint("spi2_pb13_pb14_pb15", "SPI2", 2,
                             "PB13", "PB14", "PB15", ["PB12"],
                             18000000),
            ]},
            "internal_controllers": [],
        },
        "waveform": {
            "pwm": [
                {"endpoint_id": "tim3_ch1_pa6",
                 "kconfig_symbol": "PWM0_PIN_PA6", "enabled": False,
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
                {"endpoint_id": "tim1_ch1_pa8_dma1_ch2",
                 "kconfig_symbol": "TIMED_BITSTREAM0_PIN_PA8", "enabled": False,
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
        "defaults": [["PA0", "PA1", "PB0", "PB1", None]],
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
        "exti": {"endpoints": [{
            "endpoint_id": "exti13_pc13", "backend_status": "planned",
            "enabled": False, "pin": "PC13", "line": 13,
        }]},
        "uart": {
            "endpoints": [
                {
                    "endpoint_id": "usart1_pa9_pa10",
                    "kconfig_symbol": "UART0_PINS_PA9_PA10",
                    "enabled": True,
                    "name": "uart_0",
                    "port": 0,
                    "rx_pin": "PA10",
                    "tx_pin": "PA9",
                    "direction_pin": None,
                    "baud_rate": 115200,
                    "minimum_baud_rate": 300,
                    "maximum_baud_rate": 10000000,
                    "backend_status": "implemented",
                },
                {
                    "endpoint_id": "usart1_pb6_pb7",
                    "kconfig_symbol": "UART0_PINS_PB6_PB7",
                    "enabled": False,
                    "name": "uart_0",
                    "port": 0,
                    "rx_pin": "PB7",
                    "tx_pin": "PB6",
                    "direction_pin": None,
                    "baud_rate": 115200,
                    "minimum_baud_rate": 300,
                    "maximum_baud_rate": 10000000,
                    "backend_status": "implemented",
                },
                {
                    "endpoint_id": "usart2_pa2_pa3",
                    "kconfig_symbol": "UART1_PINS_PA2_PA3",
                    "enabled": True,
                    "name": "uart_1",
                    "port": 1,
                    "rx_pin": "PA3",
                    "tx_pin": "PA2",
                    "direction_pin": None,
                    "baud_rate": 115200,
                    "minimum_baud_rate": 300,
                    "maximum_baud_rate": 10000000,
                    "backend_status": "implemented",
                },
                {
                    "endpoint_id": "usart3_pb10_pb11",
                    "kconfig_symbol": "UART2_PINS_PB10_PB11",
                    "enabled": True,
                    "name": "uart_2",
                    "port": 2,
                    "rx_pin": "PB11",
                    "tx_pin": "PB10",
                    "direction_pin": None,
                    "baud_rate": 115200,
                    "minimum_baud_rate": 300,
                    "maximum_baud_rate": 10000000,
                    "backend_status": "implemented",
                },
            ],
        },
        "bus": {
            "i2c": {"endpoints": [
                i2c_endpoint("i2c1_pb6_pb7", "I2C1", 1, "PB6", "PB7"),
            ]},
            "spi": {"endpoints": [
                spi_endpoint("spi1_pa5_pa6_pa7", "SPI1", 1,
                             "PA5", "PA6", "PA7", ["PA4", "PA15"],
                             32000000),
                spi_endpoint("spi2_pb13_pb14_pb15", "SPI2", 2,
                             "PB13", "PB14", "PB15", ["PB12"],
                             32000000),
            ]},
            "internal_controllers": [],
        },
        "waveform": {
            "pwm": [
                {"endpoint_id": "tim3_ch1_pc6",
                 "kconfig_symbol": "PWM0_PIN_PC6", "enabled": True,
                 "name": "board_led", "channel": 0, "pin": "PC6",
                 "capable_pins": ["PC6"], "timer": "TIM3_CH1",
                 "frequency_group": "TIM3", "backend_status": "implemented",
                 "frequency_hz": 10000, "default_duty_percent": 35,
                 "active_low": False},
                {"endpoint_id": "tim3_ch1_pa6",
                 "kconfig_symbol": "PWM0_PIN_PA6", "enabled": False,
                 "name": "pwm_0", "channel": 0, "pin": "PA6",
                 "capable_pins": ["PA6"], "timer": "TIM3_CH1",
                 "frequency_group": "TIM3", "backend_status": "implemented",
                 "frequency_hz": 1000, "default_duty_percent": 50,
                 "active_low": False},
                {"endpoint_id": "tim2_ch3_pb10",
                 "kconfig_symbol": "PWM0_PIN_PB10", "enabled": False,
                 "name": "pwm_0", "channel": 0, "pin": "PB10",
                 "capable_pins": ["PB10"], "timer": "TIM2_CH3",
                 "frequency_group": "TIM2", "backend_status": "implemented",
                 "frequency_hz": 10000, "default_duty_percent": 50,
                 "active_low": False},
                {"endpoint_id": "tim2_ch4_pb11",
                 "kconfig_symbol": "PWM1_PIN_PB11", "enabled": False,
                 "name": "pwm_1", "channel": 1, "pin": "PB11",
                 "capable_pins": ["PB11"], "timer": "TIM2_CH4",
                 "frequency_group": "TIM2", "backend_status": "implemented",
                 "frequency_hz": 10000, "default_duty_percent": 50,
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
                {"endpoint_id": "tim1_ch1_pa8_dma1_ch1",
                 "kconfig_symbol": "TIMED_BITSTREAM0_PIN_PA8", "enabled": False,
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
    if pin in {"PA2", "PA3"}:
        conditions.append("!(HARDWARE_UART_RESOURCE_COUNT > 1 && UART1_PINS_PA2_PA3)")
    if pin in {"PB10", "PB11"}:
        conditions.append("!(HARDWARE_UART_RESOURCE_COUNT > 2 && UART2_PINS_PB10_PB11)")
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
                # 端点与 Kconfig 的绑定不能从首项继承；未实现端点没有
                # 可启用的固件符号，否则会把 planned 端点伪装成同一后端。
                if "kconfig_symbol" not in template:
                    item.pop("kconfig_symbol", None)
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
            "exti": board.get("exti", {"endpoints": []}),
            "uart": board.get("uart", {"endpoints": []}),
            "uart_defaults": [
                endpoint
                for endpoint in board.get("uart", {}).get("endpoints", [])
                if endpoint.get("enabled", False)
            ],
            "waveform": normalized_waveform,
            "bus": board.get("bus", {
                "i2c": {"endpoints": []},
                "spi": {"endpoints": []},
                "internal_controllers": [],
            }),
            "motion_defaults": [
                {"step": item[0], "dir": item[1], "enable": item[2],
                 "dir_inverted": False,
                 "enable_source": None, "enable_active_low": True,
                 "tmc_uart": item[3], "limit": item[4],
                 "maximum_step_rate_hz": board["maximum_step_rate_hz"]}
                for item in board["defaults"]
            ],
        })
    catalog = {"schema_version": CATALOG_SCHEMA_VERSION, "boards": boards}
    validate_catalog(catalog)
    return json.dumps(catalog, ensure_ascii=False, indent=2) + "\n"


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

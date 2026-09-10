#!/usr/bin/env python3
"""读取 Kconfig/.config，生成供 C 固件使用的配置头文件。"""

import argparse
from pathlib import Path
from typing import Mapping

try:
    import kconfiglib
except ModuleNotFoundError:
    kconfiglib = None


def parse_explicit_config(config: Path) -> dict[str, str]:
    """读取显式 defconfig，供无 kconfiglib 的受限环境使用。"""
    values: dict[str, str] = {}
    for source_line in config.read_text(encoding="utf-8").splitlines():
        line = source_line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            name, value = line.split("=", 1)
            values[name] = value
    return values


def append_derived_uart_config(output: Path, values: Mapping[str, str]) -> None:
    """生成硬件 UART 与 TMC 单线端口合计后的兼容对象数量。"""
    if values.get("CONFIG_BOARD_STM32F103CBT6") == "y" or \
            values.get("CONFIG_BOARD_STM32G431CBU6") == "y":
        hardware_default = "3"
        hardware_maximum = 3
    elif values.get("CONFIG_BOARD_STM32F072RBT6") == "y":
        hardware_default = "1"
        hardware_maximum = 1
    else:
        hardware_default = "0"
        hardware_maximum = 1
    hardware_count = int(values.get(
        "CONFIG_HARDWARE_UART_RESOURCE_COUNT", hardware_default), 0)
    tmc_enabled = (
        values.get("CONFIG_REMOTEBSP_TMC2209_UART") == "y"
        or values.get("CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART") == "y"
        or any(values.get(
            f"CONFIG_MOTION_SLOT{index}_DRIVER_TMC2209_UART") == "y"
            for index in range(5))
    )
    tmc_capacity = int(values.get(
        "CONFIG_TMC2209_UART_PORT_CAPACITY",
        values.get("CONFIG_SOFT_HALF_DUPLEX_UART_PORT_COUNT", "5")), 0) \
        if tmc_enabled else 0
    total_count = hardware_count + tmc_capacity
    if hardware_count < 0 or hardware_count > hardware_maximum:
        raise SystemExit(
            f"当前MCU的硬件UART资源数必须在0～{hardware_maximum}之间")
    if tmc_capacity < 0 or tmc_capacity > 5 or total_count > 8:
        raise SystemExit("UART 静态资源容量超出当前 Remote Core 上限")

    derived = {
        "CONFIG_HARDWARE_UART_RESOURCE_COUNT": hardware_count,
        "CONFIG_TMC2209_UART_PORT_CAPACITY": tmc_capacity,
        "CONFIG_UART_RESOURCE_COUNT": total_count,
        "CONFIG_TMC2209_UART_OBJECT_BASE": hardware_count,
    }
    lines = output.read_text(encoding="utf-8").splitlines()
    replaced: set[str] = set()
    for index, line in enumerate(lines):
        for name, value in derived.items():
            if line.startswith(f"#define {name} "):
                lines[index] = f"#define {name} {value}"
                replaced.add(name)
                break
    if replaced != set(derived):
        lines.extend([
            "",
            "/* 由配置生成器计算的逻辑 UART 对象布局。 */",
        ])
        for name, value in derived.items():
            if name not in replaced:
                lines.append(f"#define {name} {value}")
    lines.append("")
    output.write_text("\n".join(lines), encoding="utf-8")


def validate_pwm_config(values: Mapping[str, str]) -> None:
    """校验共享定时器和固定复用引脚形成的 PWM 组合约束。"""
    if values.get("CONFIG_REMOTEBSP_PWM") != "y":
        return
    pwm_count = int(values.get("CONFIG_PWM_RESOURCE_COUNT", "1"), 0)
    dual_g431 = (
        values.get("CONFIG_PWM0_PIN_PB10") == "y"
        and values.get("CONFIG_PWM1_PIN_PB11") == "y"
    )
    if pwm_count == 2 and not dual_g431:
        raise SystemExit(
            "两路 PWM 仅支持 G431 的 PB10=TIM2_CH3、PB11=TIM2_CH4 完整映射"
        )
    if not dual_g431:
        return
    if values.get("CONFIG_BOARD_STM32G431CBU6") != "y" or pwm_count != 2:
        raise SystemExit("PB10/PB11 双 PWM 仅支持 G431 的两通道配置")
    hardware_uart_count = int(values.get(
        "CONFIG_HARDWARE_UART_RESOURCE_COUNT", "3"), 0)
    if hardware_uart_count > 2 or \
            values.get("CONFIG_UART2_PINS_PB10_PB11") == "y":
        raise SystemExit("G431 PB10/PB11 双 PWM 与 USART3 引脚冲突")
    if values.get("CONFIG_REMOTEBSP_MOTION") == "y":
        raise SystemExit("G431 PB10/PB11 双 PWM 与运动模块共用 TIM2")


def validate_kconfig(kconf: "kconfiglib.Kconfig") -> dict[str, str]:
    """检查 Kconfig 难以表达的跨配置约束，并返回显式值。"""
    values = {
        f"CONFIG_{name}": symbol.str_value
        for name, symbol in kconf.syms.items()
        if symbol.str_value != ""
    }
    values.update({
        f"CONFIG_{name}": "y"
        for name, symbol in kconf.syms.items()
        if symbol.type in (kconfiglib.BOOL, kconfiglib.TRISTATE)
        and symbol.tri_value == 2
    })

    if values.get("CONFIG_REMOTEBSP_MOTION") == "y":
        enabled_axes = sum(
            values.get(f"CONFIG_MOTION_SLOT{index}_ENABLED") == "y"
            for index in range(5)
        )
        maximum_axes = int(values["CONFIG_MOTION_MAX_AXES"], 0)
        if enabled_axes > maximum_axes:
            raise SystemExit(
                f"已启用 {enabled_axes} 个静态运动槽，但最大轴容量只有 {maximum_axes}"
            )
        single_rate = int(values["CONFIG_MOTION_MAX_STEP_RATE_HZ"], 0)
        total_rate = int(values["CONFIG_MOTION_MAX_TOTAL_STEP_RATE_HZ"], 0)
        if total_rate < single_rate:
            raise SystemExit("整板总 STEP 频率预算不能小于单轴预算")

    if values.get("CONFIG_REMOTEBSP_TMC2209_UART") == "y":
        capacity = int(values["CONFIG_TMC2209_UART_PORT_CAPACITY"], 0)
        for index in range(5):
            if (values.get(f"CONFIG_MOTION_SLOT{index}_DRIVER_TMC2209_UART") == "y"
                    and index >= capacity):
                raise SystemExit(
                    f"槽 {index} 使用 TMC2209，但单线端口容量只有 {capacity}"
                )
    validate_pwm_config(values)
    return values


def write_explicit_config(config: Path, output: Path) -> None:
    """在未安装 kconfiglib 时转换完整 defconfig。"""
    lines = ["/* 由显式 defconfig 生成；请勿手工修改。 */", "#pragma once", ""]
    values = parse_explicit_config(config)
    validate_pwm_config(values)
    for name, value in values.items():
        if value == "n":
            continue
        if value == "y":
            value = "1"
        lines.append(f"#define {name} {value}")
    lines.append("")
    output.write_text("\n".join(lines), encoding="utf-8")
    append_derived_uart_config(output, values)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kconfig", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--header", required=True)
    args = parser.parse_args()

    output = Path(args.header)
    output.parent.mkdir(parents=True, exist_ok=True)
    if kconfiglib is None:
        write_explicit_config(Path(args.config), output)
    else:
        kconf = kconfiglib.Kconfig(args.kconfig)
        kconf.load_config(args.config)
        values = validate_kconfig(kconf)
        kconf.write_autoconf(str(output))
        append_derived_uart_config(output, values)


if __name__ == "__main__":
    main()

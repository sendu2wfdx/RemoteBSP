#!/usr/bin/env python3
"""验证固件配置生成器的跨选项约束和 UART 对象布局。"""

from pathlib import Path
import kconfiglib
import subprocess
import sys
import tempfile


FIRMWARE_ROOT = Path(__file__).resolve().parents[1]
GENERATOR = FIRMWARE_ROOT / "scripts" / "genconfig.py"
KCONFIG = FIRMWARE_ROOT / "Kconfig"


def generate(config: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(GENERATOR),
            "--kconfig",
            str(KCONFIG),
            "--config",
            str(config),
            "--header",
            str(output),
        ],
        cwd=FIRMWARE_ROOT,
        check=False,
        capture_output=True,
        text=True,
    )


def read_integer_defines(header: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in header.read_text(encoding="utf-8").splitlines():
        if not line.startswith("#define CONFIG_"):
            continue
        fields = line.split()
        if len(fields) == 3:
            try:
                values[fields[1]] = int(fields[2], 0)
            except ValueError:
                pass
    return values


def assert_layout(config_name: str, hardware: int, tmc: int) -> None:
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "remotebsp_config.h"
        result = generate(FIRMWARE_ROOT / "configs" / config_name, output)
        assert result.returncode == 0, result.stderr
        values = read_integer_defines(output)
        assert values["CONFIG_HARDWARE_UART_RESOURCE_COUNT"] == hardware
        assert values["CONFIG_TMC2209_UART_PORT_CAPACITY"] == tmc
        assert values["CONFIG_TMC2209_UART_OBJECT_BASE"] == hardware
        assert values["CONFIG_UART_RESOURCE_COUNT"] == hardware + tmc


def test_invalid_tmc_capacity() -> None:
    source = FIRMWARE_ROOT / "configs" / (
        "stm32f103_weact_bluepill_plus_motion_5axis_tmc2209_defconfig"
    )
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        invalid = temporary / "invalid_defconfig"
        invalid.write_text(
            source.read_text(encoding="utf-8").replace(
                "CONFIG_TMC2209_UART_PORT_CAPACITY=5",
                "CONFIG_TMC2209_UART_PORT_CAPACITY=1",
            ),
            encoding="utf-8",
        )
        result = generate(invalid, temporary / "invalid.h")
        assert result.returncode != 0
        assert "槽 1 使用 TMC2209" in result.stderr


def test_remote_budget_menu_visibility() -> None:
    kconf = kconfiglib.Kconfig(str(KCONFIG))
    edit = kconf.syms["REMOTE_BUDGET_EDIT"]
    packet = kconf.syms["REMOTE_MAX_PACKET_SIZE"]
    assert edit.str_value == "n"
    assert packet.visibility == 0
    edit.set_value(2)
    assert packet.visibility == 2


def test_shared_enable_derivation() -> None:
    kconf = kconfiglib.Kconfig(str(KCONFIG))
    kconf.load_config(str(FIRMWARE_ROOT / "configs" /
                          "stm32f072_mellow_fly_d5_katapult_defconfig"))
    kconf.syms["MOTION_SLOT1_ENABLE_SHARE_SLOT0"].set_value(2)
    assert kconf.syms["MOTION_SLOT1_ENABLE_SHARED"].str_value == "y"
    assert (kconf.syms["MOTION_SLOT1_ENABLE_PIN"].str_value ==
            kconf.syms["MOTION_SLOT0_ENABLE_PIN"].str_value)
    assert (kconf.syms["MOTION_SLOT1_ENABLE_ACTIVE_LOW"].str_value ==
            kconf.syms["MOTION_SLOT0_ENABLE_ACTIVE_LOW"].str_value)
    assert kconf.syms["MOTION_SLOT1_ENABLE_PIN_PA2"].visibility == 0


def main() -> None:
    assert_layout(
        "stm32f072_mellow_fly_d5_katapult_defconfig", 0, 5)
    assert_layout(
        "stm32f103_weact_bluepill_plus_motion_5axis_tmc2209_defconfig",
        1,
        5,
    )
    assert_layout(
        "stm32g431_weact_core_motion_1axis_tmc2209_defconfig", 0, 1)
    test_invalid_tmc_capacity()
    test_remote_budget_menu_visibility()
    test_shared_enable_derivation()
    print("固件配置生成器测试通过")


if __name__ == "__main__":
    main()

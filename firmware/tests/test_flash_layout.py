#!/usr/bin/env python3
"""Flash 布局门禁的纯软件回归测试。"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "verify_flash_layout.py"
SPEC = importlib.util.spec_from_file_location("verify_flash_layout", SCRIPT)
assert SPEC and SPEC.loader
layout = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = layout
SPEC.loader.exec_module(layout)


class FlashLayoutTests(unittest.TestCase):
    def make_fixture(self, root: Path, *, boot_end: int = 0x0801E800,
                     text_end: int = 0x08018000) -> tuple[Path, Path, Path, Path]:
        config = root / "firmware.config"
        config.write_text(
            "CONFIG_BOARD_STM32F103CBT6=y\n"
            "CONFIG_REMOTEBSP_DEVICE_PARAMS=y\n"
            "CONFIG_APP_LAYOUT_KATAPULT_8K=y\n"
            "CONFIG_APPLICATION_FLASH_OFFSET=0x2000\n", encoding="utf-8")
        elf = root / "firmware.elf"
        elf.write_bytes(b"ELF fixture")
        map_path = root / "firmware.map"
        map_path.write_text(
            " 0x0801e800 PROVIDE (__rbsp_motion_epoch_flash_start__ = x)\n"
            " 0x0801f000 PROVIDE (__rbsp_motion_epoch_flash_end__ = x)\n"
            " 0x0801f000 PROVIDE (__rbsp_health_epoch_flash_start__ = x)\n"
            " 0x0801f800 PROVIDE (__rbsp_health_epoch_flash_end__ = x)\n"
            " 0x0801f800 PROVIDE (__rbsp_device_param_flash_start__ = x)\n"
            " 0x08020000 PROVIDE (__rbsp_device_param_flash_end__ = x)\n",
            encoding="utf-8")
        boot = root / "bootloader" / "configs"
        boot.mkdir(parents=True)
        (boot / "katapult_stm32f103_weact_bluepill_plus_dual.config").write_text(
            f"CONFIG_FLASH_APPLICATION_END_ADDRESS=0x{boot_end:08x}\n",
            encoding="utf-8")
        objdump = (
            "  0 .isr_vector 00000100  08002000  08002000  00001000  2**2\n"
            "                  CONTENTS, ALLOC, LOAD, READONLY, DATA\n"
            f"  1 .text {text_end - 0x08002100:08x}  08002100  08002100  "
            "00001100  2**2\n"
            "                  CONTENTS, ALLOC, LOAD, READONLY, CODE\n")
        return config, elf, map_path, root / "bootloader", objdump

    def run_verify(self, *, boot_end: int = 0x0801E800,
                   text_end: int = 0x08018000) -> None:
        with tempfile.TemporaryDirectory() as directory:
            args = self.make_fixture(Path(directory), boot_end=boot_end,
                                     text_end=text_end)
            config, elf, map_path, bootloader, objdump = args
            with mock.patch.object(layout.subprocess, "check_output",
                                   return_value=objdump):
                with contextlib.redirect_stdout(io.StringIO()):
                    layout.verify(config, elf, map_path, bootloader)

    def test_f103_katapult_valid_layout(self) -> None:
        self.run_verify()

    def test_rejects_katapult_overwriting_health_pages(self) -> None:
        with self.assertRaisesRegex(SystemExit, "未停在 motion epoch 前"):
            self.run_verify(boot_end=0x0801F800)

    def test_rejects_loaded_section_crossing_application_end(self) -> None:
        with self.assertRaisesRegex(SystemExit, "越过应用区间"):
            self.run_verify(text_end=0x0801F100)


if __name__ == "__main__":
    unittest.main()

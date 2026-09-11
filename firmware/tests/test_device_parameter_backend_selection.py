#!/usr/bin/env python3
from pathlib import Path


root = Path(__file__).resolve().parents[2]
cmake = (root / "firmware" / "CMakeLists.txt").read_text(encoding="utf-8")

# 默认/关闭/外部三条路径必须在构建图层面互斥，避免仅靠预处理器“假裁剪”。
assert 'REGEX "^CONFIG_REMOTEBSP_DEVICE_PARAM_EXTERNAL_EEPROM=y$"' in cmake
external = cmake.index("if(RBSP_DEVICE_PARAM_EEPROM_LINES)")
disabled = cmake.index("else()\n    set(RBSP_DEVICE_PARAM_FLASH_SIZE 0x0)", external)
selection = cmake[external:disabled]
assert "src/device_parameter_eeprom.c" in selection
assert "src/device_parameter_flash.c" in selection
assert "set(RBSP_DEVICE_PARAM_FLASH_SIZE 0x0)" in selection
assert cmake.index("src/device_parameter_backend.c") < disabled

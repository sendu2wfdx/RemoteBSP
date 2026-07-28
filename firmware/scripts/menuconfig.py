#!/usr/bin/env python3
"""在 firmware 目录中启动 menuconfig。"""

from pathlib import Path
import os

import kconfiglib
import menuconfig


root = Path(__file__).resolve().parents[1]
os.chdir(root)
os.environ.setdefault("KCONFIG_CONFIG", str(root / ".config"))
menuconfig.menuconfig(kconfiglib.Kconfig("Kconfig"))

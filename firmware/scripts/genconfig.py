#!/usr/bin/env python3
"""读取 Kconfig/.config，生成供 C 固件使用的配置头文件。"""

import argparse
from pathlib import Path

try:
    import kconfiglib
except ModuleNotFoundError:
    kconfiglib = None


def write_explicit_config(config: Path, output: Path) -> None:
    """在未安装 kconfiglib 时转换完整 defconfig。"""
    lines = ["/* 由显式 defconfig 生成；请勿手工修改。 */", "#pragma once", ""]
    for source_line in config.read_text(encoding="utf-8").splitlines():
        line = source_line.strip()
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        if value == "n":
            continue
        if value == "y":
            value = "1"
        lines.append(f"#define {name} {value}")
    lines.append("")
    output.write_text("\n".join(lines), encoding="utf-8")


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
        kconf.write_autoconf(str(output))


if __name__ == "__main__":
    main()

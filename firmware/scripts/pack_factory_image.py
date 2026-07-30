#!/usr/bin/env python3
"""把 Katapult Bootloader 与 RemoteBSP APP 合成为首次烧录镜像。"""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_integer(value: str) -> int:
    """接受十进制或 0x 前缀的整数。"""
    return int(value, 0)


def pack_image(
    bootloader: bytes,
    application: bytes,
    application_offset: int,
    flash_size: int,
) -> bytes:
    """校验布局并返回使用 0xFF 填充间隙的合并镜像。"""
    if application_offset <= 0:
        raise ValueError("APP 偏移必须大于 0")
    if flash_size <= application_offset:
        raise ValueError("Flash 容量必须大于 APP 偏移")
    if len(bootloader) > application_offset:
        raise ValueError(
            f"Bootloader 大小 {len(bootloader)} 字节，超过 "
            f"{application_offset} 字节的预留区"
        )
    if len(application) > flash_size - application_offset:
        raise ValueError(
            f"APP 大小 {len(application)} 字节，超过可用的 "
            f"{flash_size - application_offset} 字节"
        )

    return (
        bootloader
        + bytes([0xFF]) * (application_offset - len(bootloader))
        + application
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="合并 Katapult Bootloader 和带偏移链接的 RemoteBSP APP"
    )
    parser.add_argument("--bootloader", required=True, type=Path)
    parser.add_argument("--application", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--application-offset",
        type=parse_integer,
        default=0x2000,
        help="APP 起始偏移，默认 0x2000",
    )
    parser.add_argument(
        "--flash-size",
        type=parse_integer,
        default=128 * 1024,
        help="芯片 Flash 容量，默认 128 KiB",
    )
    args = parser.parse_args()

    bootloader = args.bootloader.read_bytes()
    application = args.application.read_bytes()
    image = pack_image(
        bootloader,
        application,
        args.application_offset,
        args.flash_size,
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(
        f"已生成 {args.output}：Bootloader={len(bootloader)} 字节，"
        f"APP={len(application)} 字节，APP 地址偏移=0x{args.application_offset:X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

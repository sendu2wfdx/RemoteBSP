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
    persistent_size: int = 0,
) -> bytes:
    """校验布局并返回使用 0xFF 填充间隙的合并镜像。"""
    if application_offset <= 0:
        raise ValueError("APP 偏移必须大于 0")
    if flash_size <= application_offset:
        raise ValueError("Flash 容量必须大于 APP 偏移")
    if persistent_size < 0 or persistent_size >= flash_size - application_offset:
        raise ValueError("持久化区大小超出 APP 可用范围")
    if len(bootloader) > application_offset:
        raise ValueError(
            f"Bootloader 大小 {len(bootloader)} 字节，超过 "
            f"{application_offset} 字节的预留区"
        )
    application_capacity = flash_size - application_offset - persistent_size
    if len(application) > application_capacity:
        raise ValueError(
            f"APP 大小 {len(application)} 字节，超过可用的 "
            f"{application_capacity} 字节（已保留 {persistent_size} 字节参数区）"
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
    parser.add_argument(
        "--persistent-size",
        type=parse_integer,
        default=0,
        help="Flash 末端持久化参数区大小，默认 0",
    )
    args = parser.parse_args()

    bootloader = args.bootloader.read_bytes()
    application = args.application.read_bytes()
    image = pack_image(
        bootloader,
        application,
        args.application_offset,
        args.flash_size,
        args.persistent_size,
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(
        f"已生成 {args.output}：Bootloader={len(bootloader)} 字节，"
        f"APP={len(application)} 字节，APP 地址偏移=0x{args.application_offset:X}，"
        f"持久化区={args.persistent_size} 字节"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

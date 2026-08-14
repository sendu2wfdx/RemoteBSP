#!/usr/bin/env python3
"""量产镜像打包工具测试。"""

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).parents[1] / "scripts" / "pack_factory_image.py"
SPEC = importlib.util.spec_from_file_location("pack_factory_image", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PackFactoryImageTests(unittest.TestCase):
    def test_在偏移处放置应用并以擦除值填充(self) -> None:
        image = MODULE.pack_image(b"\x01\x02", b"\xAA\xBB", 8, 16)
        self.assertEqual(image, b"\x01\x02" + b"\xFF" * 6 + b"\xAA\xBB")

    def test_拒绝超过预留区的_bootloader(self) -> None:
        with self.assertRaisesRegex(ValueError, "超过"):
            MODULE.pack_image(b"\x00" * 9, b"\x01", 8, 16)

    def test_拒绝超过_flash_容量的应用(self) -> None:
        with self.assertRaisesRegex(ValueError, "超过"):
            MODULE.pack_image(b"\x00", b"\x01" * 9, 8, 16)

    def test_持久化区会缩小应用容量(self) -> None:
        with self.assertRaisesRegex(ValueError, "参数区"):
            MODULE.pack_image(b"\x00", b"\x01" * 5, 8, 16, 4)

        image = MODULE.pack_image(b"\x00", b"\x01" * 4, 8, 16, 4)
        self.assertEqual(len(image), 12)


if __name__ == "__main__":
    unittest.main()

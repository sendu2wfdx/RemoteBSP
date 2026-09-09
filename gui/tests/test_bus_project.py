import copy
import json
import sys
import unittest
from pathlib import Path


GUI_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = GUI_ROOT.parent
sys.path.insert(0, str(GUI_ROOT))

from project_config import (  # noqa: E402
    ProjectConfigError,
    generate_mock_board_manifest,
    generate_project_config,
)


CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
PROJECT_PATH = REPO_ROOT / "tests" / "data" / "studio_bus_project_v2.json"
MANIFEST_PATH = REPO_ROOT / "tests" / "data" / "studio_bus_manifest_v2.json"


class BusProjectTest(unittest.TestCase):
    def setUp(self):
        self.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.project = json.loads(PROJECT_PATH.read_text(encoding="utf-8"))

    def test_studio_project_generates_digital_twin_manifest(self):
        """Studio 与 C++ 数字孪生共用同一份黄金清单。"""
        result = generate_mock_board_manifest(self.project, self.catalog)
        expected = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        self.assertEqual(result.manifest, expected)
        self.assertEqual(result.resource_count, 4)
        self.assertEqual(result.summary["resource_count"], 4)
        self.assertEqual(result.manifest["capabilities"], ["i2c", "spi"])

    def test_physical_firmware_rejects_unimplemented_bus_backend(self):
        with self.assertRaisesRegex(ProjectConfigError, "STM32 BSP尚未验收"):
            generate_project_config(self.project, self.catalog)

    def test_mock_bus_export_rejects_non_bus_resources(self):
        """未映射的 Studio 资源必须显式拒绝，不得静默丢失。"""
        cases = {
            "GPIO": ("gpio", "resources", {
                "name": "button", "pin": "PA0", "direction": "input",
                "pull": "down", "active_low": False, "safe_level": None,
                "debounce_ms": 10,
            }),
            "UART": ("uart", "ports", {
                "name": "uart_0", "endpoint_id": "usart1_pa9_pa10",
                "port": 0, "baud_rate": 115200, "direction_pin": None,
            }),
            "motion": ("motion", "axes", {
                "step": "PA1", "dir": "PA2", "enable": "PA3",
                "enable_source": None, "driver_type": "none",
                "maximum_step_rate_hz": 10000,
            }),
            "PWM": ("pwm", "channels", {
                "endpoint_id": "tim3_ch1_pa6", "pin": "PA6",
                "frequency_hz": 20000,
            }),
            "WS2812": ("timed_bitstream", "ws2812", {
                "endpoint_id": "tim1_ch1_pa8_dma1_ch2", "pin": "PA8",
                "pixel_count": 8,
            }),
        }
        for label, (group, key, resource) in cases.items():
            with self.subTest(resource=label):
                project = copy.deepcopy(self.project)
                project[group][key].append(resource)
                with self.assertRaisesRegex(ProjectConfigError, label):
                    generate_mock_board_manifest(project, self.catalog)

    def test_bus_parent_endpoint_and_contract_are_bounded(self):
        broken = copy.deepcopy(self.project)
        broken["i2c"]["devices"][0]["parent_bus"] = "spi_main"
        with self.assertRaisesRegex(ProjectConfigError, "父总线"):
            generate_mock_board_manifest(broken, self.catalog)

        broken = copy.deepcopy(self.project)
        broken["spi"]["buses"][0]["maximum_clock_hz"] = 18_000_001
        with self.assertRaisesRegex(ProjectConfigError, "maximum_clock_hz"):
            generate_mock_board_manifest(broken, self.catalog)

        broken = copy.deepcopy(self.project)
        broken["i2c"]["devices"][0]["maximum_clock_hz"] = 400_001
        with self.assertRaisesRegex(ProjectConfigError, "maximum_clock_hz"):
            generate_mock_board_manifest(broken, self.catalog)

        broken = copy.deepcopy(self.project)
        broken["spi"]["devices"][0]["chip_select_pin"] = "PA0"
        with self.assertRaisesRegex(ProjectConfigError, "片选"):
            generate_mock_board_manifest(broken, self.catalog)

    def test_duplicate_device_endpoint_is_rejected(self):
        broken = copy.deepcopy(self.project)
        duplicate = copy.deepcopy(broken["i2c"]["devices"][0])
        duplicate["name"] = "sensor_duplicate"
        broken["i2c"]["devices"].append(duplicate)
        with self.assertRaisesRegex(ProjectConfigError, "地址重复"):
            generate_mock_board_manifest(broken, self.catalog)

    def test_internal_converter_bus_is_not_public(self):
        fly = next(board for board in self.catalog["boards"]
                   if board["id"] == "mellow-fly-d5-v1")
        internal = fly["bus"]["internal_controllers"]
        public_spi = fly["bus"]["spi"]["endpoints"]
        self.assertIn("SPI1", {item["controller"] for item in internal})
        self.assertNotIn("SPI1", {item["controller"] for item in public_spi})


if __name__ == "__main__":
    unittest.main()

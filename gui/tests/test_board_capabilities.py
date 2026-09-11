import copy
import json
import sys
import unittest
from pathlib import Path


GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from board_capabilities import (  # noqa: E402
    BoardCapabilityError,
    CATALOG_SCHEMA_VERSION,
    validate_catalog,
)
CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"


class BoardCapabilityTest(unittest.TestCase):
    def setUp(self):
        self.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))

    def test_generated_three_board_catalog_matches_contract(self):
        self.assertIs(validate_catalog(self.catalog), self.catalog)
        self.assertEqual(self.catalog["schema_version"],
                         CATALOG_SCHEMA_VERSION)
        self.assertEqual({board["id"] for board in self.catalog["boards"]}, {
            "mellow-fly-d5-v1", "weact-bluepill-plus-v1",
            "weact-g431-core-v10",
        })

    def test_rejects_unknown_pin_and_planned_firmware_binding(self):
        broken = copy.deepcopy(self.catalog)
        broken["boards"][1]["uart"]["endpoints"][0]["rx_pin"] = "PZ99"
        with self.assertRaisesRegex(BoardCapabilityError, "公开引脚"):
            validate_catalog(broken)

        broken = copy.deepcopy(self.catalog)
        planned = next(item for item in broken["boards"][0]["waveform"]["pwm"]
                       if item["backend_status"] == "planned")
        planned["kconfig_symbol"] = "PWM0_PIN_FAKE"
        with self.assertRaisesRegex(BoardCapabilityError, "未实现端点"):
            validate_catalog(broken)

    def test_rejects_duplicate_fixed_occupancy(self):
        broken = copy.deepcopy(self.catalog)
        broken["boards"][0]["reserved"].append(
            copy.deepcopy(broken["boards"][0]["reserved"][0]))
        with self.assertRaisesRegex(BoardCapabilityError, "固定占用引脚重复"):
            validate_catalog(broken)

    def test_exti_endpoints_are_explicit_disabled_and_line_unique(self):
        endpoints = [
            endpoint for board in self.catalog["boards"]
            for endpoint in board["exti"]["endpoints"]]
        self.assertTrue(endpoints)
        self.assertTrue(all(not endpoint["enabled"] for endpoint in endpoints))
        self.assertTrue(all(endpoint["backend_status"] == "implemented"
                            for endpoint in endpoints))

        broken = copy.deepcopy(self.catalog)
        duplicate = copy.deepcopy(broken["boards"][1]["exti"]["endpoints"][0])
        duplicate["endpoint_id"] = "duplicate-line"
        # PB0 与 PA0 是不同引脚，但共享 EXTI0，必须在生成前失败关闭。
        duplicate["pin"] = "PB0"
        broken["boards"][1]["gpio_interfaces"].append({
            "id": "test_pb0", "pin": "PB0",
            "allowed_directions": ["input"], "allowed_pulls": ["none"]})
        broken["boards"][1]["exti"]["endpoints"].append(duplicate)
        with self.assertRaisesRegex(BoardCapabilityError, "line必须唯一"):
            validate_catalog(broken)

        broken = copy.deepcopy(self.catalog)
        broken["boards"][2]["exti"]["endpoints"][0]["line"] = 12
        with self.assertRaisesRegex(BoardCapabilityError, "与引脚号一致"):
            validate_catalog(broken)

        broken = copy.deepcopy(self.catalog)
        endpoint = broken["boards"][1]["exti"]["endpoints"][0]
        endpoint["enabled"] = True
        endpoint.pop("kconfig_symbol")
        with self.assertRaisesRegex(BoardCapabilityError, "Kconfig符号"):
            validate_catalog(broken)

    def test_project_validation_does_not_trust_tampered_catalog(self):
        from project_config import ProjectConfigError, validate_project

        board = self.catalog["boards"][1]
        project = {
            "schema_version": 2, "board_id": board["id"],
            "gpio": {"resources": copy.deepcopy(board["gpio_defaults"])},
            "uart": {"ports": copy.deepcopy(board["uart_defaults"])},
            "motion": {"axes": []}, "pwm": {"channels": []},
            "timed_bitstream": {"ws2812": []},
            "i2c": {"buses": [], "devices": []},
            "spi": {"buses": [], "devices": []},
        }
        broken = copy.deepcopy(self.catalog)
        broken["boards"][1]["reserved"][0]["owner"] = ""
        with self.assertRaisesRegex(ProjectConfigError, "非空owner"):
            validate_project(project, broken)

    def test_project_exti_binding_generates_static_config_and_rejects_mismatch(self):
        from project_config import (ProjectConfigError,
                                    generate_project_config)

        board = self.catalog["boards"][1]
        gpio = copy.deepcopy(board["gpio_defaults"])
        gpio[0]["exti_endpoint_id"] = "exti0_pa0"
        project = {
            "schema_version": 2, "board_id": board["id"],
            "gpio": {"resources": gpio}, "uart": {"ports": []},
            "motion": {"axes": []}, "pwm": {"channels": []},
            "timed_bitstream": {"ws2812": []},
            "i2c": {"buses": [], "devices": []},
            "spi": {"buses": [], "devices": []},
        }
        generated = generate_project_config(project, self.catalog)
        self.assertIn("CONFIG_REMOTEBSP_GPIO_EXTI=y", generated.config)
        self.assertIn("CONFIG_GPIO_EXTI0_PA0=y", generated.config)
        self.assertIn("RBSP_STUDIO_EXTI_RESOURCE_COUNT 1U",
                      generated.static_resource_header)

        gpio[0]["exti_endpoint_id"] = "exti13_pc13"
        with self.assertRaisesRegex(ProjectConfigError, "静态引脚不匹配"):
            generate_project_config(project, self.catalog)


if __name__ == "__main__":
    unittest.main()

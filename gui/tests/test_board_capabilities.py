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


if __name__ == "__main__":
    unittest.main()

import copy
import json
import sys
import unittest
from pathlib import Path


GUI_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = GUI_ROOT.parent
sys.path.insert(0, str(GUI_ROOT))

from project_compare import (  # noqa: E402
    MAX_PROJECT_BYTES,
    compare_projects,
)
from project_config import ProjectConfigError  # noqa: E402


CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
PROJECT_PATH = REPO_ROOT / "tests" / "data" / "studio_bus_project_v2.json"


def default_project(board: dict) -> dict:
    return {
        "schema_version": 2,
        "board_id": board["id"],
        "gpio": {"resources": board["gpio_defaults"]},
        "uart": {"ports": board.get("uart_defaults", [])},
        "motion": {"axes": board["motion_defaults"]},
        "pwm": {"channels": [item for item in board["waveform"]["pwm"]
                              if item["enabled"]]},
        "timed_bitstream": {"ws2812": [
            item for item in board["waveform"]["ws2812"]
            if item["enabled"]]},
        "i2c": {"buses": [], "devices": []},
        "spi": {"buses": [], "devices": []},
    }


class ProjectCompareTest(unittest.TestCase):
    def setUp(self):
        self.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.project = json.loads(PROJECT_PATH.read_text(encoding="utf-8"))

    def test_equal_projects_have_stable_empty_diff(self):
        reordered = json.loads(json.dumps(self.project, sort_keys=True))
        result = compare_projects(self.project, reordered, self.catalog)
        self.assertTrue(result["equal"])
        self.assertEqual(result["format"], "PROJECT_COMPARISON_V1")
        self.assertEqual(result["changes"], [])
        self.assertRegex(result["comparison_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(result["summary"]["headline"],
                         "两份工程的板卡与资源配置一致")
        self.assertEqual(result["left"]["project_sha256"],
                         result["right"]["project_sha256"])
        repeated = compare_projects(self.project, reordered, self.catalog)
        self.assertEqual(result["comparison_sha256"],
                         repeated["comparison_sha256"])

    def test_original_schema_difference_is_visible_after_equal_migration(self):
        legacy = copy.deepcopy(self.project)
        legacy.pop("schema_version")
        result = compare_projects(legacy, self.project, self.catalog)
        self.assertFalse(result["equal"])
        self.assertEqual(result["left"]["schema_version"], 2)
        self.assertEqual(result["left"]["original_schema_version"], 0)
        self.assertEqual(result["right"]["original_schema_version"], 2)
        self.assertEqual(result["left"]["project_sha256"],
                         result["right"]["project_sha256"])
        self.assertEqual(result["changes"][0]["change"], "schema_changed")

    def test_resource_add_remove_and_contract_change_are_classified(self):
        right = copy.deepcopy(self.project)
        right["i2c"]["buses"][0]["maximum_clock_hz"] = 100_000
        added = copy.deepcopy(right["i2c"]["devices"][0])
        added["name"] = "sensor_49"
        added["address"] = 0x49
        right["i2c"]["devices"].append(added)
        right["spi"]["devices"] = []
        result = compare_projects(self.project, right, self.catalog)
        self.assertFalse(result["equal"])
        self.assertEqual(result["summary"]["added"], 1)
        self.assertEqual(result["summary"]["removed"], 1)
        self.assertGreaterEqual(result["summary"]["modified"], 1)
        self.assertTrue(any(item["change"] == "resource_added"
                            for item in result["changes"]))
        self.assertTrue(any(item["change"] == "resource_removed"
                            for item in result["changes"]))
        self.assertTrue(any(
            field["category"] == "contract" and
            field["path"].endswith("maximum_clock_hz")
            for item in result["changes"]
            if item["change"] == "resource_modified"
            for field in item["fields"]))

    def test_uart_pin_change_is_reported_as_wiring(self):
        board = next(item for item in self.catalog["boards"]
                     if item["id"] == "weact-bluepill-plus-v1")
        left = default_project(board)
        left["gpio"]["resources"] = []
        left["motion"]["axes"] = []
        left["pwm"]["channels"] = []
        left["uart"]["ports"] = left["uart"]["ports"][:1]
        right = copy.deepcopy(left)
        right["uart"]["ports"][0]["endpoint_id"] = "usart1_pb6_pb7"
        result = compare_projects(left, right, self.catalog)
        uart = next(item for item in result["changes"]
                    if item.get("identity") == "uart/000")
        self.assertEqual(uart["change"], "resource_modified")
        wiring = [field for field in uart["fields"]
                  if field["category"] == "wiring"]
        self.assertEqual({field["path"] for field in wiring},
                         {"bindings.RX.value", "bindings.TX.value"})
        self.assertTrue(all("接线" in field["summary"] for field in wiring))

    def test_board_change_and_limits_are_explicit(self):
        left_board = self.catalog["boards"][0]
        right_board = self.catalog["boards"][2]
        result = compare_projects(default_project(left_board),
                                  default_project(right_board), self.catalog)
        self.assertEqual(result["changes"][0]["change"], "board_changed")
        self.assertEqual(result["limits"]["max_project_bytes"],
                         MAX_PROJECT_BYTES)

        oversized = copy.deepcopy(self.project)
        oversized["说明"] = "大" * MAX_PROJECT_BYTES
        with self.assertRaisesRegex(ProjectConfigError, "128 KiB"):
            compare_projects(oversized, self.project, self.catalog)

    def test_each_side_must_pass_existing_validation(self):
        broken = copy.deepcopy(self.project)
        broken["spi"]["devices"][0]["chip_select_pin"] = "PA0"
        with self.assertRaisesRegex(ProjectConfigError, "片选"):
            compare_projects(self.project, broken, self.catalog)

    def test_field_details_are_explicitly_truncated_at_bound(self):
        left = copy.deepcopy(self.project)
        left["spi"] = {"buses": [], "devices": []}
        template = left["i2c"]["devices"][0]
        left["i2c"]["devices"] = []
        for address in range(1, 128):
            device = copy.deepcopy(template)
            device.update({"name": f"sensor_{address:03d}",
                           "address": address, "initial_data": []})
            left["i2c"]["devices"].append(device)
        right = copy.deepcopy(left)
        for device in right["i2c"]["devices"]:
            device.update({
                "maximum_clock_hz": 100_000,
                "maximum_transfer_bytes": 15,
                "queue_capacity": 1,
                "minimum_timeout_us": 200,
                "maximum_timeout_us": 9_000,
                "maximum_operations_per_second": 500,
                "flags": ["recovery"],
            })
        result = compare_projects(left, right, self.catalog)
        self.assertTrue(result["summary"]["truncated"])
        self.assertGreater(result["summary"]["omitted_field_changes"], 0)
        self.assertEqual(sum(len(item.get("fields", []))
                             for item in result["changes"]), 512)


if __name__ == "__main__":
    unittest.main()

import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.request import urlopen

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from server import CATALOG_PATH, make_server  # noqa: E402


class GuiTest(unittest.TestCase):
    def test_pin_catalog_has_unique_defaults(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.assertEqual(catalog["schema_version"], 1)
        self.assertEqual(len(catalog["boards"]), 3)
        for board in catalog["boards"]:
            reserved = {item["pin"] for item in board["reserved"]}
            selected = []
            for axis in board["motion_defaults"]:
                selected.extend(axis[name] for name in
                                ("step", "dir", "enable", "tmc_uart", "limit")
                                if axis[name])
                self.assertIsNone(axis["enable_source"])
                self.assertIsInstance(axis["enable_active_low"], bool)
                self.assertIsInstance(axis["dir_inverted"], bool)
            interfaces = {item["pin"]: item for item in board["gpio_interfaces"]}
            for pin, interface in interfaces.items():
                self.assertIn(pin, board["pins"])
                self.assertNotIn(pin, reserved)
                self.assertIn(interface["default_direction"],
                              interface["allowed_directions"])
                self.assertIn(interface["default_pull"],
                              interface["allowed_pulls"])
            for gpio in board["gpio_defaults"]:
                selected.append(gpio["pin"])
                interface = interfaces[gpio["pin"]]
                self.assertIn(gpio["direction"], interface["allowed_directions"])
                self.assertIn(gpio["pull"], interface["allowed_pulls"])
                self.assertIsInstance(gpio["active_low"], bool)
                self.assertGreaterEqual(gpio["debounce_ms"], 0)
            waveform = board["waveform"]
            pwm_endpoints = set()
            for pwm in waveform["pwm"]:
                self.assertNotIn(pwm["endpoint_id"], pwm_endpoints)
                pwm_endpoints.add(pwm["endpoint_id"])
                self.assertIn(pwm["pin"], pwm["capable_pins"])
                self.assertIn(pwm["pin"], board["pins"])
                self.assertGreater(pwm["frequency_hz"], 0)
                self.assertGreaterEqual(pwm["default_duty_percent"], 0)
                self.assertLessEqual(pwm["default_duty_percent"], 100)
                self.assertIsInstance(pwm["channel"], int)
                self.assertIn(pwm["backend_status"],
                              ("implemented", "planned"))
                self.assertTrue(pwm["frequency_group"])
                if pwm["enabled"]:
                    selected.append(pwm["pin"])
            strip_endpoints = set()
            for strip in waveform["ws2812"]:
                self.assertNotIn(strip["endpoint_id"], strip_endpoints)
                strip_endpoints.add(strip["endpoint_id"])
                self.assertIn(strip["pin"], strip["capable_pins"])
                self.assertIn(strip["pin"], board["pins"])
                self.assertLessEqual(strip["pixel_count"],
                                     strip["max_pixels"])
                self.assertIn(strip["color_order"], ("RGB", "GRB", "BRG"))
                self.assertGreaterEqual(strip["reset_time_us"], 50)
                self.assertIn(strip["backend_status"],
                              ("implemented", "planned"))
                self.assertTrue(strip["dma_resource"])
                if strip["enabled"]:
                    selected.append(strip["pin"])
            self.assertEqual(len(selected), len(set(selected)), board["id"])
            self.assertFalse(reserved.intersection(selected), board["id"])

    def test_http_apis_and_mock_state(self):
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "state.json"
            state_path.write_text(json.dumps({
                "schema_version": 1, "board_name": "测试板", "online": True,
                "elapsed_ms": 123, "gpio": [],
                "motion": {"state": "idle", "fault": "none", "queue_depth": 0,
                           "queue_capacity": 1, "axes": []},
                "pwm": [],
                "ws2812": {"supported": False, "strips": []},
            }), encoding="utf-8")
            server = make_server("127.0.0.1", 0, state_path)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                base = f"http://127.0.0.1:{server.server_port}"
                state = json.loads(urlopen(base + "/api/state").read())
                self.assertEqual(state["board_name"], "测试板")
                self.assertEqual(state["source"], "mock_mcu")
                catalog = json.loads(urlopen(base + "/api/catalog").read())
                self.assertEqual(catalog["schema_version"], 1)
                page = urlopen(base + "/").read()
                self.assertIn(b"RemoteBSP Studio", page)
                self.assertIn(b"gpioTable", page)
                self.assertIn(b"pwmTable", page)
                self.assertIn(b"stripTable", page)
                self.assertIn(b"controlGpio", page)
                self.assertIn(b"controlPwm", page)
                self.assertIn(b"controlStrips", page)
                self.assertIn(b"resource-fields", urlopen(
                    base + "/config.css").read())
                self.assertIn(b"motor-pointer", urlopen(
                    base + "/motor.css").read())
                self.assertIn(b"pwm-dial", urlopen(
                    base + "/waveform.css").read())
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()

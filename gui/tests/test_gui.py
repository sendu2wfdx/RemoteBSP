import base64
import copy
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.request import Request, urlopen
from unittest.mock import patch

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from firmware_builder import (  # noqa: E402
    BuildArtifact,
    FirmwareBuildError,
    FirmwareBuildResult,
    build_firmware_project,
    resolve_artifact,
)
from project_config import ProjectConfigError, generate_project_config  # noqa: E402
from server import CATALOG_PATH, make_server  # noqa: E402


class GuiTest(unittest.TestCase):
    @staticmethod
    def _default_project(board):
        return {
            "schema_version": 1,
            "board_id": board["id"],
            "gpio": {"resources": board["gpio_defaults"]},
            "uart": {"ports": board.get("uart_defaults", [])},
            "motion": {"axes": board["motion_defaults"]},
            "pwm": {"channels": [
                item for item in board["waveform"]["pwm"]
                if item["enabled"]
            ]},
            "timed_bitstream": {"ws2812": [
                item for item in board["waveform"]["ws2812"]
                if item["enabled"]
            ]},
        }

    def test_all_board_defaults_generate_static_firmware_config(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        for board in catalog["boards"]:
            with self.subTest(board=board["id"]):
                result = generate_project_config(
                    self._default_project(board), catalog)
                self.assertEqual(result.board_id, board["id"])
                self.assertGreaterEqual(result.resource_count, 1)
                self.assertIn("CONFIG_REMOTEBSP_DEVICE_PARAMS=y",
                              result.config)
                self.assertIn("CONFIG_REMOTEBSP_STATIC_GPIO_MAP=y",
                              result.config)
                if board["id"] == "weact-bluepill-plus-v1":
                    self.assertIn(
                        "CONFIG_HARDWARE_UART_RESOURCE_COUNT=3",
                        result.config)
                    self.assertIn("CONFIG_UART1_PINS_PA2_PA3=y",
                                  result.config)
                    self.assertIn("CONFIG_UART2_PINS_PB10_PB11=y",
                                  result.config)
                if board["id"] == "weact-g431-core-v10":
                    self.assertIn(
                        "CONFIG_HARDWARE_UART_RESOURCE_COUNT=3",
                        result.config)
                    self.assertIn("CONFIG_UART1_PINS_PA2_PA3=y",
                                  result.config)
                    self.assertIn("CONFIG_UART2_PINS_PB10_PB11=y",
                                  result.config)

    def test_pin_catalog_has_unique_defaults(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.assertEqual(catalog["schema_version"], 1)
        self.assertEqual(len(catalog["boards"]), 3)
        for board in catalog["boards"]:
            reserved = {item["pin"] for item in board["reserved"]}
            selected = []
            uart_endpoints = board.get("uart", {}).get("endpoints", [])
            enabled_uart = [item for item in uart_endpoints
                            if item.get("enabled")]
            self.assertEqual(enabled_uart, board.get("uart_defaults", []))
            for endpoint in uart_endpoints:
                self.assertIn(endpoint["rx_pin"], board["pins"])
                self.assertIn(endpoint["tx_pin"], board["pins"])
                self.assertGreater(endpoint["baud_rate"], 0)
                self.assertLessEqual(endpoint["minimum_baud_rate"],
                                     endpoint["baud_rate"])
                self.assertGreaterEqual(endpoint["maximum_baud_rate"],
                                        endpoint["baud_rate"])
                self.assertEqual(endpoint["backend_status"], "implemented")
            for axis in board["motion_defaults"]:
                selected.extend(axis[name] for name in
                                ("step", "dir", "enable", "tmc_uart", "limit")
                                if axis[name])
                self.assertIsNone(axis["enable_source"])
                self.assertIsInstance(axis["enable_active_low"], bool)
                self.assertIsInstance(axis["dir_inverted"], bool)
            interfaces = {item["pin"]: item
                          for item in board["gpio_interfaces"]}
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
                self.assertIn(gpio["direction"],
                              interface["allowed_directions"])
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
                self.assertIn(pwm["backend_status"],
                              ("implemented", "planned"))
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
                if strip["enabled"]:
                    selected.append(strip["pin"])
            self.assertEqual(len(selected), len(set(selected)), board["id"])
            self.assertFalse(reserved.intersection(selected), board["id"])

    def test_generator_rejects_invalid_uart_and_pin_conflict(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        board = next(item for item in catalog["boards"]
                     if item["id"] == "weact-bluepill-plus-v1")

        project = copy.deepcopy(self._default_project(board))
        project["uart"]["ports"][0]["baud_rate"] = 4_500_001
        with self.assertRaisesRegex(ProjectConfigError, "波特率"):
            generate_project_config(project, catalog)

        project = copy.deepcopy(self._default_project(board))
        project["uart"]["ports"][0]["direction_pin"] = "PB5"
        with self.assertRaisesRegex(ProjectConfigError, "方向控制"):
            generate_project_config(project, catalog)

        project = copy.deepcopy(self._default_project(board))
        project["uart"]["ports"] = project["uart"]["ports"][1:]
        with self.assertRaisesRegex(ProjectConfigError, "连续启用"):
            generate_project_config(project, catalog)

        project = copy.deepcopy(self._default_project(board))
        project["gpio"]["resources"].append(
            copy.deepcopy(project["gpio"]["resources"][0]))
        with self.assertRaisesRegex(ProjectConfigError, "重复使用"):
            generate_project_config(project, catalog)

    def test_firmware_builder_archives_bounded_artifacts(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        board = catalog["boards"][0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_root = root / "build"
            output_root = root / "out"

            def fake_runner(command, timeout):
                self.assertEqual(timeout, 12)
                if "--build" in command:
                    build_dir = Path(command[2])
                    for suffix in ("elf", "bin", "hex", "map"):
                        (build_dir / f"remotebsp-stm32f072rbt6.{suffix}").write_bytes(
                            ("test-" + suffix).encode())
                return ("测试构建输出\n"
                        "Memory region         Used Size  Region Size  %age Used\n"
                        "RAM: 1024 B 20 KB 5.00%\n"
                        "FLASH: 4096 B 126 KB 3.17%\n")

            result = build_firmware_project(
                self._default_project(board), catalog, jobs=32,
                build_root=build_root, output_root=output_root,
                runner=fake_runner, timeout=12)
            self.assertRegex(result.build_id,
                             r"^mellow-fly-d5-v1-[0-9a-f]{16}$")
            self.assertEqual(result.record["parallel_jobs"], 32)
            self.assertEqual(result.record["memory"]["ram"]["used_bytes"],
                             1024)
            self.assertEqual(
                result.record["memory"]["flash"]["capacity_bytes"],
                126 * 1024)
            self.assertEqual(
                {item.filename for item in result.artifacts},
                {"studio-project.json", "firmware.config", "build.log", "firmware.elf",
                 "firmware.bin", "firmware.hex", "firmware.map",
                 "build-record.json"})
            artifact = resolve_artifact(
                result.build_id, "firmware.bin", output_root)
            self.assertEqual(artifact.read_bytes(), b"test-bin")
            with self.assertRaises(FirmwareBuildError):
                resolve_artifact(result.build_id, "../firmware.bin",
                                 output_root)

    def test_http_apis_and_mock_state(self):
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "state.json"
            state_path.write_text(json.dumps({
                "schema_version": 1, "board_name": "测试板", "online": True,
                "elapsed_ms": 123, "gpio": [],
                "motion": {"state": "idle", "fault": "none",
                           "queue_depth": 0, "queue_capacity": 1,
                           "axes": []},
                "pwm": [], "ws2812": {"supported": False, "strips": []},
            }), encoding="utf-8")
            artifact_root = Path(directory) / "artifacts"
            artifact_dir = artifact_root / "test-board-0123456789abcdef"
            artifact_dir.mkdir(parents=True)
            (artifact_dir / "firmware.bin").write_bytes(b"test")
            (artifact_dir / "build-record.json").write_text(json.dumps({
                "artifacts": [{"filename": "firmware.bin"}]
            }), encoding="utf-8")
            server = make_server(
                "127.0.0.1", 0, state_path,
                build_output_root=artifact_root)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                base = f"http://127.0.0.1:{server.server_port}"
                state = json.loads(urlopen(base + "/api/state").read())
                self.assertEqual(state["board_name"], "测试板")
                self.assertEqual(state["source"], "mock_mcu")
                catalog = json.loads(urlopen(base + "/api/catalog").read())
                target = json.loads(urlopen(
                    base + "/api/project/target").read())
                self.assertTrue(target["enabled"])
                self.assertEqual(target["mode"], "static-firmware")
                self.assertTrue(target["build_enabled"])
                self.assertEqual(target["parallel_jobs"], 32)
                board = catalog["boards"][0]
                request = Request(
                    base + "/api/project/generate",
                    data=json.dumps({
                        "project": self._default_project(board)
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                generated = json.loads(urlopen(request).read())
                config = base64.b64decode(
                    generated["config_base64"]).decode("utf-8")
                self.assertTrue(generated["ok"])
                self.assertEqual(generated["format"], "KCONFIG")
                self.assertIn("CONFIG_REMOTEBSP_DEVICE_PARAMS=y", config)
                fake_result = FirmwareBuildResult(
                    "test-board-0123456789abcdef", board["id"],
                    "mellow-fly-d5", "0" * 64, Path(directory),
                    (BuildArtifact("firmware.bin", 4, "1" * 64),), {})
                build_request = Request(
                    base + "/api/project/build",
                    data=json.dumps({
                        "project": self._default_project(board)
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                with patch("server.build_firmware_project",
                           return_value=fake_result):
                    built = json.loads(urlopen(build_request).read())
                self.assertTrue(built["ok"])
                self.assertEqual(built["format"], "FIRMWARE_BUILD")
                self.assertEqual(built["artifacts"][0]["filename"],
                                 "firmware.bin")
                downloaded = urlopen(
                    base + "/api/project/artifacts/"
                    "test-board-0123456789abcdef/firmware.bin").read()
                self.assertEqual(downloaded, b"test")
                page = urlopen(base + "/").read()
                for marker in (b"RemoteBSP Studio", b"gpioTable", b"pwmTable",
                               b"stripTable", b"controlGpio", b"controlPwm",
                               b"controlStrips", b"compileConfig",
                               b"buildFirmware"):
                    self.assertIn(marker, page)
                self.assertNotIn(b"deployConfig", page)
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()

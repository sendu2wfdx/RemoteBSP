import base64
import copy
import hashlib
import io
import json
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from unittest.mock import patch

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from firmware_builder import (  # noqa: E402
    BuildArtifact,
    FirmwareBuildError,
    FirmwareBuildResult,
    _vendor_dependency_identity,
    build_firmware_project,
    resolve_artifact,
)
from project_config import ProjectConfigError, generate_project_config  # noqa: E402
from project_contract import (  # noqa: E402
    CURRENT_PROJECT_SCHEMA_VERSION,
    ProjectContractError,
    prepare_project,
)
from server import CATALOG_PATH, make_server  # noqa: E402


class GuiTest(unittest.TestCase):
    @staticmethod
    def _default_project(board):
        return {
            "schema_version": CURRENT_PROJECT_SCHEMA_VERSION,
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
            "i2c": {"buses": [], "devices": []},
            "spi": {"buses": [], "devices": []},
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
                self.assertIn(
                    "#define RBSP_STUDIO_RESOURCE_BOARD_TYPE",
                    result.static_resource_header)
                self.assertIn(
                    f'"{result.project_sha256}"',
                    result.static_resource_header)
                self.assertIn(
                    "#define RBSP_STUDIO_UART_RESOURCE_COUNT",
                    result.static_resource_header)
                self.assertIn(
                    "#define RBSP_STUDIO_PWM_RESOURCE_COUNT",
                    result.static_resource_header)
                self.assertIn(
                    "#define RBSP_STUDIO_TIMED_BITSTREAM_RESOURCE_COUNT",
                    result.static_resource_header)
                self.assertRegex(result.static_resource_sha256,
                                 r"^[0-9a-f]{64}$")
                if board["id"] == "weact-bluepill-plus-v1":
                    self.assertIn(
                        "CONFIG_HARDWARE_UART_RESOURCE_COUNT=3",
                        result.config)
                    self.assertIn("CONFIG_UART1_PINS_PA2_PA3=y",
                                  result.config)
                    self.assertIn("CONFIG_UART2_PINS_PB10_PB11=y",
                                  result.config)
                    self.assertIn(
                        "{0U, 10U, 9U, UINT32_C(300), "
                        "UINT32_C(4500000)}",
                        result.static_resource_header)
                if board["id"] == "weact-g431-core-v10":
                    self.assertIn(
                        "CONFIG_HARDWARE_UART_RESOURCE_COUNT=3",
                        result.config)
                    self.assertIn("CONFIG_UART1_PINS_PA2_PA3=y",
                                  result.config)
                    self.assertIn("CONFIG_UART2_PINS_PB10_PB11=y",
                                  result.config)
                    self.assertIn(
                        "{0U, 38U, UINT32_C(10000)}",
                        result.static_resource_header)

    def test_project_contract_migrates_legacy_and_hashes_canonically(self):
        """旧草案可显式迁移，键顺序和排版不改变工程身份。"""
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        project = self._default_project(catalog["boards"][0])
        reordered = json.loads(json.dumps(project, sort_keys=True))
        self.assertEqual(
            prepare_project(project).sha256,
            prepare_project(reordered).sha256)

        legacy = copy.deepcopy(project)
        legacy.pop("schema_version")
        prepared = prepare_project(legacy)
        self.assertEqual(prepared.original_schema_version, 0)
        self.assertEqual(prepared.schema_version,
                         CURRENT_PROJECT_SCHEMA_VERSION)
        self.assertEqual(prepared.document["schema_version"], 2)
        self.assertEqual(len(prepared.migrations), 2)
        generated = generate_project_config(legacy, catalog)
        self.assertEqual(generated.project_sha256, prepared.sha256)

        future = copy.deepcopy(project)
        future["schema_version"] = CURRENT_PROJECT_SCHEMA_VERSION + 1
        with self.assertRaisesRegex(ProjectContractError, "请升级"):
            prepare_project(future)

    def test_pin_catalog_has_unique_defaults(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.assertEqual(catalog["schema_version"], 3)
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
                self.assertRegex(endpoint["kconfig_symbol"],
                                 r"^[A-Z][A-Z0-9_]*$")
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
                if pwm["backend_status"] == "implemented":
                    self.assertRegex(pwm["kconfig_symbol"],
                                     r"^[A-Z][A-Z0-9_]*$")
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
                if strip["backend_status"] == "implemented":
                    self.assertRegex(strip["kconfig_symbol"],
                                     r"^[A-Z][A-Z0-9_]*$")
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
        project["uart"]["ports"][0]["port"] = 2
        with self.assertRaisesRegex(ProjectConfigError, "端点目录不一致"):
            generate_project_config(project, catalog)

        project = copy.deepcopy(self._default_project(board))
        project["uart"]["ports"][0]["rx_pin"] = "PB7"
        with self.assertRaisesRegex(ProjectConfigError, "端点目录不一致"):
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

    def test_static_waveform_table_uses_catalog_endpoint_and_rejects_spoof(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        board = next(item for item in catalog["boards"]
                     if item["id"] == "mellow-fly-d5-v1")
        project = self._default_project(board)
        project["motion"]["axes"] = []
        pwm = copy.deepcopy(next(
            item for item in board["waveform"]["pwm"]
            if item["backend_status"] == "implemented"))
        strip = copy.deepcopy(next(
            item for item in board["waveform"]["ws2812"]
            if item["backend_status"] == "implemented"))
        project["pwm"]["channels"] = [pwm]
        project["timed_bitstream"]["ws2812"] = [strip]
        result = generate_project_config(project, catalog)
        self.assertIn("#if !defined(CONFIG_PWM0_PIN_PA6)",
                      result.static_resource_header)
        self.assertIn("#if !defined(CONFIG_TIMED_BITSTREAM0_PIN_PA8)",
                      result.static_resource_header)
        self.assertIn("{0U, 6U, UINT32_C(20000)}",
                      result.static_resource_header)
        self.assertIn("{0U, 8U, UINT32_C(192)}",
                      result.static_resource_header)

        broken = copy.deepcopy(project)
        broken["pwm"]["channels"][0]["pin"] = "PA7"
        with self.assertRaisesRegex(ProjectConfigError, "端点目录不一致"):
            generate_project_config(broken, catalog)
        broken = copy.deepcopy(project)
        broken["timed_bitstream"]["ws2812"][0]["channel"] = 1
        with self.assertRaisesRegex(ProjectConfigError, "端点目录不一致"):
            generate_project_config(broken, catalog)

    def test_g431_dual_pwm_project_uses_tim2_and_disables_usart3(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        board = next(item for item in catalog["boards"]
                     if item["id"] == "weact-g431-core-v10")
        project = copy.deepcopy(self._default_project(board))
        project["motion"]["axes"] = []
        project["uart"]["ports"] = project["uart"]["ports"][:2]
        endpoints = {
            item["endpoint_id"]: copy.deepcopy(item)
            for item in board["waveform"]["pwm"]
        }
        project["pwm"]["channels"] = [
            endpoints["tim2_ch3_pb10"], endpoints["tim2_ch4_pb11"]]
        result = generate_project_config(project, catalog)
        self.assertIn("CONFIG_HARDWARE_UART_RESOURCE_COUNT=2", result.config)
        self.assertNotIn("CONFIG_UART2_PINS_PB10_PB11=y", result.config)
        self.assertIn("CONFIG_PWM_RESOURCE_COUNT=2", result.config)
        self.assertIn("CONFIG_PWM0_PIN_PB10=y", result.config)
        self.assertIn("CONFIG_PWM1_PIN_PB11=y", result.config)
        self.assertIn("#define RBSP_STUDIO_PWM_RESOURCE_COUNT 2U",
                      result.static_resource_header)
        self.assertIn("{0U, 26U, UINT32_C(10000)}",
                      result.static_resource_header)
        self.assertIn("{1U, 27U, UINT32_C(10000)}",
                      result.static_resource_header)

        mismatched = copy.deepcopy(project)
        mismatched["pwm"]["channels"][1]["frequency_hz"] = 20_000
        with self.assertRaisesRegex(ProjectConfigError, "必须使用相同频率"):
            generate_project_config(mismatched, catalog)

        non_contiguous = copy.deepcopy(project)
        non_contiguous["pwm"]["channels"] = [
            non_contiguous["pwm"]["channels"][1]]
        with self.assertRaisesRegex(ProjectConfigError, "从0开始连续"):
            generate_project_config(non_contiguous, catalog)

        incomplete = copy.deepcopy(project)
        incomplete["pwm"]["channels"] = [
            incomplete["pwm"]["channels"][0]]
        with self.assertRaisesRegex(ProjectConfigError, "必须同时启用"):
            generate_project_config(incomplete, catalog)

    def test_g431_pa6_single_pwm_project(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        board = next(item for item in catalog["boards"]
                     if item["id"] == "weact-g431-core-v10")
        project = copy.deepcopy(self._default_project(board))
        endpoint = next(
            copy.deepcopy(item) for item in board["waveform"]["pwm"]
            if item["endpoint_id"] == "tim3_ch1_pa6")
        project["pwm"]["channels"] = [endpoint]
        result = generate_project_config(project, catalog)
        self.assertIn("CONFIG_PWM_RESOURCE_COUNT=1", result.config)
        self.assertIn("CONFIG_PWM0_PIN_PA6=y", result.config)
        self.assertIn("CONFIG_HARDWARE_UART_RESOURCE_COUNT=3", result.config)
        self.assertIn("{0U, 6U, UINT32_C(1000)}",
                      result.static_resource_header)

    def test_static_gpio_table_is_deterministic_and_detects_drift(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        board = next(item for item in catalog["boards"]
                     if item["id"] == "weact-bluepill-plus-v1")
        project = self._default_project(board)
        project["motion"]["axes"] = []
        project["uart"]["ports"] = []
        project["gpio"]["resources"] = [{
            "name": "input", "pin": "PA0", "direction": "input",
            "pull": "down", "active_low": False, "safe_level": None,
            "debounce_ms": 10,
        }]
        first = generate_project_config(project, catalog)
        reordered = json.loads(json.dumps(project, sort_keys=True))
        second = generate_project_config(reordered, catalog)
        self.assertEqual(first.static_resource_header,
                         second.static_resource_header)
        self.assertEqual(first.static_resource_sha256,
                         second.static_resource_sha256)
        self.assertIn("{0U, RBSP_STARTUP_GPIO_INPUT_PULLDOWN}",
                      first.static_resource_header)

        changed = copy.deepcopy(project)
        changed["gpio"]["resources"][0]["debounce_ms"] = 11
        third = generate_project_config(changed, catalog)
        self.assertNotEqual(first.static_resource_sha256,
                            third.static_resource_sha256)

        broken = copy.deepcopy(project)
        broken["gpio"]["resources"][0]["pin"] = "PA13"
        with self.assertRaisesRegex(ProjectConfigError, "保留引脚"):
            generate_project_config(broken, catalog)

        broken = copy.deepcopy(project)
        broken["gpio"]["resources"][0]["safe_level"] = False
        with self.assertRaisesRegex(ProjectConfigError, "不能设置safe_level"):
            generate_project_config(broken, catalog)

        broken = copy.deepcopy(project)
        broken["gpio"]["resources"][0]["debounce_ms"] = True
        with self.assertRaisesRegex(ProjectConfigError, "debounce_ms"):
            generate_project_config(broken, catalog)

    def test_firmware_builder_archives_bounded_artifacts(self):
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        board = catalog["boards"][0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_root = root / "build"
            output_root = root / "out"

            def fake_runner(command, timeout):
                self.assertEqual(timeout, 12)
                if "--build" not in command:
                    resource_option = next(
                        item for item in command
                        if item.startswith("-DRBSP_STATIC_RESOURCE_TABLE="))
                    resource_path = Path(resource_option.split("=", 1)[1])
                    self.assertTrue(resource_path.is_file())
                    identity_option = next(
                        item for item in command
                        if item.startswith(
                            "-DRBSP_FIRMWARE_INPUT_SHA256="))
                    self.assertRegex(identity_option.split("=", 1)[1],
                                     r"^[0-9a-f]{64}$")
                    self.assertIn("RBSP_STUDIO_GPIO_RESOURCE_COUNT",
                                  resource_path.read_text(encoding="utf-8"))
                if "--build" in command:
                    build_dir = Path(command[2])
                    for suffix in ("elf", "bin", "hex", "map"):
                        (build_dir / f"remotebsp-stm32f072rbt6.{suffix}").write_bytes(
                            ("test-" + suffix).encode())
                return ("测试构建输出\n"
                        "Memory region         Used Size  Region Size  %age Used\n"
                        "RAM: 1024 B 20 KB 5.00%\n"
                        "FLASH: 4096 B 126 KB 3.17%\n")

            with patch("firmware_builder._firmware_source_sha256",
                       side_effect=["e" * 64, "e" * 64,
                                    "f" * 64, "f" * 64]):
                result = build_firmware_project(
                    self._default_project(board), catalog, jobs=32,
                    build_root=build_root, output_root=output_root,
                    runner=fake_runner, timeout=12)
                other_source = build_firmware_project(
                    self._default_project(board), catalog, jobs=32,
                    build_root=root / "other-build",
                    output_root=root / "other-out",
                    runner=fake_runner, timeout=12)
            self.assertNotEqual(result.build_id, other_source.build_id)
            self.assertRegex(result.build_id,
                             r"^mellow-fly-d5-v1-[0-9a-f]{16}$")
            self.assertEqual(result.record["parallel_jobs"], 32)
            self.assertEqual(result.record["project_schema_version"], 2)
            self.assertEqual(result.record["project_migrations"], [])
            self.assertEqual(result.record["project_summary"][
                "resource_count"], result.record["resource_count"])
            self.assertRegex(result.record["project_sha256"],
                             r"^[0-9a-f]{64}$")
            self.assertRegex(result.record["static_resource_sha256"],
                             r"^[0-9a-f]{64}$")
            self.assertRegex(result.record["firmware_input_sha256"],
                             r"^[0-9a-f]{64}$")
            self.assertEqual(result.record["project_sha256"],
                             next(item.sha256 for item in result.artifacts
                                  if item.filename ==
                                  "studio-project.json"))
            self.assertEqual(result.record["memory"]["ram"]["used_bytes"],
                             1024)
            self.assertEqual(
                result.record["memory"]["flash"]["capacity_bytes"],
                126 * 1024)
            self.assertEqual(
                {item.filename for item in result.artifacts},
                {"studio-project.json", "firmware.config",
                 "remotebsp_static_resources.h", "build.log",
                 "firmware.elf", "firmware.bin", "firmware.hex",
                 "firmware.map", "build-record.json"})
            artifact = resolve_artifact(
                result.build_id, "firmware.bin", output_root)
            self.assertEqual(artifact.read_bytes(), b"test-bin")
            artifact.write_bytes(b"tampered")
            with self.assertRaisesRegex(FirmwareBuildError, "SHA-256"):
                resolve_artifact(result.build_id, "firmware.bin",
                                 output_root)
            artifact.unlink()
            outside = root / "outside.bin"
            outside.write_bytes(b"test-bin")
            artifact.symlink_to(outside)
            with self.assertRaises(FirmwareBuildError):
                resolve_artifact(result.build_id, "firmware.bin",
                                 output_root)
            with self.assertRaises(FirmwareBuildError):
                resolve_artifact(result.build_id, "../firmware.bin",
                                 output_root)

    def test_vendor_dependency_identity_captures_index_and_untracked_content(self):
        revision = SimpleNamespace(stdout=b"a" * 40, returncode=0)
        clean = SimpleNamespace(stdout=b"", returncode=0)
        with tempfile.TemporaryDirectory() as directory:
            dependency = Path(directory) / "cmsis-core"
            source = dependency / "Core" / "Include" / "new.h"
            source.parent.mkdir(parents=True)
            source.write_bytes(b"#define VALUE 1\n")
            with patch("firmware_builder.subprocess.run",
                       side_effect=[revision, clean, clean, clean]):
                base = _vendor_dependency_identity(dependency)
            staged = SimpleNamespace(stdout=b":100644 100644 a b M\0",
                                     returncode=0)
            with patch("firmware_builder.subprocess.run",
                       side_effect=[revision, staged, clean, clean]):
                changed_index = _vendor_dependency_identity(dependency)
            untracked = SimpleNamespace(
                stdout=b"Core/Include/new.h\0", returncode=0)
            with patch("firmware_builder.subprocess.run",
                       side_effect=[revision, clean, untracked, clean]):
                changed_worktree = _vendor_dependency_identity(dependency)
            self.assertNotEqual(base, changed_index)
            self.assertNotEqual(base, changed_worktree)

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
                "build_id": "test-board-0123456789abcdef",
                "artifacts": [{
                    "filename": "firmware.bin", "size": 4,
                    "sha256": hashlib.sha256(b"test").hexdigest(),
                }]
            }), encoding="utf-8")
            server = make_server(
                "127.0.0.1", 0, state_path,
                build_output_root=artifact_root,
                history_root=Path(directory) / "history")
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
                self.assertTrue(target["project_reports_enabled"])
                self.assertTrue(target["project_compare_enabled"])
                self.assertTrue(target["project_comparison_export_enabled"])
                self.assertTrue(target["production_record_enabled"])
                self.assertTrue(target["production_batch_enabled"])
                self.assertTrue(target["production_history_enabled"])
                self.assertFalse(target["stlink_deployment_enabled"])
                self.assertFalse(target["device_parameter_write_enabled"])
                self.assertEqual(target["parallel_jobs"], 32)
                self.assertEqual(target["project_schema_version"], 2)
                self.assertFalse(target["runtime_control_enabled"])
                board = catalog["boards"][0]
                inspect_request = Request(
                    base + "/api/project/inspect",
                    data=json.dumps({
                        "project": self._default_project(board)
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                inspected = json.loads(urlopen(inspect_request).read())
                self.assertEqual(inspected["format"], "PROJECT_INSPECTION")
                self.assertEqual(inspected["project_schema_version"], 2)
                self.assertRegex(inspected["project_sha256"],
                                 r"^[0-9a-f]{64}$")
                self.assertEqual(inspected["summary"]["resource_count"],
                                 inspected["resource_count"])
                validate_request = Request(
                    base + "/api/project/validate",
                    data=json.dumps({
                        "project": self._default_project(board)
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                validated = json.loads(urlopen(validate_request).read())
                self.assertTrue(validated["ok"])
                self.assertEqual(validated["format"], "PROJECT_VALIDATION")
                self.assertEqual(validated["resource_count"],
                                 inspected["resource_count"])
                reports_request = Request(
                    base + "/api/project/generate-reports",
                    data=json.dumps({
                        "project": self._default_project(board)
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                reports = json.loads(urlopen(reports_request).read())
                self.assertTrue(reports["ok"])
                self.assertEqual(reports["format"], "PROJECT_REPORTS_V1")
                self.assertEqual(reports["project_sha256"],
                                 inspected["project_sha256"])
                self.assertRegex(reports["resource_set_sha256"],
                                 r"^[0-9a-f]{64}$")
                with zipfile.ZipFile(io.BytesIO(base64.b64decode(
                        reports["archive_base64"]))) as archive:
                    self.assertIn("SHA256SUMS", archive.namelist())
                    self.assertTrue(any(
                        name.endswith("接线表.md")
                        for name in archive.namelist()))
                compare_request = Request(
                    base + "/api/project/compare",
                    data=json.dumps({
                        "left": self._default_project(board),
                        "right": self._default_project(board),
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                comparison = json.loads(urlopen(compare_request).read())
                self.assertTrue(comparison["ok"])
                self.assertTrue(comparison["equal"])
                self.assertEqual(comparison["format"],
                                 "PROJECT_COMPARISON_V1")
                export_comparison_request = Request(
                    base + "/api/project/export-comparison",
                    data=json.dumps({
                        "left": self._default_project(board),
                        "right": self._default_project(board),
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                comparison_export = json.loads(urlopen(
                    export_comparison_request).read())
                self.assertEqual(comparison_export["format"],
                                 "PROJECT_COMPARISON_EXPORT_V1")
                self.assertEqual(comparison_export["comparison"], comparison)
                with zipfile.ZipFile(io.BytesIO(base64.b64decode(
                        comparison_export["archive_base64"]))) as archive:
                    self.assertIn("SHA256SUMS", archive.namelist())
                    self.assertTrue(any(name.endswith("工程差异-v1.json")
                                        for name in archive.namelist()))
                    self.assertTrue(any(name.endswith("工程差异报告-v1.md")
                                        for name in archive.namelist()))
                production_request = Request(
                    base + "/api/project/generate-production-record",
                    data=json.dumps({
                        "project": self._default_project(board),
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                production = json.loads(urlopen(production_request).read())
                self.assertEqual(production["format"],
                                 "PRODUCTION_RECORD_V1")
                self.assertEqual(production["status"], "design_only")
                production_document = json.loads(base64.b64decode(
                    production["record_base64"]))
                self.assertEqual(production_document["execution_status"]
                                 ["firmware_flash"], "not_performed")
                batch_request = Request(
                    base + "/api/production-batch/export",
                    data=json.dumps({
                        "batch_id": "http-smoke-001",
                        "name": "HTTP批次冒烟测试",
                        "note": "纯软件",
                        "production_records": [{
                            "record": production["record"],
                            "record_sha256": production["record_sha256"],
                        }],
                        "comparison_exports": [],
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                batch = json.loads(urlopen(batch_request).read())
                self.assertEqual(batch["format"],
                                 "PRODUCTION_BATCH_EXPORT_V1")
                self.assertTrue(batch["validation"]["valid"])
                with zipfile.ZipFile(io.BytesIO(base64.b64decode(
                        batch["archive_base64"]))) as archive:
                    self.assertIn("SHA256SUMS", archive.namelist())
                    self.assertTrue(any(name.endswith("生产批次清单-v1.json")
                                        for name in archive.namelist()))
                batch_validation_request = Request(
                    base + "/api/production-batch/validate",
                    data=json.dumps({"manifest": batch["manifest"]}).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                batch_validation = json.loads(urlopen(
                    batch_validation_request).read())
                self.assertTrue(batch_validation["valid"])
                history_save_request = Request(
                    base + "/api/production-history/save",
                    data=json.dumps({"manifest": batch["manifest"]}).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                history_saved = json.loads(urlopen(
                    history_save_request).read())
                self.assertTrue(history_saved["stored"])
                history_status = json.loads(urlopen(
                    base + "/api/production-history/status").read())
                self.assertEqual(history_status["valid_record_count"], 1)
                history_search = json.loads(urlopen(
                    base + "/api/production-history/search?field=board_id&"
                    "query=" + board["id"]).read())
                self.assertEqual(history_search["total_matches"], 1)
                for invalid_query in (
                        "query=a&query=b", "field=all&unknown=value",
                        "limit=1&limit=2"):
                    with self.subTest(invalid_query=invalid_query):
                        with self.assertRaises(HTTPError) as caught:
                            urlopen(base + "/api/production-history/search?" +
                                    invalid_query)
                        self.assertEqual(caught.exception.code, 400)
                history_record = json.loads(urlopen(
                    base + "/api/production-history/record/" +
                    batch["manifest_sha256"]).read())
                self.assertEqual(history_record["manifest"],
                                 batch["manifest"])
                linked_production_request = Request(
                    base + "/api/project/generate-production-record",
                    data=json.dumps({
                        "project": self._default_project(board),
                        "build_id": "test-board-0123456789abcdef",
                    }).encode(),
                    headers={"Content-Type": "application/json"},
                    method="POST")
                linked_production = json.loads(urlopen(
                    linked_production_request).read())
                self.assertEqual(linked_production["status"],
                                 "build_incomplete")
                self.assertGreater(len(linked_production["record"]
                                       ["missing_or_invalid_fields"]), 0)
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
                self.assertEqual(generated["project_sha256"],
                                 inspected["project_sha256"])
                self.assertRegex(generated["config_sha256"],
                                 r"^[0-9a-f]{64}$")
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
                               b"i2cBusTable", b"i2cDeviceTable",
                               b"spiBusTable", b"spiDeviceTable",
                               b"stripTable", b"controlGpio", b"controlPwm",
                               b"controlStrips", b"compileConfig",
                               b"buildFirmware", b"exportReports",
                               b"reportResult", b"compareProjects",
                               b"compareLeftFile", b"compareRightFile",
                               b"compareResult", b"productionBuildId",
                               b"exportProductionRecord",
                               b"productionResult", b"exportComparison"):
                    self.assertIn(marker, page)
                for marker in (b"exportProductionBatch",
                               b"validateProductionBatch",
                               b"batchManifestFile", b"batchResult"):
                    self.assertIn(marker, page)
                for marker in (b"saveProductionHistory",
                               b"searchProductionHistory",
                               b"historyField", b"historyStatus",
                               b"historyResults"):
                    self.assertIn(marker, page)
                for marker in (b"deployBuildId", b"deployUuid",
                               b"deployConfirmation", b"deployPreflight",
                               b"deployExecute", b"deployResult"):
                    self.assertIn(marker, page)
                for marker in (b"parameterUuid", b"parameterGeneration",
                               b"parameterId", b"parameterValue",
                               b"parameterBackup", b"parameterWritePreflight",
                               b"parameterRestorePreflight",
                               b"parameterExecute", b"parameterWriteResult"):
                    self.assertIn(marker, page)
                for marker in (b"runtimePwmNode", b"runtimePwmResource",
                               b"runtimePwmLease", b"runtimePwmIdempotency",
                               b"runtimePwmFrequency", b"runtimePwmDuty",
                               b"runtimePwmConfigure", b"runtimePwmStop",
                               b"runtimePwmRefresh", b"runtimePwmStatus"):
                    self.assertIn(marker, page)
                target = json.loads(urlopen(
                    base + "/api/project/target").read())
                self.assertFalse(target["runtime_pwm"]["available"])
                self.assertFalse(target["runtime_pwm"]["auth_proxy"])
                self.assertEqual(
                    target["runtime_pwm"]["reason"],
                    "runtime_pwm_auth_proxy_unconfigured")
                self.assertEqual(target["runtime_pwm"]["contract"], {
                    "configure": "POST /api/v1/control/pwm/configure",
                    "stop": "POST /api/v1/control/pwm/stop",
                    "snapshot": "GET /api/v1/snapshot",
                    "result": "data.operation.result",
                })
                script = urlopen(base + "/app.js").read()
                self.assertIn(b"/api/deployment/preflight", script)
                self.assertIn(b"/api/deployment/execute", script)
                self.assertIn(
                    b"/api/device-parameters/write-preflight", script)
                self.assertIn(
                    b"/api/device-parameters/restore-preflight", script)
                self.assertIn(b"snapshot_path", script)
                self.assertIn(b"auth_proxy", script)
                self.assertIn(b"data.operation.result", script)
                self.assertNotIn(b"status_path", script)
                self.assertNotIn(b"deployConfig", page)
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()

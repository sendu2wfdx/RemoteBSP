import hashlib
import itertools
import json
import sys
import tempfile
import unittest
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from firmware_deployment import (  # noqa: E402
    DeviceIdentity, FirmwareDeploymentError, deploy_can_katapult, deploy_stlink,
    IdentityCapabilityError, JsonIdentityFileReader, ToolbusdIdentityReader,
    expected_identity, make_can_katapult_plan, make_stlink_plan)


class Reader:
    def __init__(self, values):
        self.values = iter(values)

    def read_identity(self):
        value = next(self.values)
        if isinstance(value, Exception):
            raise value
        return value


class FirmwareDeploymentTest(unittest.TestCase):
    @staticmethod
    def _node_list(*, uuid="ab" * 16, node_id=7, online=True,
                   ready=True, board_type=0x0431CB):
        return json.dumps({
            "schema_version": 1, "command": "node-list", "data": {
                "nodes": [{
                    "node_id": node_id, "online": online, "ready": ready,
                    "board_type": board_type,
                    "firmware": {"major": 0, "minor": 2, "patch": 0},
                    "protocol_version": 1, "uuid": uuid,
                }],
            },
        }).encode()

    @staticmethod
    def _firmware_identity(*, uuid="ab" * 16, board_type=0x0431CB,
                           project="a" * 64, config="b" * 64,
                           firmware_input="d" * 64):
        return json.dumps({
            "schema_version": 1, "command": "firmware-identity",
            "identity_schema_version": 1, "board_type": board_type,
            "uuid": uuid, "project_sha256": project,
            "config_sha256": config,
            "firmware_input_sha256": firmware_input,
        }).encode()

    def test_toolbusd_reader_strictly_selects_runtime_node(self):
        calls = []

        def runner(command, timeout, maximum):
            calls.append((tuple(command), timeout, maximum))
            return self._node_list()

        reader = ToolbusdIdentityReader(
            "/tmp/toolbusd.sock", 7, expected_uuid="AB" * 16,
            remote_cli="/opt/remotebsp/remote-cli", timeout=1.25,
            runner=runner)
        node = reader.read_runtime_node()
        self.assertEqual(node.board_id, "weact-g431-core-v10")
        self.assertEqual(node.device_uuid, "ab" * 16)
        self.assertTrue(node.online and node.ready)
        self.assertEqual(node.firmware_version, (0, 2, 0))
        self.assertEqual(node.protocol_version, 1)
        self.assertEqual(calls[0][0], (
            "/opt/remotebsp/remote-cli", "--json", "--socket",
            "/tmp/toolbusd.sock", "node-list"))
        self.assertEqual(calls[0][1:], (1.25, 64 * 1024))

    def test_toolbusd_reader_rejects_ambiguous_or_untrusted_identity(self):
        cases = (
            (self._node_list(node_id=8), "不存在"),
            (self._node_list(online=False), "尚未在线"),
            (self._node_list(board_type=123), "不受Studio支持"),
            (b'{"schema_version":1,"schema_version":1}', "重复字段"),
            (b"x" * (64 * 1024 + 1), "64 KiB"),
        )
        for output, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(
                    FirmwareDeploymentError, message):
                ToolbusdIdentityReader(
                    "/tmp/toolbusd.sock", 7,
                    runner=lambda *_args, value=output: value
                ).read_runtime_node()
        with self.assertRaisesRegex(FirmwareDeploymentError, "UUID"):
            ToolbusdIdentityReader(
                "/tmp/toolbusd.sock", 7, expected_uuid="cd" * 16,
                runner=lambda *_: self._node_list()).read_runtime_node()

    def test_toolbusd_incomplete_contract_fails_before_flash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_id = self._build(root)
            flashed = []
            reader = ToolbusdIdentityReader(
                "/tmp/toolbusd.sock", 7,
                runner=lambda command, *_: (
                    self._node_list() if command[-1] == "node-list" else
                    self._firmware_identity(project=None)))
            with self.assertRaisesRegex(IdentityCapabilityError,
                                        "未提供project_sha256"):
                deploy_stlink(
                    build_id, reader, output_root=root,
                    runner=lambda *_: flashed.append(True))
            self.assertEqual(flashed, [])

    def test_toolbusd_complete_identity_merges_same_node(self):
        calls = []

        def runner(command, *_):
            calls.append(tuple(command))
            return (self._node_list() if command[-1] == "node-list" else
                    self._firmware_identity())

        observed = ToolbusdIdentityReader(
            "/tmp/toolbusd.sock", 7, runner=runner).read_identity()
        self.assertEqual(observed, DeviceIdentity(
            "weact-g431-core-v10", "a" * 64, "b" * 64,
            "d" * 64, "ab" * 16))
        self.assertEqual(calls[1][-3:],
                         ("--node", "7", "firmware-identity"))

    def test_toolbusd_identity_rejects_node_change(self):
        def runner(command, *_):
            return (self._node_list() if command[-1] == "node-list" else
                    self._firmware_identity(uuid="cd" * 16))

        with self.assertRaisesRegex(FirmwareDeploymentError, "同一运行中节点"):
            ToolbusdIdentityReader(
                "/tmp/toolbusd.sock", 7, runner=runner).read_identity()

    def test_toolbusd_complete_identity_allows_flash_and_recheck(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_id = self._build(root)
            identity_calls = []
            flashed = []

            def identity_runner(command, *_):
                identity_calls.append(tuple(command))
                return (self._node_list() if command[-1] == "node-list" else
                        self._firmware_identity())

            result = deploy_stlink(
                build_id,
                ToolbusdIdentityReader(
                    "/tmp/toolbusd.sock", 7, runner=identity_runner),
                output_root=root,
                runner=lambda command, timeout: flashed.append(
                    (tuple(command), timeout)), sleeper=lambda _: None)
            self.assertTrue(result.verified)
            self.assertEqual(len(flashed), 1)
            self.assertEqual(len(identity_calls), 4)

    def test_json_identity_reader_is_strict_and_bounded(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "identity.json"
            identity = {
                "board_id": "weact-g431-core-v10",
                "project_sha256": "a" * 64,
                "config_sha256": "b" * 64,
                "firmware_identity_sha256": "c" * 64,
                "device_uuid": "ab" * 16,
            }
            path.write_text(json.dumps(identity), encoding="utf-8")
            observed = JsonIdentityFileReader(path).read_identity()
            self.assertEqual(observed.device_uuid, "ab" * 16)
            path.write_text('{"board_id":"x","board_id":"y"}',
                            encoding="utf-8")
            with self.assertRaisesRegex(FirmwareDeploymentError, "重复字段"):
                JsonIdentityFileReader(path).read_identity()
            path.write_bytes(b" " * (JsonIdentityFileReader.MAX_BYTES + 1))
            with self.assertRaisesRegex(FirmwareDeploymentError, "16 KiB"):
                JsonIdentityFileReader(path).read_identity()

    def test_non_finite_wait_is_rejected_before_flash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_id = self._build(root)
            called = []
            with self.assertRaisesRegex(FirmwareDeploymentError,
                                        "重连等待参数无效"):
                deploy_stlink(
                    build_id, Reader([]), output_root=root,
                    reconnect_timeout=float("nan"),
                    runner=lambda command, timeout: called.append(command))
            self.assertEqual(called, [])

    def _build(self, root: Path):
        build_id = "weact-test-01234567"
        directory = root / build_id
        directory.mkdir()
        files = {"firmware.bin": b"binary", "firmware.elf": b"elf",
                 "firmware.config": b"CONFIG_APP_LAYOUT_KATAPULT_8K=y\n"}
        artifacts = []
        for name, content in files.items():
            (directory / name).write_bytes(content)
            artifacts.append({"filename": name, "size": len(content),
                              "sha256": hashlib.sha256(content).hexdigest()})
        record = {
            "schema_version": 1, "build_id": build_id,
            "board_id": "weact-g431-core-v10",
            "project_sha256": "a" * 64, "config_sha256": "b" * 64,
            "firmware_input_sha256": "d" * 64,
            "artifacts": artifacts,
        }
        (directory / "build-record.json").write_text(
            json.dumps(record), encoding="utf-8")
        return build_id

    def test_plan_uses_verified_artifact_and_no_shell_tokens(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            plan = make_stlink_plan(build_id, output_root=root,
                                    probe_serial="ABC-123")
            self.assertEqual(plan.backend, "stlink-openocd")
            self.assertIn("target/stm32g4x.cfg", plan.command)
            self.assertIn("verify reset exit", plan.command[-1])
            self.assertNotIn(";", "".join(plan.command))

    def test_rejects_probe_serial_injection(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            with self.assertRaisesRegex(FirmwareDeploymentError, "序列号"):
                make_stlink_plan(build_id, output_root=root,
                                 probe_serial="x; shutdown")

    def test_can_katapult_plan_is_targeted_and_uses_verified_bin(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            flashtool = root / "flashtool.py"
            flashtool.write_text("# test", encoding="utf-8")
            plan = make_can_katapult_plan(
                build_id, output_root=root, can_interface="can0",
                katapult_uuid="A1b2C3d4e5f6", flashtool=flashtool)
            self.assertEqual(plan.backend, "can-katapult")
            self.assertEqual(plan.command[-6:], (
                "-i", "can0", "-u", "a1b2c3d4e5f6", "-f",
                str(root / build_id / "firmware.bin")))

    def test_can_katapult_rejects_broadcast_or_injection_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            flashtool = root / "flashtool.py"
            flashtool.write_text("# test", encoding="utf-8")
            for interface, uuid in (("can0;id", "abcdef"),
                                    ("can0", ""), ("can0", "all")):
                with self.subTest(interface=interface, uuid=uuid), \
                        self.assertRaises(FirmwareDeploymentError):
                    make_can_katapult_plan(
                        build_id, output_root=root, can_interface=interface,
                        katapult_uuid=uuid, flashtool=flashtool)

    def test_can_katapult_rejects_non_bootloader_layout(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            config = root / build_id / "firmware.config"
            content = b"CONFIG_APP_LAYOUT_KATAPULT_8K=n\n"
            config.write_bytes(content)
            record_path = root / build_id / "build-record.json"
            record = json.loads(record_path.read_text(encoding="utf-8"))
            item = next(item for item in record["artifacts"]
                        if item["filename"] == "firmware.config")
            item.update(size=len(content),
                        sha256=hashlib.sha256(content).hexdigest())
            record_path.write_text(json.dumps(record), encoding="utf-8")
            flashtool = root / "flashtool.py"
            flashtool.write_text("# test", encoding="utf-8")
            with self.assertRaisesRegex(FirmwareDeploymentError, "8 KiB"):
                make_can_katapult_plan(
                    build_id, output_root=root, can_interface="can0",
                    katapult_uuid="abcdef", flashtool=flashtool)

    def test_can_katapult_deploy_reuses_four_way_identity_check(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            flashtool = root / "flashtool.py"
            flashtool.write_text("# test", encoding="utf-8")
            expected = expected_identity(build_id, output_root=root)
            observed = DeviceIdentity(
                expected.board_id, expected.project_sha256,
                expected.config_sha256, expected.firmware_identity_sha256,
                "ab" * 16)
            calls = []
            result = deploy_can_katapult(
                build_id, Reader([observed]), output_root=root,
                can_interface="can0", katapult_uuid="abcdef123456",
                flashtool=flashtool,
                runner=lambda command, timeout: calls.append(tuple(command)),
                sleeper=lambda _: None)
            self.assertTrue(result.verified)
            self.assertEqual(result.backend, "can-katapult")
            self.assertEqual(len(calls), 1)

    def test_tampered_firmware_is_rejected_before_flash(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            (root / build_id / "firmware.elf").write_bytes(b"tampered")
            with self.assertRaisesRegex(FirmwareDeploymentError, "不可用于部署"):
                make_stlink_plan(build_id, output_root=root)

    def test_flash_retry_and_four_way_identity_verification(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            expected = expected_identity(build_id, output_root=root)
            observed = DeviceIdentity(
                expected.board_id, expected.project_sha256,
                expected.config_sha256, expected.firmware_identity_sha256,
                "ab" * 16)
            commands = []
            result = deploy_stlink(
                build_id, Reader([TimeoutError("booting"), observed]),
                output_root=root, runner=lambda command, timeout:
                commands.append((tuple(command), timeout)), sleeper=lambda _: None)
            self.assertTrue(result.verified)
            self.assertEqual(result.attempts, 2)
            self.assertEqual(len(commands), 1)

    def test_identity_mismatch_never_becomes_success(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build_id = self._build(root)
            expected = expected_identity(build_id, output_root=root)
            bad = DeviceIdentity(
                expected.board_id, expected.project_sha256, "c" * 64,
                expected.firmware_identity_sha256, "ab" * 16)
            with self.assertRaisesRegex(FirmwareDeploymentError, "身份核对"):
                deploy_stlink(
                    build_id, Reader(itertools.repeat(bad)), output_root=root,
                    reconnect_timeout=.01, poll_interval=.001,
                    runner=lambda command, timeout: None, sleeper=lambda _: None)


if __name__ == "__main__":
    unittest.main()

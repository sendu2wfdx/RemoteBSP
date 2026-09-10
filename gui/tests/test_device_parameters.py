import base64
import sys
import json
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from device_parameters import (  # noqa: E402
    DeviceParameterError, DeviceParameterManager, WRITE_CONFIRMATION)
from server import make_server  # noqa: E402


UUID = "ab" * 16


class FakeRunner:
    def __init__(self):
        self.generation = 4
        self.value = b"old"
        self.commands = []

    def __call__(self, command, timeout, maximum):
        self.commands.append(tuple(command))
        name = next(item for item in (
            "node-list", "param-status", "param-list", "param-get",
            "param-set-cas", "param-set")
                    if item in command)
        if name == "node-list":
            return json.dumps({
                "schema_version": 1, "command": "node-list", "data": {
                    "nodes": [{"node_id": 7, "online": True, "ready": True,
                               "board_type": 274891,
                               "firmware": {"major": 0, "minor": 2, "patch": 0},
                               "protocol_version": 1, "uuid": UUID}]}
            }, separators=(",", ":")).encode()
        if name == "param-status":
            return (f"version=1 generation={self.generation} stored=1 definitions=1 "
                    "maintenance_unlocked=no restart_required=no store_error=0\n").encode()
        if name == "param-list":
            return b"id=0x100 name=device-name type=2 flags=0x12 length=0..32\n"
        if name == "param-get":
            return (f"id=0x100 name=device-name generation={self.generation} "
                    f"type=2 value={self.value.decode()}\n").encode()
        if name in ("param-set", "param-set-cas"):
            if name == "param-set-cas" and int(command[-2]) != self.generation:
                raise DeviceParameterError("远端操作失败，状态码 4")
            self.value = bytes.fromhex(command[-1][4:])
            self.generation += 1
            return (f"version=1 generation={self.generation} stored=1 definitions=1 "
                    "maintenance_unlocked=no restart_required=no store_error=0\n").encode()
        raise AssertionError(command)


class DeviceParameterTests(unittest.TestCase):
    def manager(self, runner):
        return DeviceParameterManager("/tmp/toolbusd.sock", 7, runner=runner)

    def test_snapshot_is_bounded_and_backup_ready(self):
        result = self.manager(FakeRunner()).snapshot()
        self.assertEqual(result["node_uuid"], UUID)
        self.assertEqual(result["parameters"][0]["text"], "old")
        self.assertEqual(base64.b64decode(
            result["parameters"][0]["value_base64"]), b"old")

    def test_backup_v2_detects_tampering_and_v1_remains_restorable(self):
        runner = FakeRunner()
        manager = self.manager(runner)
        backup = manager.backup()
        self.assertEqual(backup["schema_version"], 2)
        self.assertEqual(len(backup["sha256"]), 64)
        changed = json.loads(json.dumps(backup))
        changed["parameters"][0]["value_base64"] = base64.b64encode(b"bad").decode()
        with self.assertRaisesRegex(DeviceParameterError, "完整性"):
            manager.restore(changed, expected_uuid=UUID, expected_generation=4,
                            confirmation=WRITE_CONFIRMATION)
        legacy = manager.snapshot()
        manager.restore(legacy, expected_uuid=UUID, expected_generation=4,
                        confirmation=WRITE_CONFIRMATION)

    def test_write_requires_confirmation_uuid_and_generation(self):
        runner = FakeRunner()
        manager = self.manager(runner)
        value = base64.b64encode(b"new").decode()
        with self.assertRaisesRegex(DeviceParameterError, "显式维护确认"):
            manager.write(expected_uuid=UUID, expected_generation=4,
                          parameter_id=0x100, value_base64=value,
                          confirmation="yes")
        with self.assertRaisesRegex(DeviceParameterError, "UUID"):
            manager.write(expected_uuid="cd" * 16, expected_generation=4,
                          parameter_id=0x100, value_base64=value,
                          confirmation=WRITE_CONFIRMATION)
        with self.assertRaisesRegex(DeviceParameterError, "代数"):
            manager.write(expected_uuid=UUID, expected_generation=3,
                          parameter_id=0x100, value_base64=value,
                          confirmation=WRITE_CONFIRMATION)
        result = manager.write(
            expected_uuid=UUID, expected_generation=4, parameter_id=0x100,
            value_base64=value, confirmation=WRITE_CONFIRMATION)
        self.assertEqual(result["status"]["generation"], 5)
        self.assertEqual(result["parameters"][0]["text"], "new")

    def test_restore_rejects_wrong_target_and_restores(self):
        runner = FakeRunner()
        manager = self.manager(runner)
        backup = manager.snapshot()
        runner.value = b"new"
        runner.generation = 5
        with self.assertRaisesRegex(DeviceParameterError, "目标UUID"):
            manager.restore({**backup, "node_uuid": "cd" * 16},
                            expected_uuid=UUID, expected_generation=5,
                            confirmation=WRITE_CONFIRMATION)
        restored = manager.restore(
            backup, expected_uuid=UUID, expected_generation=5,
            confirmation=WRITE_CONFIRMATION)
        self.assertEqual(restored["parameters"][0]["text"], "old")
        self.assertEqual(restored["status"]["generation"], 6)

    def test_restore_skips_unchanged_values(self):
        runner = FakeRunner()
        manager = self.manager(runner)
        backup = manager.snapshot()
        restored = manager.restore(
            backup, expected_uuid=UUID, expected_generation=4,
            confirmation=WRITE_CONFIRMATION)
        self.assertEqual(restored["status"]["generation"], 4)
        self.assertFalse(any(any(item.startswith("param-set") for item in command)
                             for command in runner.commands))

    def test_generation_change_at_write_boundary_is_rejected(self):
        class RacingRunner(FakeRunner):
            def __call__(self, command, timeout, maximum):
                if "param-set-cas" in command:
                    self.generation += 1
                return super().__call__(command, timeout, maximum)

        runner = RacingRunner()
        with self.assertRaisesRegex(DeviceParameterError, "状态码"):
            self.manager(runner).write(
                expected_uuid=UUID, expected_generation=4,
                parameter_id=0x100,
                value_base64=base64.b64encode(b"new").decode(),
                confirmation=WRITE_CONFIRMATION)
        self.assertEqual(runner.value, b"old")

    def test_write_rejects_oversized_value_before_mutation(self):
        runner = FakeRunner()
        with self.assertRaisesRegex(DeviceParameterError, "不符合定义长度"):
            self.manager(runner).write(
                expected_uuid=UUID, expected_generation=4,
                parameter_id=0x100,
                value_base64=base64.b64encode(b"x" * 33).decode(),
                confirmation=WRITE_CONFIRMATION)
        self.assertFalse(any(any(item.startswith("param-set") for item in command)
                             for command in runner.commands))

    def test_server_exposes_only_read_and_rejects_cross_origin_writes(self):
        runner = FakeRunner()
        manager = self.manager(runner)
        with tempfile.TemporaryDirectory() as directory:
            server = make_server(
                "127.0.0.1", 0, None, history_root=Path(directory),
                device_parameter_manager=manager)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"
            try:
                target = json.loads(urlopen(base + "/api/project/target").read())
                self.assertTrue(target["device_parameters_enabled"])
                read = Request(
                    base + "/api/device-parameters/read", data=b"{}",
                    headers={"Content-Type": "application/json"}, method="POST")
                snapshot = json.loads(urlopen(read).read())["snapshot"]
                self.assertEqual(snapshot["node_uuid"], UUID)
                backup_request = Request(
                    base + "/api/device-parameters/backup", data=b"{}",
                    headers={"Content-Type": "application/json"}, method="POST")
                backup_response = json.loads(urlopen(backup_request).read())
                self.assertEqual(backup_response["format"],
                                 "DEVICE_PARAMETER_BACKUP_V2")
                self.assertEqual(backup_response["backup"]["schema_version"], 2)

                for path in ("write", "restore"):
                    write = Request(
                        base + "/api/device-parameters/" + path,
                        data=b"confirmation=WRITE_DEVICE_PARAMETERS",
                        headers={"Content-Type": "text/plain",
                                 "Origin": "https://evil.example"},
                        method="POST")
                    with self.assertRaises(HTTPError) as caught:
                        urlopen(write)
                    self.assertEqual(caught.exception.code, 404)
                self.assertFalse(any(any(item.startswith("param-set")
                                         for item in command)
                                     for command in runner.commands))
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()

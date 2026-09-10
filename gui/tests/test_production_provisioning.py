import base64
import sys
import tempfile
import unittest
import json
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from device_parameters import DeviceParameterError  # noqa: E402
from production_provisioning import (  # noqa: E402
    BATCH_FORMAT, ProvisioningBatchController, _digest, create_policy)


class Manager:
    node_id = 7
    def __init__(self, uuid, existing=b""):
        self.uuid = uuid; self.generation = 4; self.existing = existing; self.writes = 0
    def snapshot(self):
        return {"node_uuid": self.uuid, "status": {"generation": self.generation},
                "parameters": [{"id": 0x100, "byte_count": len(self.existing)}]}
    def write(self, **kwargs):
        self.writes += 1; self.generation += 1
        return self.snapshot()


class Audit:
    def begin(self, **kwargs):
        assert kwargs["operator"]["role"] == "provisioner"
        return "a" * 32
    def finish(self, *args, **kwargs): pass


class ProvisioningTests(unittest.TestCase):
    def setUp(self):
        self.uuid = "ab" * 16
        self.policy = create_policy(operators=[
            {"operator_id": "worker-1", "role": "provisioner"},
            {"operator_id": "lead-1", "role": "supervisor"}],
            parameter_rules=[{"parameter_id": 0x100, "strategy": "write_once"}])

    def batch(self, supervisor="lead-1"):
        value = {"format": BATCH_FORMAT, "schema_version": 1,
            "batch_id": "lot-001", "operator_id": "worker-1",
            "supervisor_id": supervisor, "devices": [{"uuid": self.uuid,
                "generation": 4, "parameters": [{"id": 0x100,
                "value_base64": base64.b64encode(b"SN001").decode()}]}]}
        value["sha256"] = _digest(value); return value

    def test_role_one_time_and_idempotent_batch(self):
        with tempfile.TemporaryDirectory() as temporary:
            manager = Manager(self.uuid)
            controller = ProvisioningBatchController(policy=self.policy,
                journal_root=Path(temporary), manager_factory=lambda _: manager,
                audit_factory=lambda _: Audit())
            with self.assertRaisesRegex(DeviceParameterError, "supervisor"):
                controller.execute(self.batch(supervisor="worker-1"))
            result = controller.execute(self.batch())
            self.assertEqual(result["record"]["outcome"], "completed")
            self.assertFalse(result["hardware_acceptance"])
            duplicate = controller.execute(self.batch())
            self.assertTrue(duplicate["replayed_existing"])
            self.assertEqual(duplicate["record"]["sha256"],
                             result["record"]["sha256"])
            self.assertEqual(manager.writes, 1)

    def test_existing_once_field_and_batch_conflict_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            manager = Manager(self.uuid, b"OLD")
            controller = ProvisioningBatchController(policy=self.policy,
                journal_root=Path(temporary), manager_factory=lambda _: manager,
                audit_factory=lambda _: Audit())
            with self.assertRaisesRegex(DeviceParameterError, "已经存在"):
                controller.execute(self.batch())
            self.assertEqual(manager.writes, 0)
            manager.existing = b""
            controller.execute(self.batch())
            changed = self.batch(); changed["devices"][0]["parameters"][0]["value_base64"] = base64.b64encode(b"SN002").decode(); changed["sha256"] = _digest(changed)
            with self.assertRaisesRegex(DeviceParameterError, "内容冲突"):
                controller.execute(changed)

    def test_cross_instance_lock_and_forged_replay_field_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); manager = Manager(self.uuid)
            first = ProvisioningBatchController(policy=self.policy,
                journal_root=root, manager_factory=lambda _: manager,
                audit_factory=lambda _: Audit())
            second = ProvisioningBatchController(policy=self.policy,
                journal_root=root, manager_factory=lambda _: manager,
                audit_factory=lambda _: Audit())
            lock = root / "batch-lot-001.lock"; lock.write_text("pid=other\n")
            with self.assertRaisesRegex(DeviceParameterError, "另一进程"):
                second.execute(self.batch())
            self.assertEqual(manager.writes, 0)
            lock.unlink(); first.execute(self.batch())
            path = root / "batch-lot-001.json"
            record = json.loads(path.read_text(encoding="utf-8"))
            record["replayed_existing"] = True
            from production_provisioning import _digest
            record["sha256"] = _digest(record)
            path.write_text(json.dumps(record), encoding="utf-8")
            with self.assertRaisesRegex(DeviceParameterError, "篡改"):
                second.execute(self.batch())


if __name__ == "__main__": unittest.main()

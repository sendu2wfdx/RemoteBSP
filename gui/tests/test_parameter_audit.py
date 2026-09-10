import json
import sys
import tempfile
import unittest
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from device_parameters import DeviceParameterError  # noqa: E402
from parameter_audit import ParameterAuditStore, parameter_evidence  # noqa: E402


class ParameterAuditTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.key = self.root / "key"
        self.key.write_bytes(b"k" * 32)
        self.audit = self.root / "audit"

    def tearDown(self):
        self.temporary.cleanup()

    def test_intent_and_terminal_are_atomic_hmac_chained_and_redacted(self):
        store = ParameterAuditStore(self.audit, self.key)
        evidence = parameter_evidence(0x100, b"top-secret")
        operation = store.begin(
            operation="write", node_id=7, node_uuid="ab" * 16,
            expected_generation=4, parameters=[evidence])
        pending = store.get(operation)
        self.assertEqual(len(pending["events"]), 1)
        self.assertEqual(pending["events"][0]["phase"], "intent")
        self.assertNotIn("top-secret", json.dumps(pending))
        self.assertEqual(pending["events"][0]["operator"], {
            "source": "local_cli", "identity": None,
            "authenticated": False})
        completed = store.finish(
            operation, outcome="success", generation_after=5,
            applied_count=1)
        self.assertEqual(completed["events"][1]["outcome"], "success")
        self.assertEqual(completed["events"][1]["previous_hmac_sha256"],
                         completed["events"][0]["event_hmac_sha256"])

    def test_partial_failure_and_tamper_are_detected(self):
        store = ParameterAuditStore(self.audit, self.key)
        operation = store.begin(
            operation="restore", node_id=7, node_uuid="ab" * 16,
            expected_generation=4,
            parameters=[parameter_evidence(0x100, b"one"),
                        parameter_evidence(0x101, b"two")])
        store.finish(operation, outcome="partial_failure",
                     generation_after=None, applied_count=1,
                     error_type="DeviceParameterError")
        path = self.audit / f"audit-{operation}.json"
        record = json.loads(path.read_text(encoding="utf-8"))
        record["events"][1]["applied_count"] = 2
        path.write_text(json.dumps(record), encoding="utf-8")
        with self.assertRaisesRegex(DeviceParameterError, "HMAC"):
            ParameterAuditStore(self.audit, self.key)

    def test_wrong_key_and_unsafe_directory_are_rejected(self):
        store = ParameterAuditStore(self.audit, self.key)
        store.begin(operation="write", node_id=7, node_uuid="ab" * 16,
                    expected_generation=1,
                    parameters=[parameter_evidence(1, b"x")])
        other = self.root / "other-key"
        other.write_bytes(b"z" * 32)
        with self.assertRaisesRegex(DeviceParameterError, "密钥标识"):
            ParameterAuditStore(self.audit, other)
        unsafe = self.root / "unsafe"
        unsafe.mkdir()
        (unsafe / "foreign.txt").write_text("x", encoding="utf-8")
        with self.assertRaisesRegex(DeviceParameterError, "非空"):
            ParameterAuditStore(unsafe, self.key)


if __name__ == "__main__":
    unittest.main()

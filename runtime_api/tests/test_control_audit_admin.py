import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from runtime_api.control_audit_admin import (
    ControlAuditAdminError, export_anchor, rotate_key, verify_anchor,
    verify_rotation)
from runtime_api.control_audit_journal import ControlAuditJournal


class ControlAuditAdminTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.old_key = self.root / "old.key"
        self.new_key = self.root / "new.key"
        self.other_key = self.root / "other.key"
        for path, value in ((self.old_key, b"o" * 32),
                            (self.new_key, b"n" * 32),
                            (self.other_key, b"x" * 32)):
            path.write_bytes(value)
            path.chmod(0o600)
        self.journal = self.root / "journal"

    def tearDown(self):
        self.temporary.cleanup()

    def _complete_record(self):
        with ControlAuditJournal(self.journal, self.old_key) as journal:
            intent = journal.append_intent(
                request_id="1" * 32, key_id="operator", action="gpio_write",
                request_digest=hashlib.sha256(b"request").hexdigest())
            journal.append_terminal(intent.sequence, "committed")

    def test_anchor_export_verify_and_tamper(self):
        self._complete_record()
        anchor = self.root / "head.json"
        exported = export_anchor(self.journal, self.old_key, anchor, label="shift-a")
        verified = verify_anchor(anchor, [self.old_key, self.new_key])
        self.assertEqual(verified["last_mac"], exported["last_mac"])
        self.assertEqual(verified["time_trust"], "untrusted_host_clock")
        changed = json.loads(anchor.read_text())
        changed["last_sequence"] = 1
        anchor.write_text(json.dumps(changed))
        anchor.chmod(0o600)
        with self.assertRaises(ControlAuditAdminError):
            verify_anchor(anchor, [self.old_key])

    def test_rotation_preserves_old_generation_and_starts_new(self):
        self._complete_record()
        archive = self.root / "archive"
        receipt = rotate_key(self.journal, self.old_key, self.new_key, archive)
        receipt_path = next(archive.glob("rotation-*.json"))
        verified = verify_rotation(receipt_path, [self.old_key, self.new_key])
        self.assertEqual(verified["old_last_mac"], receipt["old_last_mac"])
        old_generation = archive / str(receipt["old_generation"])
        with ControlAuditJournal(old_generation, self.old_key) as old:
            self.assertEqual(old.health_snapshot().records, 2)
        with ControlAuditJournal(self.journal, self.new_key) as new:
            self.assertEqual(new.health_snapshot().records, 0)
        with self.assertRaises(Exception):
            ControlAuditJournal(old_generation, self.new_key)
        with self.assertRaises(ControlAuditAdminError):
            verify_rotation(receipt_path, [self.old_key, self.other_key])

    def test_rotation_rejects_same_key_and_trust_is_bounded(self):
        self._complete_record()
        with self.assertRaises(ControlAuditAdminError):
            rotate_key(self.journal, self.old_key, self.old_key,
                       self.root / "archive")
        anchor = self.root / "head.json"
        export_anchor(self.journal, self.old_key, anchor)
        with self.assertRaises(ControlAuditAdminError):
            verify_anchor(anchor, [self.old_key] * 17)


if __name__ == "__main__":
    unittest.main()

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from runtime_api.tls_rotation_audit import (
    TlsRotationAuditError, TlsRotationAuditJournal)


class TlsRotationAuditJournalTest(unittest.TestCase):
    def test_atomic_write_failure_does_not_advance_visible_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "rotation.json"
            journal = TlsRotationAuditJournal(path, capacity=2)
            with patch("runtime_api.tls_rotation_audit.os.replace",
                       side_effect=OSError("injected write failure")):
                with self.assertRaises(OSError):
                    journal.append(
                        generation=1, result="reloaded",
                        old_fingerprint="a" * 64,
                        new_fingerprint="b" * 64)
            self.assertEqual(journal.status()["records"], 0)
            self.assertFalse(path.exists())

            journal.append(generation=1, result="startup_loaded",
                           old_fingerprint=None,
                           new_fingerprint="b" * 64)
            restored = TlsRotationAuditJournal(path, capacity=2)
            self.assertEqual(restored.next_generation(), 2)
            self.assertEqual(restored.status()["last"]["new_certificate_sha256"],
                             "b" * 64)

            path.write_text("{}", encoding="utf-8")
            path.chmod(0o600)
            with self.assertRaises(TlsRotationAuditError):
                TlsRotationAuditJournal(path)


if __name__ == "__main__":
    unittest.main()

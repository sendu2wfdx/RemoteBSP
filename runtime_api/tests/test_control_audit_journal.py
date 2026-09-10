import errno
import hashlib
import os
import tempfile
import unittest
from pathlib import Path

from runtime_api.control_audit_journal import (
    ControlAuditCapacityError,
    ControlAuditConfigError,
    ControlAuditCorruptionError,
    ControlAuditIoError,
    ControlAuditJournal,
    ControlAuditOptions,
)


class ControlAuditJournalTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.key = self.root / "audit.key"
        self.key.write_bytes(bytes(range(32)))
        self.key.chmod(0o600)
        self.directory = self.root / "journal"

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def _digest(value: bytes = b"request") -> str:
        return hashlib.sha256(value).hexdigest()

    def _intent(self, journal: ControlAuditJournal, index: int = 1):
        return journal.append_intent(
            request_id=f"{index:032x}", key_id="operator",
            action="gpio_write", lease_id=f"{index + 1:032x}",
            node_id=3, resource_id=7,
            request_digest=self._digest(str(index).encode()))

    def test_intent_terminal_restart_and_health(self):
        journal = ControlAuditJournal(self.directory, self.key,
                                      clock_ms=lambda: 1234)
        intent = self._intent(journal)
        terminal = journal.append_terminal(
            intent.sequence, "committed", operation_id=self._digest(b"op"))
        self.assertEqual((intent.sequence, terminal.sequence), (1, 2))
        health = journal.health_snapshot()
        self.assertTrue(health.operational)
        self.assertEqual((health.records, health.last_sequence,
                          health.dangling_intents), (2, 2, 0))
        journal.close()
        journal.close()

        reopened = ControlAuditJournal(self.directory, self.key)
        self.assertEqual([record.state for record in reopened.snapshot()],
                         ["intent", "terminal"])
        self.assertEqual(reopened.snapshot()[0].occurred_at_ms, 1234)
        reopened.close()

    def test_unknown_and_state_machine_are_strict(self):
        with ControlAuditJournal(self.directory, self.key) as journal:
            intent = self._intent(journal)
            unknown = journal.append_unknown(
                intent.sequence, "downstream_uncertain",
                operation_id=self._digest(b"unknown"))
            self.assertEqual(unknown.state, "unknown")
            with self.assertRaises(ValueError):
                journal.append_terminal(intent.sequence, "committed")
            with self.assertRaises(ValueError):
                journal.append_unknown(999, "process_recovery")

    def test_restart_recovers_dangling_intent_once(self):
        journal = ControlAuditJournal(self.directory, self.key)
        self._intent(journal)
        journal.close()

        reopened = ControlAuditJournal(self.directory, self.key)
        self.assertEqual([record.state for record in reopened.snapshot()],
                         ["intent", "unknown"])
        self.assertEqual(reopened.snapshot()[1].reason, "process_recovery")
        self.assertEqual(reopened.health_snapshot().dangling_intents, 0)
        reopened.close()

        reopened_again = ControlAuditJournal(self.directory, self.key)
        self.assertEqual(len(reopened_again.snapshot()), 2)
        reopened_again.close()

    def test_dangling_recovery_failure_keeps_startup_closed(self):
        journal = ControlAuditJournal(self.directory, self.key)
        self._intent(journal)
        journal.close()

        def fail_recovery(point: str) -> None:
            if point == "record_sync":
                raise OSError(errno.EIO, os.strerror(errno.EIO))

        with self.assertRaises(ControlAuditIoError):
            ControlAuditJournal(self.directory, self.key, io_hook=fail_recovery)
        # 失败构造必须已经释放 flock；下一次正常启动可以继续恢复。
        with ControlAuditJournal(self.directory, self.key) as reopened:
            self.assertEqual([record.state for record in reopened.snapshot()],
                             ["intent", "unknown"])

    def test_fields_are_bounded_and_raw_values_have_no_entry(self):
        with ControlAuditJournal(self.directory, self.key) as journal:
            with self.assertRaises(ValueError):
                journal.append_intent(
                    request_id="A" * 32, key_id="operator", action="acquire",
                    request_digest=self._digest())
            with self.assertRaises(ValueError):
                journal.append_intent(
                    request_id="0" * 32, key_id="bad\nkey", action="acquire",
                    request_digest=self._digest())
            with self.assertRaises(ValueError):
                journal.append_intent(
                    request_id="0" * 32, key_id="operator", action="raw_write",
                    request_digest=self._digest())
            with self.assertRaises(TypeError):
                journal.append_intent(  # type: ignore[call-arg]
                    request_id="0" * 32, key_id="operator", action="gpio_write",
                    request_digest=self._digest(), value=True)

    def test_keyed_request_digest_is_stable_bounded_and_redacted(self):
        raw = b'owner=operator;idempotency=guessable;gpio=1'
        with ControlAuditJournal(self.directory, self.key) as journal:
            first = journal.request_digest(raw)
            self.assertEqual(first, journal.request_digest(raw))
            self.assertNotEqual(first, journal.request_digest(raw[:-1] + b"0"))
            with self.assertRaises(ValueError):
                journal.request_digest(b"")
            with self.assertRaises(ValueError):
                journal.request_digest(b"x" * 4097)
            journal.append_intent(
                request_id="1" * 32, key_id="operator", action="gpio_write",
                request_digest=first)
        persisted = b"".join(
            path.read_bytes() for path in self.directory.iterdir()
            if path.is_file())
        self.assertNotIn(raw, persisted)
        self.assertNotIn(b"guessable", persisted)

    def test_wrong_key_and_manifest_tamper_fail_closed(self):
        with ControlAuditJournal(self.directory, self.key) as journal:
            self._intent(journal)
        wrong = self.root / "wrong.key"
        wrong.write_bytes(b"z" * 32)
        wrong.chmod(0o600)
        with self.assertRaises(ControlAuditCorruptionError):
            ControlAuditJournal(self.directory, wrong)

        manifest = self.directory / "manifest.json"
        data = bytearray(manifest.read_bytes())
        position = data.index(b'"record_count":1') + len('"record_count":')
        data[position] = ord("0")
        manifest.write_bytes(data)
        manifest.chmod(0o600)
        with self.assertRaises(ControlAuditCorruptionError):
            ControlAuditJournal(self.directory, self.key)

    def test_record_tamper_and_committed_truncation_are_corruption(self):
        with ControlAuditJournal(self.directory, self.key) as journal:
            self._intent(journal)
        segment = self.directory / "segment-0000000000000001.rcaj"
        original = segment.read_bytes()
        changed = bytearray(original)
        changed[20] ^= 1
        segment.write_bytes(changed)
        segment.chmod(0o600)
        with self.assertRaises(ControlAuditCorruptionError):
            ControlAuditJournal(self.directory, self.key)
        segment.write_bytes(original[:-1])
        segment.chmod(0o600)
        with self.assertRaises(ControlAuditCorruptionError):
            ControlAuditJournal(self.directory, self.key)

    def test_uncommitted_tail_is_truncated_to_signed_manifest(self):
        with ControlAuditJournal(self.directory, self.key) as journal:
            self._intent(journal)
        segment = self.directory / "segment-0000000000000001.rcaj"
        committed_size = segment.stat().st_size
        with segment.open("ab") as output:
            output.write(b"partial-uncommitted-frame")
            output.flush()
            os.fsync(output.fileno())
        with ControlAuditJournal(self.directory, self.key) as reopened:
            self.assertEqual([record.state for record in reopened.snapshot()],
                             ["intent", "unknown"])
        self.assertNotIn(b"partial-uncommitted-frame", segment.read_bytes())
        self.assertGreater(segment.stat().st_size, committed_size)

    def test_rotation_and_total_capacity_are_bounded(self):
        options = ControlAuditOptions(
            maximum_records=4, maximum_segment_bytes=512,
            maximum_total_bytes=2048)
        with ControlAuditJournal(self.directory, self.key,
                                 options=options) as journal:
            first = self._intent(journal, 1)
            journal.append_terminal(first.sequence, "committed")
            second = self._intent(journal, 2)
            journal.append_unknown(second.sequence, "deadline_exceeded")
            self.assertGreaterEqual(journal.health_snapshot().segments, 2)
            with self.assertRaises(ControlAuditCapacityError):
                self._intent(journal, 3)
            self.assertTrue(journal.operational)

    def test_lock_permissions_symlink_and_hardlink_are_rejected(self):
        journal = ControlAuditJournal(self.directory, self.key)
        with self.assertRaises(ControlAuditConfigError):
            ControlAuditJournal(self.directory, self.key)
        journal.close()

        self.key.chmod(0o644)
        with self.assertRaises(ControlAuditConfigError):
            ControlAuditJournal(self.root / "bad-perm", self.key)
        self.key.chmod(0o600)
        key_link = self.root / "key-link"
        key_link.symlink_to(self.key)
        with self.assertRaises(ControlAuditConfigError):
            ControlAuditJournal(self.root / "key-symlink", key_link)

        key_hardlink = self.root / "key-hardlink"
        os.link(self.key, key_hardlink)
        with self.assertRaises(ControlAuditConfigError):
            ControlAuditJournal(self.root / "key-hard", self.key)

    def test_segment_link_and_permissions_are_rejected(self):
        with ControlAuditJournal(self.directory, self.key) as journal:
            self._intent(journal)
        segment = self.directory / "segment-0000000000000001.rcaj"
        segment.chmod(0o644)
        with self.assertRaises(ControlAuditConfigError):
            ControlAuditJournal(self.directory, self.key)
        segment.chmod(0o600)
        linked = self.root / "segment-link"
        os.link(segment, linked)
        with self.assertRaises(ControlAuditConfigError):
            ControlAuditJournal(self.directory, self.key)

    def test_enospc_and_eio_make_journal_unavailable(self):
        for point, error in (("record_write", errno.ENOSPC),
                             ("record_sync", errno.EIO)):
            with self.subTest(point=point):
                directory = self.root / point

                def fail(actual: str, target: str = point,
                         number: int = error) -> None:
                    if actual == target:
                        raise OSError(number, os.strerror(number))

                journal = ControlAuditJournal(
                    directory, self.key, io_hook=fail)
                with self.assertRaises(ControlAuditIoError):
                    self._intent(journal)
                self.assertFalse(journal.operational)
                with self.assertRaises(ControlAuditIoError):
                    self._intent(journal, 2)
                journal.close()

    def test_manifest_sync_failure_is_fail_closed_and_recoverable(self):
        failed = False

        def fail_once(point: str) -> None:
            nonlocal failed
            if point == "manifest_sync" and not failed:
                failed = True
                raise OSError(errno.EIO, os.strerror(errno.EIO))

        # 初始化 manifest 也使用相同同步点，因此先正常初始化再注入。
        journal = ControlAuditJournal(self.directory, self.key)
        journal._io_hook = fail_once  # 仅测试故障注入边界。
        with self.assertRaises(ControlAuditIoError):
            self._intent(journal)
        journal.close()
        with ControlAuditJournal(self.directory, self.key) as reopened:
            self.assertEqual(reopened.snapshot(), ())

    def test_directory_symlink_and_unknown_file_are_rejected(self):
        real = self.root / "real"
        real.mkdir(mode=0o700)
        linked = self.root / "linked-dir"
        linked.symlink_to(real, target_is_directory=True)
        with self.assertRaises(ControlAuditConfigError):
            ControlAuditJournal(linked, self.key)

        journal = ControlAuditJournal(self.directory, self.key)
        journal.close()
        unknown = self.directory / "unexpected"
        unknown.write_bytes(b"x")
        unknown.chmod(0o600)
        with self.assertRaises(ControlAuditCorruptionError):
            ControlAuditJournal(self.directory, self.key)

    def test_interrupted_manifest_temporary_file_is_cleaned_safely(self):
        journal = ControlAuditJournal(self.directory, self.key)
        journal.close()
        temporary = self.directory / "manifest.tmp-123-456"
        temporary.write_bytes(b"uncommitted")
        temporary.chmod(0o600)
        with ControlAuditJournal(self.directory, self.key):
            self.assertFalse(temporary.exists())


if __name__ == "__main__":
    unittest.main()

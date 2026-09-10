"""ControlAuditJournal 真实文件系统掉电、损坏与进程重启演练。"""

import hashlib
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from runtime_api.control_audit_journal import (
    ControlAuditConfigError,
    ControlAuditCorruptionError,
    ControlAuditIoError,
    ControlAuditJournal,
    ControlAuditOptions,
)


class ControlAuditFilesystemDrillTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.key = self.root / "audit.key"
        self.key.write_bytes(bytes(range(32)))
        self.key.chmod(0o600)

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def _intent(journal: ControlAuditJournal, index: int):
        return journal.append_intent(
            request_id=f"{index:032x}", key_id="filesystem-drill",
            action="gpio_write", lease_id=f"{index + 100:032x}",
            node_id=1, resource_id=index,
            request_digest=hashlib.sha256(str(index).encode()).hexdigest())

    def _append_pair(self, directory: Path, index: int = 1,
                     options: ControlAuditOptions | None = None) -> None:
        with ControlAuditJournal(directory, self.key, options=options) as journal:
            intent = self._intent(journal, index)
            journal.append_terminal(intent.sequence, "committed")

    def test_power_loss_tail_is_trimmed_but_committed_boundary_survives(self):
        directory = self.root / "tail"
        self._append_pair(directory)
        segment = directory / "segment-0000000000000001.rcaj"
        committed = segment.read_bytes()
        with segment.open("ab") as output:
            output.write(b"simulated-torn-write")
            output.flush()
            os.fsync(output.fileno())

        with ControlAuditJournal(directory, self.key) as reopened:
            self.assertEqual(len(reopened.snapshot()), 2)
            self.assertTrue(reopened.operational)
        self.assertEqual(segment.read_bytes(), committed)

    def test_committed_truncation_and_chain_damage_are_local_fail_closed(self):
        truncated = self.root / "truncated"
        damaged = self.root / "damaged"
        healthy = self.root / "healthy"
        for directory in (truncated, damaged, healthy):
            self._append_pair(directory)

        truncated_segment = truncated / "segment-0000000000000001.rcaj"
        truncated_segment.write_bytes(truncated_segment.read_bytes()[:-1])
        damaged_segment = damaged / "segment-0000000000000001.rcaj"
        payload = bytearray(damaged_segment.read_bytes())
        payload[24] ^= 0x01
        damaged_segment.write_bytes(payload)

        for directory in (truncated, damaged):
            with self.subTest(directory=directory.name):
                with self.assertRaises(ControlAuditCorruptionError):
                    ControlAuditJournal(directory, self.key)
        # 一个 journal 的损坏不得污染另一资源域的读取和重启。
        with ControlAuditJournal(healthy, self.key) as reopened:
            self.assertEqual(len(reopened.snapshot()), 2)

    def test_segment_rotation_restarts_from_signed_chain_head(self):
        directory = self.root / "rotation"
        options = ControlAuditOptions(
            maximum_records=12, maximum_segment_bytes=512,
            maximum_total_bytes=6144)
        with ControlAuditJournal(directory, self.key, options=options) as journal:
            for index in range(1, 5):
                intent = self._intent(journal, index)
                journal.append_terminal(intent.sequence, "committed")
            before = journal.chain_head()
            self.assertGreater(before.active_segment, 1)
        with ControlAuditJournal(directory, self.key, options=options) as reopened:
            after = reopened.chain_head()
            self.assertEqual(after, before)
            self.assertEqual(len(reopened.snapshot()), 8)

    def test_rotation_creation_failure_poison_closes_current_writer(self):
        directory = self.root / "rotation-failure"
        options = ControlAuditOptions(
            maximum_records=4, maximum_segment_bytes=512,
            maximum_total_bytes=2048)
        journal = ControlAuditJournal(directory, self.key, options=options)
        intent = self._intent(journal, 1)
        collision = directory / "segment-0000000000000002.rcaj"
        collision.write_bytes(b"")
        collision.chmod(0o600)
        with self.assertRaises(ControlAuditIoError):
            journal.append_terminal(intent.sequence, "committed")
        self.assertFalse(journal.operational)
        with self.assertRaises(ControlAuditIoError):
            self._intent(journal, 2)
        journal.close()
        # 重启会删除 manifest 未引用的空分段，再把 durable intent 收敛为 unknown。
        with ControlAuditJournal(directory, self.key, options=options) as reopened:
            self.assertEqual(
                [record.state for record in reopened.snapshot()],
                ["intent", "unknown"])

    def test_other_process_is_locked_out_and_clean_restart_recovers(self):
        directory = self.root / "process-lock"
        repository = Path(__file__).resolve().parents[2]
        script = textwrap.dedent("""
            import sys
            from runtime_api.control_audit_journal import ControlAuditJournal
            journal = ControlAuditJournal(sys.argv[1], sys.argv[2])
            print("READY", flush=True)
            sys.stdin.readline()
            journal.close()
        """)
        process = subprocess.Popen(
            [sys.executable, "-c", script, str(directory), str(self.key)],
            cwd=repository, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), "READY")
            with self.assertRaisesRegex(
                    ControlAuditConfigError, "另一个Runtime实例锁定"):
                ControlAuditJournal(directory, self.key)
            assert process.stdin is not None
            process.stdin.write("stop\n")
            process.stdin.flush()
            self.assertEqual(process.wait(timeout=5), 0, process.stderr.read())
            with ControlAuditJournal(directory, self.key) as reopened:
                self.assertTrue(reopened.operational)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream is not None:
                    stream.close()


if __name__ == "__main__":
    unittest.main()

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class PersistenceRecoveryProcessTest(unittest.TestCase):
    """跨真实解释器进程演练持久文件恢复边界。"""

    @staticmethod
    def _run(source: str, *arguments: Path,
             expect_success: bool = True) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [sys.executable, "-c", source, *(str(item) for item in arguments)],
            cwd=Path(__file__).resolve().parents[2], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15,
            check=False)
        if expect_success and result.returncode != 0:
            raise AssertionError(result.stderr)
        return result

    def test_event_store_and_control_audit_process_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            events = root / "events"
            audit = root / "audit"
            key = root / "audit.key"
            key.write_bytes(bytes(range(32)))
            key.chmod(0o600)

            self._run("""
from pathlib import Path
import sys
from runtime_api.event_store import RuntimeEventStore
from runtime_api.events import RuntimeEventLog
from runtime_api.provider import mock_snapshot
root=Path(sys.argv[1])
log=RuntimeEventLog(capacity=8, incarnation='1'*32,
                    store=RuntimeEventStore(root))
assert log.observe(mock_snapshot())
""", events)
            event_file = events / "event-history-v1.json"
            original = event_file.read_bytes()
            event_file.write_bytes(original[:len(original) // 2])
            event_file.chmod(0o600)
            self._run("""
from pathlib import Path
import sys
from runtime_api.event_store import RuntimeEventStore
from runtime_api.events import RuntimeEventLog
from runtime_api.provider import mock_snapshot
root=Path(sys.argv[1]); store=RuntimeEventStore(root)
log=RuntimeEventLog(capacity=8, incarnation='2'*32, store=store)
assert log.persistence_status['load_status']=='corrupt_isolated'
assert log.observe(mock_snapshot())
assert not list(root.glob('.*.tmp'))
""", events)
            self.assertEqual(len(list(events.glob(
                "event-history-v1.corrupt-*.json"))), 1)
            self.assertTrue(event_file.read_bytes().endswith(b"\n"))

            self._run("""
from pathlib import Path
import hashlib,sys
from runtime_api.control_audit_journal import ControlAuditJournal
j=ControlAuditJournal(Path(sys.argv[1]),Path(sys.argv[2]))
j.append_intent(request_id='1'*32,key_id='operator',action='gpio_write',
 lease_id='2'*32,node_id=1,resource_id=1,
 request_digest=hashlib.sha256(b'request').hexdigest())
j.close()
""", audit, key)
            segment = audit / "segment-0000000000000001.rcaj"
            committed = segment.stat().st_size
            with segment.open("ab") as output:
                output.write(b"uncommitted-tail")
                output.flush()
                os.fsync(output.fileno())
            self._run("""
from pathlib import Path
import sys
from runtime_api.control_audit_journal import ControlAuditJournal
with ControlAuditJournal(Path(sys.argv[1]),Path(sys.argv[2])) as j:
 assert [r.state for r in j.snapshot()]==['intent','unknown']
""", audit, key)
            self.assertNotIn(b"uncommitted-tail", segment.read_bytes())
            self.assertGreater(segment.stat().st_size, committed)

            # 已由签名 manifest 承诺的字节被截断时，新进程必须失败关闭。
            segment.write_bytes(segment.read_bytes()[:-1])
            segment.chmod(0o600)
            failed = self._run("""
from pathlib import Path
import sys
from runtime_api.control_audit_journal import ControlAuditJournal
ControlAuditJournal(Path(sys.argv[1]),Path(sys.argv[2]))
""", audit, key, expect_success=False)
            self.assertNotEqual(failed.returncode, 0)


if __name__ == "__main__":
    unittest.main()

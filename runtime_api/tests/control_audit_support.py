"""Runtime HTTP 测试使用的同步控制审计替身。"""

from types import SimpleNamespace

from runtime_api.control_audit_journal import (
    ControlAuditHealth,
    ControlAuditIoError,
)


class FakeControlAuditJournal:
    def __init__(self):
        self.operational = True
        self.events = []
        self._sequence = 0
        self.fail_terminal = False

    def request_digest(self, value):
        if not self.operational:
            raise ControlAuditIoError("审计不可用")
        self.events.append(("digest", value))
        return "d" * 64

    def append_intent(self, **fields):
        if not self.operational:
            raise ControlAuditIoError("审计不可用")
        self._sequence += 1
        self.events.append(("intent", fields))
        return SimpleNamespace(sequence=self._sequence)

    def append_terminal(self, sequence, result, *, operation_id=None):
        if self.fail_terminal:
            self.operational = False
            raise ControlAuditIoError("模拟terminal同步失败")
        self._sequence += 1
        self.events.append(("terminal", sequence, result, operation_id))
        return SimpleNamespace(sequence=self._sequence)

    def append_unknown(self, sequence, reason, *, operation_id=None):
        self._sequence += 1
        self.events.append(("unknown", sequence, reason, operation_id))
        return SimpleNamespace(sequence=self._sequence)

    def health_snapshot(self):
        return ControlAuditHealth(
            operational=self.operational,
            records=self._sequence,
            segments=1,
            bytes=128 * self._sequence,
            last_sequence=self._sequence,
            dangling_intents=0)

    def close(self):
        self.operational = False

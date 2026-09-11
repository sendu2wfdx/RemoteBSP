import copy
import sys
import tempfile
import unittest
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from production_signing import create_trust_policy, generate_key_pair, sign_evidence  # noqa: E402
from project_config import ProjectConfigError  # noqa: E402
from release_approval import (  # noqa: E402
    _sign_external_time_for_test, approve_release, create_authority_policy,
    verify_external_time_attestation, verify_release_approval)


class ReleaseApprovalTests(unittest.TestCase):
    def test_approval_and_external_time_are_separate_verified_authorities(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            keys = []
            for name in ("signer", "approver", "time"):
                private, public = root / f"{name}.key", root / f"{name}.pub"
                info = generate_key_pair(private, public); keys.append((private, public, info))
            evidence = {"format": "REMOTEBSP_PRODUCTION_BATCH_V1", "value": 1}
            signature = sign_evidence(evidence, keys[0][0])
            evidence_policy = create_trust_policy([keys[0][1]])
            authority = create_authority_policy([
                {"key_id": keys[1][2]["key_id"], "role": "release_approver",
                 "status": "active", "public_key_pem": keys[1][1].read_text()},
                {"key_id": keys[2][2]["key_id"], "role": "time_authority",
                 "status": "active", "public_key_pem": keys[2][1].read_text()}])
            approval = approve_release(evidence, signature, evidence_policy,
                                       keys[1][0], authority,
                                       decision="approved", note="离线审批")
            checked = verify_release_approval(evidence, signature, evidence_policy,
                                              approval, authority)
            self.assertFalse(checked["time_trusted"])
            stamp = _sign_external_time_for_test(
                keys[2][0], approval["sha256"], "2026-09-11T08:00:00Z")
            trusted = verify_external_time_attestation(approval, stamp, authority)
            self.assertTrue(trusted["time_trusted"])
            changed = copy.deepcopy(stamp); changed["time_utc"] = "2026-09-12T08:00:00Z"
            with self.assertRaises(ProjectConfigError):
                verify_external_time_attestation(approval, changed, authority)

    def test_revoked_or_wrong_role_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); private = root / "a.key"; public = root / "a.pub"
            info = generate_key_pair(private, public)
            time_private = root / "t.key"; time_public = root / "t.pub"
            time_info = generate_key_pair(time_private, time_public)
            policy = create_authority_policy([{"key_id": info["key_id"],
                "role": "time_authority", "status": "revoked",
                "public_key_pem": public.read_text()}])
            approval = {"sha256": "a" * 64}
            stamp = _sign_external_time_for_test(private, "a" * 64,
                                                 "2026-09-11T08:00:00Z")
            with self.assertRaises(ProjectConfigError):
                verify_external_time_attestation(approval, stamp, policy)

    def test_policy_pem_note_and_time_boundaries_fail_before_trust(self):
        with self.assertRaisesRegex(ProjectConfigError, "PEM长度"):
            create_authority_policy([{"key_id": "x", "role": "time_authority",
                "status": "active", "public_key_pem": "x" * 4097}])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); private = root / "a.key"; public = root / "a.pub"
            info = generate_key_pair(private, public)
            time_private = root / "t.key"; time_public = root / "t.pub"
            time_info = generate_key_pair(time_private, time_public)
            authority = create_authority_policy([{"key_id": info["key_id"],
                "role": "release_approver", "status": "active",
                "public_key_pem": public.read_text()}, {"key_id": time_info["key_id"],
                "role": "time_authority", "status": "active",
                "public_key_pem": time_public.read_text()}])
            evidence = {"format": "REMOTEBSP_PRODUCTION_BATCH_V1", "value": 1}
            signature = sign_evidence(evidence, private)
            trust = create_trust_policy([public])
            with self.assertRaisesRegex(ProjectConfigError, "备注"):
                approve_release(evidence, signature, trust, private, authority,
                                decision="approved", note="x" * 513)
            approval = approve_release(evidence, signature, trust, private, authority,
                                       decision="approved", note="ok")
            noncanonical = _sign_external_time_for_test(
                time_private, approval["sha256"], "2026-09-11T08:00:00.000Z")
            with self.assertRaisesRegex(ProjectConfigError, "规范UTC"):
                verify_external_time_attestation(approval, noncanonical, authority)
            malformed = copy.deepcopy(approval); malformed.pop("decision")
            with self.assertRaisesRegex(ProjectConfigError, "发布审批无效"):
                verify_external_time_attestation(malformed, noncanonical, authority)


if __name__ == "__main__": unittest.main()

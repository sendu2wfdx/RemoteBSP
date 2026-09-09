import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from runtime_api.auth import (
    MAXIMUM_AUTH_CONFIG_BYTES,
    MAXIMUM_API_KEYS,
    ApiKeyAuthenticator,
    AuthConfigurationError,
    load_api_key_authenticator,
)


class ApiKeyAuthenticatorTest(unittest.TestCase):
    KEY_A = "a" * 32
    KEY_B = "b" * 48

    def _write(self, directory: str, value: object) -> Path:
        path = Path(directory) / "runtime-auth.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_load_and_verify_all_configured_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            authenticator = load_api_key_authenticator(self._write(
                directory, {"schema_version": 1,
                            "api_keys": [self.KEY_A, self.KEY_B]}))
        self.assertEqual(authenticator.key_count, 2)
        self.assertTrue(authenticator.verify(self.KEY_A))
        self.assertTrue(authenticator.verify(self.KEY_B))
        self.assertFalse(authenticator.verify("x" * 32))

    def test_verification_does_not_stop_at_first_match(self):
        authenticator = ApiKeyAuthenticator([self.KEY_A, self.KEY_B])
        calls: list[tuple[bytes, bytes]] = []

        def compare(left: bytes, right: bytes) -> bool:
            calls.append((left, right))
            return left == right

        with patch("runtime_api.auth.hmac.compare_digest",
                   side_effect=compare):
            self.assertTrue(authenticator.verify(self.KEY_A))
        self.assertEqual(len(calls), 2)

    def test_configuration_is_bounded_and_closed(self):
        invalid_values = (
            {"schema_version": 1, "api_keys": []},
            {"schema_version": 1,
             "api_keys": [str(index).zfill(32)
                          for index in range(MAXIMUM_API_KEYS + 1)]},
            {"schema_version": 1, "api_keys": ["short"]},
            {"schema_version": 1, "api_keys": ["a" * 31 + " "]},
            {"schema_version": 1, "api_keys": [self.KEY_A, self.KEY_A]},
            {"schema_version": 2, "api_keys": [self.KEY_A]},
            {"schema_version": True, "api_keys": [self.KEY_A]},
            {"schema_version": 1, "api_keys": [self.KEY_A], "extra": 1},
        )
        with tempfile.TemporaryDirectory() as directory:
            for index, value in enumerate(invalid_values):
                with self.subTest(index=index):
                    with self.assertRaises(AuthConfigurationError):
                        load_api_key_authenticator(self._write(directory,
                                                               value))

            path = Path(directory) / "duplicate.json"
            path.write_text(
                '{"schema_version":1,"schema_version":1,"api_keys":[]}',
                encoding="utf-8")
            with self.assertRaisesRegex(AuthConfigurationError, "重复字段"):
                load_api_key_authenticator(path)

            oversized = Path(directory) / "oversized.json"
            oversized.write_bytes(b" " * (MAXIMUM_AUTH_CONFIG_BYTES + 1))
            with self.assertRaisesRegex(AuthConfigurationError, "不得超过"):
                load_api_key_authenticator(oversized)


if __name__ == "__main__":
    unittest.main()

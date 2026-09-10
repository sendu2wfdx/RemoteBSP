import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from runtime_api.tls_deployment import (
    TlsDeploymentError, create_artifact, preflight,
)


class TlsDeploymentTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.certificate = self.root / "server.crt"
        self.private_key = self.root / "server.key"
        self.certificate.write_text("CERTIFICATE", encoding="ascii")
        self.private_key.write_text("PRIVATE KEY", encoding="ascii")
        self.certificate.chmod(0o644)
        self.private_key.chmod(0o600)
        self.config = self.root / "tls-baseline.json"

    def tearDown(self):
        self.temporary.cleanup()

    def _create(self):
        return create_artifact(
            self.config, runtime_bind="127.0.0.1", proxy_bind="::1",
            certificate_file=self.certificate.resolve(),
            private_key_file=self.private_key.resolve())

    def test_valid_preflight_keeps_loopback_and_strips_forwarded_headers(self):
        self._create()
        with patch("ssl.SSLContext.load_cert_chain") as load:
            result = preflight(self.config)
        self.assertTrue(result["ok"])
        self.assertEqual(result["runtime_bind"], "127.0.0.1")
        self.assertEqual(result["proxy_bind"], "::1")
        self.assertEqual(result["forwarded_headers"], "strip_all")
        load.assert_called_once()

    def test_non_loopback_missing_and_unsafe_paths_fail_closed(self):
        with self.assertRaises(TlsDeploymentError):
            create_artifact(self.config, runtime_bind="0.0.0.0",
                            proxy_bind="127.0.0.1",
                            certificate_file=self.certificate.resolve(),
                            private_key_file=self.private_key.resolve())
        self._create()
        self.private_key.chmod(0o644)
        with self.assertRaises(TlsDeploymentError):
            preflight(self.config)
        self.private_key.unlink()
        with self.assertRaises(TlsDeploymentError):
            preflight(self.config)

    def test_config_tamper_missing_digest_and_proxy_trust_fail_closed(self):
        self._create()
        self.config.write_bytes(self.config.read_bytes() + b" ")
        with self.assertRaises(TlsDeploymentError):
            preflight(self.config)
        self._create()
        self.config.with_suffix(".json.sha256").unlink()
        with self.assertRaises(TlsDeploymentError):
            preflight(self.config)
        self._create()
        raw = self.config.read_text(encoding="utf-8").replace(
            '"strip_all"', '"trust_x_forwarded_for"')
        self.config.write_text(raw, encoding="utf-8")
        self.config.with_suffix(".json.sha256").write_text(
            hashlib.sha256(self.config.read_bytes()).hexdigest() + "\n",
            encoding="ascii")
        self.config.with_suffix(".json.sha256").chmod(0o600)
        # 即使摘要被一并更新，严格 schema 仍不能放宽代理信任边界。
        with self.assertRaises(TlsDeploymentError):
            preflight(self.config)


if __name__ == "__main__":
    unittest.main()

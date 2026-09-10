import http.client
import json
import socket
import subprocess
import sys
import time
import tempfile
import unittest
from pathlib import Path


class ResourceExhaustionProcessTest(unittest.TestCase):
    def test_isolated_slow_clients_and_oversized_body(self):
        temporary = tempfile.TemporaryDirectory()
        auth_file = Path(temporary.name) / "auth.json"
        api_key = "resource-exhaustion-test-key-123456"
        auth_file.write_text(json.dumps({"schema_version": 2, "keys": [{
            "key_id": "resource-test", "api_key": api_key,
            "permissions": ["runtime.read",
                            "runtime.control.lease.acquire"]}]}),
            encoding="utf-8")
        auth_file.chmod(0o600)
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        process = subprocess.Popen([
            sys.executable, "-m", "runtime_api.server", "--host",
            "127.0.0.1", "--port", str(port), "--http-workers", "3",
            "--http-request-timeout-ms", "800",
            "--api-key-file", str(auth_file)],
            cwd=Path(__file__).resolve().parents[2],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        slow_header = None
        slow_sse = None
        try:
            deadline = time.monotonic() + 8
            while True:
                try:
                    connection = http.client.HTTPConnection(
                        "127.0.0.1", port, timeout=1)
                    connection.request("GET", "/api/v1/",
                                       headers={"X-API-Key": api_key})
                    self.assertEqual(connection.getresponse().status, 200)
                    connection.close()
                    break
                except OSError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        self.fail("资源耗尽演练进程未启动")
                    time.sleep(0.05)

            # 一个残缺头部占用一个工作槽，不能阻塞独立读取。
            slow_header = socket.create_connection(("127.0.0.1", port), timeout=2)
            slow_header.sendall(
                b"GET /api/v1/ HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                + f"X-API-Key: {api_key}\r\nX-Slow:".encode())

            # 一个不消费响应体的SSE客户端占用另一个槽，仍保留第三槽服务读取。
            slow_sse = socket.create_connection(("127.0.0.1", port), timeout=2)
            slow_sse.sendall(
                b"GET /api/v1/overview/stream HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\nAccept: text/event-stream\r\n"
                + f"X-API-Key: {api_key}\r\n\r\n".encode())
            self.assertIn(b"200 OK", slow_sse.recv(512))
            healthy = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
            healthy.request("GET", "/api/v1/",
                            headers={"X-API-Key": api_key})
            self.assertEqual(healthy.getresponse().status, 200)
            healthy.close()

            # 超大请求体只被头部声明拒绝，不读取或分配声明的全部内容。
            oversized = socket.create_connection(("127.0.0.1", port), timeout=2)
            oversized.sendall(
                b"POST /api/v1/control-leases HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\nContent-Type: application/json\r\n"
                + f"X-API-Key: {api_key}\r\n".encode()
                + b"Content-Length: 99999999\r\n\r\n{}")
            response = oversized.recv(4096)
            oversized.close()
            self.assertIn(b"413 Request Entity Too Large", response)

            final = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
            final.request("GET", "/api/v1/",
                          headers={"X-API-Key": api_key})
            self.assertEqual(final.getresponse().status, 200)
            final.close()
        finally:
            if slow_header is not None:
                slow_header.close()
            if slow_sse is not None:
                slow_sse.close()
            process.terminate()
            process.communicate(timeout=5)
            temporary.cleanup()


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Tests tools/fake_connector.py without a board: a Python stand-in dials it the way the firmware
does and answers its http_requests in chunk mode (and text mode), then the fake Connector's verdicts
are checked. Variants that break the contract (lost chunk, wrong sha256, bad byte) must FAIL.

    python3 tools/test_fake_connector.py
"""

import base64
import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ["NO_PROXY"] = os.environ["no_proxy"] = "*"  # urllib must not send 127.0.0.1 to a proxy
CHUNK = 8 * 1024


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class FakeBoard:
    """A WebSocket client speaking the device side of the Connector protocol, like connector.c."""

    def __init__(self, port, corrupt=None, scope=""):
        self.corrupt = corrupt
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            (
                f"GET /v1/connect?name=Comma-TEST&alias=comma-test&os=esp-idf&arch=xtensa&scope={scope} HTTP/1.1\r\n"
                f"Host: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                "Authorization: Bearer salix_conn_dev_override\r\n\r\n"
            ).encode()
        )
        head = b""
        while b"\r\n\r\n" not in head:
            head += self.sock.recv(1)
        assert head.startswith(b"HTTP/1.1 101"), head
        # RFC 6455 §4.2.2, as esp_transport_ws checks it.
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert b"Sec-WebSocket-Accept: " + accept + b"\r\n" in head, head
        self.buf = b""
        self.send(
            {
                "type": "metadata",
                "capabilities": {
                    "scope": scope,
                    "lan_gateway": {
                        "methods": ["http_request"],
                        "response_chunks": {"max_total_bytes": 4194304, "chunk_bytes": 32768},
                    },
                },
                "system_info": {"hostname": "Comma-TEST"},
            }
        )

    def send(self, obj):
        payload = json.dumps(obj, separators=(",", ":")).encode()
        n = len(payload)
        if n < 126:
            head = bytes([0x81, 0x80 | n])
        elif n < 65536:
            head = bytes([0x81, 0x80 | 126]) + n.to_bytes(2, "big")
        else:
            head = bytes([0x81, 0x80 | 127]) + n.to_bytes(8, "big")
        mask = os.urandom(4)
        key = (mask * (n // 4 + 1))[:n]
        masked = (int.from_bytes(payload, "big") ^ int.from_bytes(key, "big")).to_bytes(n, "big") if n else b""
        self.sock.sendall(head + mask + masked)

    def read(self, n):
        while len(self.buf) < n:
            got = self.sock.recv(65536)
            if not got:
                raise EOFError
            self.buf += got
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        head = self.read(2)
        n = head[1] & 0x7F
        if n == 126:
            n = int.from_bytes(self.read(2), "big")
        elif n == 127:
            n = int.from_bytes(self.read(8), "big")
        return head[0] & 0x0F, self.read(n)

    def run(self):
        """Answers requests until the fake Connector goes quiet."""
        while True:
            try:
                op, data = self.recv()
            except (EOFError, socket.timeout, OSError):
                return
            if op != 1:
                continue
            frame = json.loads(data)
            if frame.get("type") == "heartbeat":
                self.send({"type": "heartbeat"})
            elif frame.get("type") == "request":
                self.answer(frame)

    def answer(self, req):
        params = req["params"]
        rid = req["id"]
        try:
            with urllib.request.urlopen(params["url"], timeout=10) as r:
                status, headers, body = r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            status, headers, body = e.code, dict(e.headers), e.read()
        except OSError as e:
            self.send({"id": rid, "type": "error", "error": f"connect failed: {e}"})
            return
        headers = {k: [v] for k, v in headers.items()}
        if params.get("response_mode") != "chunks":
            text = body[:65536].decode("utf-8", "replace")
            self.send({"id": rid, "type": "response", "result": {"status": status, "headers": headers, "body": text,
                                                                  "truncated": len(body) > 65536, "body_bytes": len(body[:65536])}})
            return
        limit = min(params.get("max_bytes") or 4194304, 4194304)
        truncated = len(body) > limit
        body = body[:limit]
        pieces = [body[i:i + CHUNK] for i in range(0, len(body), CHUNK)]
        sent = bytearray()
        for seq, piece in enumerate(pieces):
            if self.corrupt == "drop" and seq == 2:
                continue
            if self.corrupt == "flip" and seq == 1:
                piece = bytes([piece[0] ^ 1]) + piece[1:]
            self.send({"id": rid, "type": "response_chunk", "seq": seq, "data_b64": base64.b64encode(piece).decode()})
            sent += piece
        digest = hashlib.sha256(body).hexdigest()
        if self.corrupt == "sha":
            digest = "0" * 64
        if self.corrupt == "flip":
            digest = hashlib.sha256(sent).hexdigest()  # consistent frames, wrong bytes: only the file compare sees it
        self.send({"id": rid, "type": "response", "result": {
            "status": status, "headers": headers, "body_encoding": "chunks", "chunks": len(pieces),
            "body_bytes": len(body), "sha256": digest, "truncated": truncated,
            "final_url": params["url"], "method": "GET"}})


class FakeConnectorTest(unittest.TestCase):
    def run_fake(self, corrupt=None, extra=()):
        files = tempfile.mkdtemp(prefix="g1-files-")
        out = tempfile.mkdtemp(prefix="g1-out-")
        ws_port, http_port = free_port(), free_port()
        proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "fake_connector.py"), "--port", str(ws_port), "--make-files", files,
             "--serve", files, "--serve-port", str(http_port), "--serve-host", "127.0.0.1", "--out", out, *extra],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        try:
            deadline = time.time() + 10
            while True:
                try:
                    board = FakeBoard(ws_port, corrupt)
                    break
                except (ConnectionRefusedError, AssertionError):
                    if time.time() > deadline:
                        raise
                    time.sleep(0.1)
            board.sock.settimeout(3)  # the plan is done once requests stop coming
            board.run()
            board.sock.close()
        finally:
            proc.terminate()
            log = proc.communicate(timeout=10)[0]
        return log, files, out

    def test_all_files_pass(self):
        log, files, out = self.run_fake()
        for name in ("10k.bin", "800k.jpg", "1m.bin", "5m.bin", "allbytes.bin"):
            self.assertIn(f"PASS {name}", log, log)
        self.assertIn("PASS text mode", log, log)
        self.assertIn("done: 0 failure(s)", log, log)
        self.assertIn("truncated=True", log)  # 5m.bin
        saved = [f for f in os.listdir(out) if f.startswith("800k.jpg.") and not f.endswith(".csv")]
        with open(os.path.join(out, saved[0]), "rb") as a, open(os.path.join(files, "800k.jpg"), "rb") as b:
            self.assertEqual(a.read(), b.read())
        csv = [f for f in os.listdir(out) if f.startswith("800k.jpg.") and f.endswith(".csv")][0]
        with open(os.path.join(out, csv)) as f:
            self.assertEqual(len(f.read().splitlines()), 1 + 100)  # header + 800 KB / 8 KB

    def test_lost_chunk_fails(self):
        log, _, _ = self.run_fake("drop", ("--only", "800k.jpg", "--no-text"))
        self.assertIn("FAIL 800k.jpg", log, log)
        self.assertIn("arrived where", log)

    def test_bad_sha_fails(self):
        log, _, _ = self.run_fake("sha", ("--only", "800k.jpg", "--no-text"))
        self.assertIn("FAIL 800k.jpg", log, log)
        self.assertIn("sha256 mismatch", log)

    def test_changed_byte_fails(self):
        log, _, _ = self.run_fake("flip", ("--only", "allbytes.bin", "--no-text"))
        self.assertIn("FAIL allbytes.bin", log, log)
        self.assertIn("bytes differ from the served file at offset 8192", log)


if __name__ == "__main__":
    unittest.main(verbosity=2)

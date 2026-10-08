#!/usr/bin/env python3
"""A minimal local Connector server for testing the T-Circle without the backend (G1). Stdlib only.

It accepts the board's dial on ws://<mac>:<port>/v1/connect, answers heartbeats, then sends
`http_request`s in chunk mode (response_mode "chunks"), reassembles the `response_chunk` frames,
checks seq order, count, body_bytes and sha256, and writes each body to disk. A served file is also
compared byte for byte with what came back.

The board dials it only in a dev build:

    idf.py -B build-fake -DCOMMA_CONNECT_URL_OVERRIDE=ws://<mac-ip>:8765/v1/connect build

Usual run (makes the test files, serves them over HTTP, fetches each once, plus one text-mode request):

    python3 tools/fake_connector.py --make-files /tmp/g1-files --serve /tmp/g1-files

During a voice call: fetch the 1 MB file again and again, every 3 s, until Ctrl-C:

    python3 tools/fake_connector.py --serve /tmp/g1-files --only 1m.bin --loop 0 --pause 3

Anything on the LAN (the camera's snapshot may answer 401: its auth is the backend's job):

    python3 tools/fake_connector.py --fetch 'http://192.168.5.33/...'

The board must allow operations (Settings › Devices) or every request comes back read-only.
--allow-ops sends device_access {"allow_operations": true}, which the board saves to NVS like a
toggle in Settings.
"""

import argparse
import asyncio
import base64
import functools
import hashlib
import http.server
import json
import os
import random
import secrets
import socketserver
import sys
import threading
import time
from urllib.parse import parse_qs, urlsplit

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC11B76"
OP_CONT, OP_TEXT, OP_BIN, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA
HEARTBEAT_S = 15
MAX_FRAME = 16 * 1024 * 1024

# The test files. 5m.bin is over the device's 4 MB cap, so it must come back truncated.
TEST_FILES = {
    "10k.bin": 10 * 1024,
    "800k.jpg": 800 * 1024,
    "1m.bin": 1024 * 1024,
    "5m.bin": 5 * 1024 * 1024,
    "allbytes.bin": 256 * 1024,
}


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def make_files(directory):
    os.makedirs(directory, exist_ok=True)
    rng = random.Random(1008)
    for name, size in TEST_FILES.items():
        if name == "allbytes.bin":
            data = bytes(range(256)) * (size // 256)
        elif name.endswith(".jpg"):
            # JPEG markers around random bytes: served as image/jpeg, incompressible like a photo.
            data = b"\xff\xd8\xff\xe0" + rng.randbytes(size - 6) + b"\xff\xd9"
        else:
            data = rng.randbytes(size)
        with open(os.path.join(directory, name), "wb") as f:
            f.write(data)
    log(f"test files in {directory}: " + ", ".join(f"{n} ({s} B)" for n, s in TEST_FILES.items()))


def serve_files(directory, port):
    handler = functools.partial(QuietHandler, directory=directory)
    httpd = socketserver.ThreadingTCPServer(("0.0.0.0", port), handler)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    log(f"serving {directory} on :{port}")
    return httpd


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log("http " + self.address_string() + " " + fmt % args)


# ---- WebSocket (RFC 6455, server side) --------------------------------------------------------------


class WebSocket:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        self.send_lock = asyncio.Lock()
        self.closed = False

    async def recv(self):
        """The next whole data message as (opcode, bytes), or None once closed."""
        parts = []
        first_op = None
        while True:
            head = await self.reader.readexactly(2)
            fin = head[0] & 0x80
            op = head[0] & 0x0F
            masked = head[1] & 0x80
            n = head[1] & 0x7F
            if n == 126:
                n = int.from_bytes(await self.reader.readexactly(2), "big")
            elif n == 127:
                n = int.from_bytes(await self.reader.readexactly(8), "big")
            if n > MAX_FRAME:
                raise ValueError(f"frame of {n} bytes")
            mask = await self.reader.readexactly(4) if masked else None
            data = await self.reader.readexactly(n)
            if not masked:
                raise ValueError("client frame not masked")
            if n:
                key = (mask * (n // 4 + 1))[:n]
                data = (int.from_bytes(data, "big") ^ int.from_bytes(key, "big")).to_bytes(n, "big")
            if op == OP_PING:
                await self.send_frame(OP_PONG, data)
                continue
            if op == OP_PONG:
                continue
            if op == OP_CLOSE:
                code = int.from_bytes(data[:2], "big") if len(data) >= 2 else 1005
                if not self.closed:
                    await self.send_frame(OP_CLOSE, data[:2])
                self.closed = True
                log(f"device closed the socket, code {code}")
                return None
            if op != OP_CONT:
                first_op = op
            parts.append(data)
            if fin:
                return first_op, b"".join(parts)

    async def send_frame(self, op, payload):
        n = len(payload)
        if n < 126:
            head = bytes([0x80 | op, n])
        elif n < 65536:
            head = bytes([0x80 | op, 126]) + n.to_bytes(2, "big")
        else:
            head = bytes([0x80 | op, 127]) + n.to_bytes(8, "big")
        async with self.send_lock:
            self.writer.write(head + payload)
            await self.writer.drain()

    async def send_json(self, obj):
        await self.send_frame(OP_TEXT, json.dumps(obj, separators=(",", ":")).encode())


async def handshake(reader, writer):
    """Reads the upgrade request and answers 101. Returns (path, query, headers) or None."""
    try:
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
    except (asyncio.IncompleteReadError, asyncio.TimeoutError, asyncio.LimitOverrunError):
        return None
    lines = head.decode("latin-1").split("\r\n")
    try:
        _, target, _ = lines[0].split(" ", 2)
    except ValueError:
        return None
    headers = {}
    for line in lines[1:]:
        if ": " in line:
            k, v = line.split(": ", 1)
            headers[k.lower()] = v
    key = headers.get("sec-websocket-key")
    if not key or "websocket" not in headers.get("upgrade", "").lower():
        writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        await writer.drain()
        return None
    accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
    writer.write(
        (
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
        ).encode()
    )
    await writer.drain()
    url = urlsplit(target)
    return url.path, parse_qs(url.query, keep_blank_values=True), headers


# ---- Connector session ----------------------------------------------------------------------------


class Pending:
    def __init__(self, req_id):
        self.id = req_id
        self.sent_at = time.monotonic()
        self.chunks = []  # (seq, arrival s since sent, raw bytes)
        self.data = bytearray()
        self.problems = []
        self.final = None
        self.done = asyncio.Event()


class Session:
    def __init__(self, ws, args, local_ip):
        self.ws = ws
        self.args = args
        self.local_ip = local_ip
        self.metadata = None
        self.metadata_event = asyncio.Event()
        self.pending = {}
        self.last_device_heartbeat = None
        self.failures = 0

    async def reader(self):
        while True:
            msg = await self.ws.recv()
            if msg is None:
                break
            op, data = msg
            if op != OP_TEXT:
                log(f"ignored a binary frame of {len(data)} B")
                continue
            try:
                frame = json.loads(data)
            except ValueError:
                log(f"invalid JSON from the device: {data[:200]!r}")
                continue
            kind = frame.get("type")
            if kind == "heartbeat":
                self.last_device_heartbeat = time.monotonic()
            elif kind == "metadata":
                self.on_metadata(frame)
            elif kind == "response_chunk":
                self.on_chunk(frame, len(data))
            elif kind in ("response", "error"):
                p = self.pending.get(frame.get("id"))
                if not p:
                    log(f"{kind} for unknown id {frame.get('id')}: {data[:300]!r}")
                    continue
                p.final = frame
                p.done.set()
            else:
                log(f"ignored frame type={kind!r}")
        for p in self.pending.values():
            p.done.set()

    def on_metadata(self, frame):
        self.metadata = frame
        caps = frame.get("capabilities", {})
        gw = caps.get("lan_gateway", {})
        host = frame.get("system_info", {}).get("hostname")
        log(
            f"metadata: hostname={host!r} scope={caps.get('scope')!r} methods={gw.get('methods')} "
            f"response_chunks={gw.get('response_chunks')}"
        )
        self.metadata_event.set()

    def on_chunk(self, frame, wire_len):
        p = self.pending.get(frame.get("id"))
        if not p:
            log(f"response_chunk for unknown id {frame.get('id')}")
            return
        seq = frame.get("seq")
        try:
            raw = base64.b64decode(frame.get("data_b64", ""), validate=True)
        except ValueError:
            p.problems.append(f"chunk {seq}: data_b64 is not valid base64")
            raw = b""
        if seq != len(p.chunks):
            p.problems.append(f"chunk seq {seq} arrived where {len(p.chunks)} was expected")
        limit = self.chunk_bytes()
        if limit and len(raw) > limit:
            p.problems.append(f"chunk {seq} carries {len(raw)} B, over chunk_bytes {limit}")
        if not raw:
            p.problems.append(f"chunk {seq} is empty")
        p.chunks.append((seq, time.monotonic() - p.sent_at, len(raw), wire_len))
        p.data += raw

    def chunk_bytes(self):
        gw = (self.metadata or {}).get("capabilities", {}).get("lan_gateway", {})
        return (gw.get("response_chunks") or {}).get("chunk_bytes")

    async def heartbeats(self):
        while not self.ws.closed:
            await asyncio.sleep(HEARTBEAT_S)
            await self.ws.send_json({"type": "heartbeat"})
            if self.last_device_heartbeat and time.monotonic() - self.last_device_heartbeat > 45:
                log("no device heartbeat for 45 s (the real server would close with 1011)")

    async def request(self, method, params, timeout):
        req_id = "req_fake_" + secrets.token_hex(6)
        p = Pending(req_id)
        self.pending[req_id] = p
        await self.ws.send_json({"id": req_id, "type": "request", "method": method, "params": params})
        try:
            await asyncio.wait_for(p.done.wait(), timeout)
        except asyncio.TimeoutError:
            p.problems.append(f"no final frame within {timeout} s ({len(p.chunks)} chunks arrived)")
        del self.pending[req_id]
        return p

    async def fetch_chunks(self, url, expected=None, label=None):
        args = self.args
        params = {"method": "GET", "url": url, "response_mode": "chunks", "timeout_seconds": args.timeout}
        if args.max_bytes:
            params["max_bytes"] = args.max_bytes
        label = label or os.path.basename(urlsplit(url).path) or "body"
        log(f"→ http_request (chunks) {url}")
        p = await self.request("http_request", params, args.timeout + 10)
        took = time.monotonic() - p.sent_at
        ok = self.check_chunked(p, expected, label)
        if p.final and p.final.get("type") == "response":
            res = p.final["result"]
            out = os.path.join(args.out, f"{label}.{p.id}")
            with open(out, "wb") as f:
                f.write(p.data)
            kbs = len(p.data) / 1024 / took if took > 0 else 0
            gaps = [b[1] - a[1] for a, b in zip(p.chunks, p.chunks[1:])]
            first = f"{p.chunks[0][1] * 1000:.0f}" if p.chunks else "-"
            log(
                f"  status={res.get('status')} chunks={res.get('chunks')} body_bytes={res.get('body_bytes')} "
                f"truncated={res.get('truncated')} content-type={res.get('headers', {}).get('Content-Type')}"
            )
            log(
                f"  {took * 1000:.0f} ms total, {kbs:.0f} KB/s, first chunk after {first} ms, "
                f"gap avg {1000 * sum(gaps) / len(gaps) if gaps else 0:.0f} ms max {1000 * max(gaps, default=0):.0f} ms, "
                f"wire {sum(c[3] for c in p.chunks)} B → {out}"
            )
            with open(out + ".chunks.csv", "w") as f:
                f.write("seq,ms_since_request,raw_bytes,wire_bytes\n")
                for seq, t, n, w in p.chunks:
                    f.write(f"{seq},{t * 1000:.1f},{n},{w}\n")
        elif p.final:
            log(f"  error: {p.final.get('error')!r} (after {len(p.chunks)} chunks, {took * 1000:.0f} ms)")
            if expected is not None:
                ok = False
        log(f"  {'PASS' if ok else 'FAIL'} {label}" + "".join(f"\n    - {x}" for x in p.problems))
        if not ok:
            self.failures += 1
        return p

    def check_chunked(self, p, expected, label):
        if not p.final:
            return False
        if p.final.get("type") != "response":
            return expected is None and not p.problems
        res = p.final.get("result", {})
        if res.get("body_encoding") != "chunks":
            p.problems.append(f"body_encoding {res.get('body_encoding')!r}")
        if "body" in res:
            p.problems.append("a text body in chunk mode")
        if res.get("chunks") != len(p.chunks):
            p.problems.append(f"final says {res.get('chunks')} chunks, {len(p.chunks)} arrived")
        if res.get("body_bytes") != len(p.data):
            p.problems.append(f"final says body_bytes {res.get('body_bytes')}, reassembled {len(p.data)}")
        digest = hashlib.sha256(p.data).hexdigest()
        if res.get("sha256") != digest:
            p.problems.append(f"sha256 mismatch: device {res.get('sha256')}, reassembled {digest}")
        if res.get("id", p.id) != p.id or p.final.get("id") != p.id:
            p.problems.append("final frame id differs")
        if expected is not None:
            cap = 4 * 1024 * 1024
            gw = (self.metadata or {}).get("capabilities", {}).get("lan_gateway", {})
            cap = (gw.get("response_chunks") or {}).get("max_total_bytes", cap)
            if self.args.max_bytes:
                cap = min(cap, self.args.max_bytes)
            want = expected[:cap]
            want_truncated = len(expected) > cap
            if bytes(p.data) != want:
                at = next((i for i, (a, b) in enumerate(zip(p.data, want)) if a != b), min(len(p.data), len(want)))
                p.problems.append(f"bytes differ from the served file at offset {at} ({len(p.data)} vs {len(want)} B)")
            if bool(res.get("truncated")) != want_truncated:
                p.problems.append(f"truncated={res.get('truncated')}, expected {want_truncated}")
            if res.get("status") != 200:
                p.problems.append(f"status {res.get('status')}")
        return not p.problems

    async def text_request(self, url):
        log(f"→ http_request (text) {url}")
        p = await self.request("http_request", {"method": "GET", "url": url, "timeout_seconds": 10}, 30)
        took = (time.monotonic() - p.sent_at) * 1000
        if p.final and p.final.get("type") == "response":
            res = p.final["result"]
            ok = "body" in res and "body_encoding" not in res and not p.chunks
            log(
                f"  status={res.get('status')} body_bytes={res.get('body_bytes')} truncated={res.get('truncated')} "
                f"{took:.0f} ms body={res.get('body', '')[:120]!r}"
            )
            log(f"  {'PASS' if ok else 'FAIL'} text mode (text body, no chunks)")
            self.failures += 0 if ok else 1
        else:
            log(f"  {(p.final or {}).get('error')!r} {p.problems}")
            self.failures += 1

    async def plan(self):
        args = self.args
        try:
            await asyncio.wait_for(self.metadata_event.wait(), 15)
        except asyncio.TimeoutError:
            log("no metadata within 15 s")
            return
        caps = self.metadata.get("capabilities", {})
        if not caps.get("lan_gateway", {}).get("response_chunks"):
            log("FAIL: metadata has no capabilities.lan_gateway.response_chunks")
            self.failures += 1
        if caps.get("scope") != "":
            if not args.allow_ops:
                log("the board is read-only: requests will be refused (use --allow-ops, or allow it in Settings)")
            else:
                log("→ device_access allow_operations=true (saved on the board)")
                p = await self.request("device_access", {"allow_operations": True}, 10)
                log(f"  {p.final}")
        targets = []  # (url, expected bytes or None, label)
        if args.serve:
            base = f"http://{args.serve_host or self.local_ip}:{args.serve_port}/"
            for name in TEST_FILES:
                path = os.path.join(args.serve, name)
                if os.path.exists(path) and (not args.only or name in args.only):
                    with open(path, "rb") as f:
                        targets.append((base + name, f.read(), name))
        for url in args.fetch:
            targets.append((url, None, None))
        rounds = 0
        while True:
            rounds += 1
            for url, expected, label in targets:
                await self.fetch_chunks(url, expected, label)
                if self.ws.closed:
                    return
            if args.serve and rounds == 1 and not args.no_text:
                # An ordinary request still gets the old text reply.
                await self.text_request(f"http://{args.serve_host or self.local_ip}:{args.serve_port}/")
            if args.loop != 0 and rounds >= args.loop:
                break
            await asyncio.sleep(args.pause)
        log(f"done: {self.failures} failure(s). The board stays connected; Ctrl-C to quit.")


async def serve_connection(reader, writer, args, done):
    peer = writer.get_extra_info("peername")
    local_ip = writer.get_extra_info("sockname")[0]
    hs = await handshake(reader, writer)
    if not hs:
        writer.close()
        return
    path, query, headers = hs
    auth = headers.get("authorization", "")
    token = auth.split()[-1] if auth else ""
    log(
        f"dial from {peer[0]} {path} name={query.get('name', [''])[0]!r} alias={query.get('alias', [''])[0]!r} "
        f"scope={query.get('scope', ['<missing>'])[0]!r} token={token[:16]}… "
        f"instance={headers.get('x-salix-connector-instance-id')}"
    )
    if token.startswith("salix_conn_") and token != "salix_conn_dev_override":
        log("WARNING: the board sent a real-looking token; the override build should send a dummy one")
    ws = WebSocket(reader, writer)
    session = Session(ws, args, local_ip)
    await ws.send_json(
        {
            "type": "connected",
            "connector_run_id": "run_fake_" + secrets.token_hex(4),
            "device_id": "dev_fake",
            "connector_id": "conn_fake",
            "owner_user_id": "usr_fake",
            "connection_generation": 1,
        }
    )
    tasks = [asyncio.create_task(session.reader()), asyncio.create_task(session.heartbeats())]
    plan = asyncio.create_task(session.plan())
    try:
        await tasks[0]
    except (asyncio.IncompleteReadError, ConnectionError, ValueError) as e:
        log(f"connection ended: {e!r}")
    finally:
        for t in tasks + [plan]:
            t.cancel()
        writer.close()
        done.append(session.failures)


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765, help="WebSocket port (default 8765)")
    ap.add_argument("--fetch", action="append", default=[], help="URL to fetch in chunk mode (repeatable)")
    ap.add_argument("--make-files", metavar="DIR", help="write the test files into DIR first")
    ap.add_argument("--serve", metavar="DIR", help="serve DIR over HTTP and fetch the test files in it")
    ap.add_argument("--serve-port", type=int, default=8000)
    ap.add_argument("--serve-host", help="this Mac's IP as the board sees it (default: from the socket)")
    ap.add_argument("--only", action="append", help="fetch only this test file (repeatable)")
    ap.add_argument("--no-text", action="store_true", help="skip the text-mode request")
    ap.add_argument("--max-bytes", type=int, help="max_bytes to send with each request")
    ap.add_argument("--timeout", type=int, default=110, help="timeout_seconds (default 110)")
    ap.add_argument("--loop", type=int, default=1, help="rounds of fetches; 0 = until Ctrl-C")
    ap.add_argument("--pause", type=float, default=3, help="seconds between rounds")
    ap.add_argument("--out", default="fake_connector_out", help="where bodies go")
    ap.add_argument("--allow-ops", action="store_true", help="send device_access true if the board is read-only")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    if args.make_files:
        make_files(args.make_files)
    if args.serve:
        serve_files(args.serve, args.serve_port)
    done = []
    server = await asyncio.start_server(
        lambda r, w: serve_connection(r, w, args, done), "0.0.0.0", args.port, limit=MAX_FRAME
    )
    log(f"fake Connector on ws://0.0.0.0:{args.port}/v1/connect — waiting for the board")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A minimal local Connector server for testing a board without the backend. Stdlib only.

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

    python3 tools/fake_connector.py --fetch 'http://192.168.1.33/...'

UDP discovery probes, Gree and miIO hellos selected within the board's advertised limits:

    python3 firmware/tools/fake_connector.py --discover-probes --discover-timeout 5000 --loop 3

Pass --discover-probes FILE.json for an array of {id, port, payload_b64} objects, e.g. a maximum-load
lab test. Results are saved under --out; logs contain only host/probe ids, byte counts and cut flags.

TCP tunnels (lan_tcp_dial): echo through a TCP echo server on this Mac (a short line, then
1 MB), pull 1 MB from a source server on this Mac, and a dial to a closed port (must be refused):

    python3 tools/fake_connector.py --tunnel

Plus an RTSP OPTIONS to a camera on your network (expects `RTSP/1.0 200 OK`):

    python3 tools/fake_connector.py --tunnel --rtsp 192.168.1.33:554

Call test (one call): the camera tunnel stays open; start a call on the board within 120 s. The board
must end the tunnel with "busy: voice call active", refuse redials during the call, and accept one
once the call is over:

    python3 tools/fake_connector.py --rtsp 192.168.1.33:554 --hold 120

The server side behaves like the backend's DeviceRelay: frames of up to 32 KB toward the board, the
board's advertised window honoured, and a tcp_ack after every data frame it takes.

The board must allow operations (Settings › Devices) or every request comes back read-only.
--allow-ops sends device_access {"allow_operations": true}, which the board saves to NVS like a
toggle in Settings.
"""

import argparse
import ssl
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

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
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


# The harness builds protocol bytes; the firmware remains protocol-agnostic.
DISCOVERY_PROBES = [
    {"id": "gree", "port": 7000, "payload_b64": base64.b64encode(b'{"t":"scan"}').decode()},
    {"id": "miio", "port": 54321, "payload_b64": base64.b64encode(bytes.fromhex("21310020") + b"\xff" * 28).decode()},
]


def discovery_params(gateway, requested=None, timeout_ms=3000):
    """Keep within all advertised limits; omit probes on old/disabled firmware."""
    params = {"timeout_ms": timeout_ms}
    limits = gateway.get("discover_probes") or {}
    keys = ("max_probes", "max_payload_bytes", "max_answer_bytes", "max_total_answer_bytes")
    if requested is None or any(type(limits.get(k)) is not int or limits[k] < 1 for k in keys):
        return params
    selected, ids, ports = [], set(), set()
    for probe in requested:
        if not isinstance(probe, dict):
            raise ValueError("each probe must be an object")
        pid, port = probe.get("id"), probe.get("port")
        if not isinstance(pid, str) or not 1 <= len(pid) <= 16 or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in pid):
            raise ValueError("probe id must be 1..16 characters of [a-z0-9_]")
        if type(port) is not int or not 1 <= port <= 65535 or pid in ids or port in ports:
            raise ValueError("probe ids/ports must be unique; ports must be integers in 1..65535")
        payload = probe.get("payload_b64")
        if not isinstance(payload, str):
            raise ValueError("probe payload_b64 must be standard base64")
        raw = base64.b64decode(payload, validate=True)
        if not raw or base64.b64encode(raw).decode() != payload:
            raise ValueError("probe payload_b64 must be nonempty, canonical standard base64")
        ids.add(pid)
        ports.add(port)
        if len(raw) <= limits["max_payload_bytes"] and len(selected) < limits["max_probes"]:
            selected.append({"id": pid, "port": port, "payload_b64": payload})
    if selected:
        params["udp_probes"] = selected
    return params


def check_discovery(result, params, gateway):
    """Check transport bounds, without interpreting or logging answer bytes."""
    problems, summaries = [], []
    if not isinstance(result, dict) or not isinstance(result.get("hosts"), list):
        return ["missing discovery hosts array"], summaries
    sent = [p["id"] for p in params.get("udp_probes", [])]
    limits = gateway.get("discover_probes") or {}
    total = 0
    for host in result["hosts"]:
        last = -1
        for answer in host.get("probes", []):
            try:
                index = sent.index(answer.get("id"))
                if index <= last:
                    raise ValueError("answer ids repeated or out of request order")
                last = index
                raw = base64.b64decode(answer["payload_b64"], validate=True)
                total += len(raw)
                if len(raw) > limits["max_answer_bytes"]:
                    problems.append("answer exceeds advertised max_answer_bytes")
                summaries.append(f"{host.get('ip')} {answer['id']}: {len(raw)} B, cut={answer.get('cut', False)}")
            except (ValueError, TypeError, KeyError, AttributeError):
                problems.append("invalid or unsolicited probe answer")
    if total > limits.get("max_total_answer_bytes", 0):
        problems.append("answers exceed advertised max_total_answer_bytes")
    if result.get("probes_truncated") and not result.get("truncated"):
        problems.append("probes_truncated requires truncated")
    return problems, summaries


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
    socketserver.ThreadingTCPServer.allow_reuse_address = True  # restart right after a run
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


# ---- TCP tunnels (lan_tcp_dial) ------------------------------------------------------------------

SERVER_TUNNEL_FRAME = 32 * 1024  # what the backend's DeviceRelay sends


class Tunnel:
    def __init__(self, tid):
        self.id = tid
        self.window = None
        self.max_frame = None
        self.sent = 0  # bytes we sent
        self.acked = 0  # what the board says it wrote
        self.data = bytearray()  # bytes from the board
        self.frames = 0
        self.largest = 0
        self.eof = None  # the eof frame
        self.problems = []
        self.changed = asyncio.Event()
        self.acks_held_until = 0  # --ack-pause: no acks before this (monotonic)

    def kick(self):
        self.changed.set()
        self.changed = asyncio.Event()


def start_echo_server(port):
    """A TCP echo server on this Mac (threaded, stdlib)."""

    class Echo(socketserver.BaseRequestHandler):
        def handle(self):
            while True:
                data = self.request.recv(65536)
                if not data:
                    return
                self.request.sendall(data)

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("0.0.0.0", port), Echo)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log(f"TCP echo server on :{port}")
    return srv


def start_source_server(port, payload):
    """Sends `payload` to whoever connects, then closes."""

    class Source(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.sendall(payload)

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("0.0.0.0", port), Source)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log(f"TCP source server on :{port} ({len(payload)} B per connection)")
    return srv


def closed_port():
    import socket

    s = socket.socket()
    s.bind(("0.0.0.0", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Session:
    def __init__(self, ws, args, local_ip):
        self.ws = ws
        self.args = args
        self.local_ip = local_ip
        self.metadata = None
        self.metadata_event = asyncio.Event()
        self.pending = {}
        self.tunnels = {}
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
            elif kind == "stream" and (frame.get("stream") or {}).get("channel") in ("tcp", "tcp_ack"):
                await self.on_tunnel_frame(frame)
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
            f"response_chunks={gw.get('response_chunks')} discover_probes={gw.get('discover_probes')}"
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

    async def request(self, method, params, timeout, req_id=None):
        req_id = req_id or "req_fake_" + secrets.token_hex(6)
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

    async def text_limit_request(self, url):
        cap = self.metadata["capabilities"]["lan_gateway"]["max_bytes"]
        p = await self.request("http_request", {"method": "GET", "url": url, "max_bytes": cap,
                                               "timeout_seconds": 30}, 40)
        res = (p.final or {}).get("result", {})
        ok = (res.get("body") == "x" * cap and res.get("body_bytes") == cap
              and res.get("truncated") is True and not p.chunks)
        log(f"{'PASS' if ok else 'FAIL'} text at limit: {cap} bytes, "
            f"body_bytes={res.get('body_bytes')}, truncated={res.get('truncated')}, "
            f"error={(p.final or {}).get('error')!r}")
        self.failures += 0 if ok else 1

    # ---- tunnels ----

    async def on_tunnel_frame(self, frame):
        t = self.tunnels.get(frame.get("id"))
        if not t:
            log(f"tunnel frame for unknown id {frame.get('id')}")
            return
        st = frame["stream"]
        if st["channel"] == "tcp_ack":
            seq = st.get("seq")
            if not isinstance(seq, int) or seq <= t.acked or seq > t.sent:
                t.problems.append(f"ack seq {seq!r} out of order (acked {t.acked}, sent {t.sent})")
            else:
                t.acked = seq
        elif st.get("eof"):
            t.eof = frame
        elif "data" in st:
            try:
                raw = base64.b64decode(st["data"], validate=True)
            except ValueError:
                t.problems.append("data frame is not valid base64")
                raw = b""
            if not raw:
                t.problems.append("empty data frame")
            if t.max_frame and len(raw) > t.max_frame:
                t.problems.append(f"data frame of {len(raw)} B, over max_frame {t.max_frame}")
            if set(frame) != {"id", "type", "stream"} or set(st) != {"channel", "data"}:
                t.problems.append(f"data frame keys {sorted(frame)} / {sorted(st)}")
            t.data += raw
            t.frames += 1
            t.largest = max(t.largest, len(raw))
            # Like DeviceRelay: written at once, acked at once (unless --ack-pause holds acks back).
            pause = self.args.ack_pause
            if pause and not t.acks_held_until and len(t.data) >= 256 * 1024:
                t.acks_held_until = time.monotonic() + pause
                log(f"  holding acks for {pause} s at {len(t.data)} B (a slow server)")
                asyncio.get_running_loop().call_later(pause, lambda: asyncio.ensure_future(self.release_acks(t)))
            if time.monotonic() >= t.acks_held_until:
                await self.ws.send_json({"id": t.id, "type": "stream", "stream": {"channel": "tcp_ack", "seq": len(t.data)}})
        t.kick()

    async def release_acks(self, t):
        log(f"  acks released at {len(t.data)} B")
        await self.ws.send_json({"id": t.id, "type": "stream", "stream": {"channel": "tcp_ack", "seq": len(t.data)}})

    async def dial(self, host, port, timeout_s=15):
        tid = "tun_fake_" + secrets.token_hex(6)
        t = Tunnel(tid)
        self.tunnels[tid] = t
        p = await self.request("lan_tcp_dial", {"host": host, "port": port, "timeout_seconds": timeout_s}, timeout_s + 10, tid)
        if p.final and p.final.get("type") == "response":
            res = p.final.get("result", {})
            t.window = res.get("window")
            t.max_frame = res.get("max_frame")
            if set(res) != {"status", "remote_address", "window", "max_frame"} or res.get("status") != "connected":
                t.problems.append(f"answer {res}")
            if not isinstance(t.window, int) or t.window < SERVER_TUNNEL_FRAME:
                t.problems.append(f"window {t.window!r}: the backend falls back to 512 KB below 32 KB")
            log(f"  connected {res.get('remote_address')} window={t.window} max_frame={t.max_frame} "
                f"({(time.monotonic() - p.sent_at) * 1000:.0f} ms)")
        return t, p

    async def tunnel_send(self, t, data):
        """Sends like DeviceRelay: frames of up to 32 KB, never more than the board's window unacked."""
        off = 0
        while off < len(data):
            room = t.window - (t.sent - t.acked)
            if room <= 0:
                changed = t.changed
                try:
                    await asyncio.wait_for(changed.wait(), self.args.ack_timeout)
                except asyncio.TimeoutError:
                    t.problems.append(f"no ack from the board for {self.args.ack_timeout} s (sent {t.sent}, acked {t.acked})")
                    return False
                if t.eof:
                    t.problems.append(f"board closed while sending: {t.eof.get('error')!r}")
                    return False
                continue
            n = min(SERVER_TUNNEL_FRAME, room, len(data) - off)
            piece = data[off:off + n]
            await self.ws.send_json({"id": t.id, "type": "stream",
                                     "stream": {"channel": "tcp", "data": base64.b64encode(piece).decode()}})
            t.sent += n
            off += n
        return True

    async def tunnel_wait(self, t, until, timeout):
        """Waits until until(t) or eof or timeout."""
        deadline = time.monotonic() + timeout
        while not until(t) and not t.eof:
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            changed = t.changed
            try:
                await asyncio.wait_for(changed.wait(), min(left, 0.5))
            except asyncio.TimeoutError:
                pass
        return until(t)

    async def tunnel_close(self, t):
        await self.ws.send_json({"id": t.id, "type": "stream", "stream": {"channel": "tcp", "eof": True}})
        await asyncio.sleep(0.3)
        if t.eof:
            t.problems.append(f"board sent eof after the server's: {t.eof}")
        del self.tunnels[t.id]

    def verdict(self, label, t, ok, extra=""):
        ok = ok and not t.problems
        log(f"  {'PASS' if ok else 'FAIL'} tunnel {label}{extra}" + "".join(f"\n    - {x}" for x in t.problems))
        if not ok:
            self.failures += 1

    async def tunnel_echo(self, host, port):
        log(f"→ lan_tcp_dial {host}:{port} (echo)")
        t, p = await self.dial(host, port)
        if not t.window:
            log(f"  FAIL tunnel echo: {(p.final or {}).get('error')!r} {p.problems}")
            self.failures += 1
            return
        line = b"hello through the T-Circle\n"
        await self.tunnel_send(t, line)
        ok = await self.tunnel_wait(t, lambda t: len(t.data) >= len(line), 10)
        ok = ok and bytes(t.data[:len(line)]) == line
        if not ok:
            t.problems.append(f"short echo: {bytes(t.data)!r}")
        big = random.Random(1009).randbytes(1024 * 1024)
        start = time.monotonic()
        t.data.clear()
        t.frames = 0
        base_sent = t.sent
        sender = asyncio.create_task(self.tunnel_send(t, big))
        stuck = lambda: sender.done() and not sender.result()  # noqa: E731
        got = await self.tunnel_wait(t, lambda t: len(t.data) >= len(big) or stuck(), 120)
        got = got and not stuck()
        took = time.monotonic() - start
        if await sender:
            await self.tunnel_wait(t, lambda t: t.acked >= base_sent + len(big), 10)
        if bytes(t.data) != big:
            at = next((i for i, (a, b) in enumerate(zip(t.data, big)) if a != b), min(len(t.data), len(big)))
            t.problems.append(f"1 MB echo differs at offset {at} ({len(t.data)} of {len(big)} B)")
        if t.acked != t.sent:
            t.problems.append(f"board acked {t.acked} of {t.sent} B written")
        extra = (f": 1 MB each way in {took:.1f} s ({len(big) / 1024 / took:.0f} KB/s), {t.frames} frames from the "
                 f"board, largest {t.largest} B")
        await self.tunnel_close(t)
        self.verdict("echo", t, ok and got, extra)

    async def tunnel_pull(self, host, port, expected):
        log(f"→ lan_tcp_dial {host}:{port} (pull {len(expected)} B)")
        start = time.monotonic()
        t, p = await self.dial(host, port)
        if not t.window:
            log(f"  FAIL tunnel pull: {(p.final or {}).get('error')!r} {p.problems}")
            self.failures += 1
            return
        await self.tunnel_wait(t, lambda t: False, 120)  # until the source closes
        took = time.monotonic() - start
        if bytes(t.data) != expected:
            at = next((i for i, (a, b) in enumerate(zip(t.data, expected)) if a != b), min(len(t.data), len(expected)))
            t.problems.append(f"pulled bytes differ at offset {at} ({len(t.data)} of {len(expected)} B)")
        if not t.eof:
            t.problems.append("no eof after the source closed")
        elif t.eof.get("error"):
            t.problems.append(f"eof carries an error: {t.eof.get('error')!r}")
        self.tunnels.pop(t.id, None)
        self.verdict("pull", t, True, f": {len(t.data)} B in {took:.1f} s ({len(t.data) / 1024 / took:.0f} KB/s), "
                                      f"{t.frames} frames, largest {t.largest} B")

    async def tunnel_refused(self, host, port):
        log(f"→ lan_tcp_dial {host}:{port} (closed port)")
        t, p = await self.dial(host, port, 5)
        self.tunnels.pop(t.id, None)
        err = (p.final or {}).get("error") or ""
        ok = (p.final or {}).get("type") == "error" and "refused" in err and "reset" not in err
        log(f"  {'PASS' if ok else 'FAIL'} tunnel refused: {err!r}")
        if not ok:
            self.failures += 1

    async def tunnel_rtsp(self, host, port):
        log(f"→ lan_tcp_dial {host}:{port} (RTSP OPTIONS)")
        t, p = await self.dial(host, port)
        if not t.window:
            log(f"  FAIL tunnel rtsp: {(p.final or {}).get('error')!r} {p.problems}")
            self.failures += 1
            return
        start = time.monotonic()
        req = f"OPTIONS rtsp://{host}:{port}/stream1 RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: comma-fake-connector\r\n\r\n"
        await self.tunnel_send(t, req.encode())
        await self.tunnel_wait(t, lambda t: b"\r\n\r\n" in t.data, 10)
        took = (time.monotonic() - start) * 1000
        head = bytes(t.data).split(b"\r\n\r\n")[0].decode("latin-1")
        ok = head.startswith("RTSP/1.0 200")
        log("  " + head.replace("\r\n", "\n  "))
        if self.args.hold > 0 and ok:
            await self.hold(t, host, port)
            return
        await self.tunnel_close(t)
        self.verdict("rtsp", t, ok, f": {took:.0f} ms round trip")

    async def hold(self, t, host, port):
        """For the call test: keep the tunnel up (an OPTIONS every 10 s) until the board ends it, e.g.
        because a call started; then redial every 5 s and log each refusal until one is accepted."""
        args = self.args
        log(f"  holding the tunnel for up to {args.hold} s: start a call on the board now")
        deadline = time.monotonic() + args.hold
        cseq = 1
        while time.monotonic() < deadline and not t.eof:
            await self.tunnel_wait(t, lambda t: False, 10)
            if t.eof:
                break
            cseq += 1
            t.data.clear()
            req = f"OPTIONS rtsp://{host}:{port}/stream1 RTSP/1.0\r\nCSeq: {cseq}\r\n\r\n"
            await self.tunnel_send(t, req.encode())
            await self.tunnel_wait(t, lambda t: b"\r\n\r\n" in t.data, 10)
            log(f"  OPTIONS {cseq}: {bytes(t.data).split(b'\r\n')[0].decode('latin-1')!r}")
        self.tunnels.pop(t.id, None)
        if not t.eof:
            log("  FAIL hold: the tunnel was still open at the end (no call started?)")
            self.failures += 1
            return
        err = t.eof.get("error") or ""
        ok = "busy: voice call active" in err
        log(f"  {'PASS' if ok else 'FAIL'} hold: the board ended the tunnel: {err!r}")
        self.failures += 0 if ok else 1
        refusals = 0
        while time.monotonic() < deadline + 600:
            await asyncio.sleep(5)
            t2, p = await self.dial(host, port, 5)
            if p.final and p.final.get("type") == "response":
                log(f"  dial accepted again after {refusals} busy refusal(s): the call is over")
                await self.tunnel_close(t2)
                break
            self.tunnels.pop(t2.id, None)
            e = (p.final or {}).get("error")
            log(f"  dial during the call: {e!r}")
            if e and "busy: voice call active" in e:
                refusals += 1
            else:
                self.failures += 1
        log(f"  {'PASS' if refusals else 'FAIL'} busy refusals during the call: {refusals}")
        self.failures += 0 if refusals else 1

    async def tunnel_plan(self):
        args = self.args
        host = args.serve_host or self.local_ip
        methods = (self.metadata or {}).get("capabilities", {}).get("lan_gateway", {}).get("methods") or []
        if "lan_tcp_dial" not in methods:
            log("SKIP tunnels: board does not advertise lan_tcp_dial")
            return
        if args.tunnel:
            await self.tunnel_echo(host, args.echo_port)
            await self.tunnel_pull(host, args.source_port, self.source_payload)
            await self.tunnel_refused(host, closed_port())
        for target in args.rtsp:
            h, _, port = target.partition(":")
            await self.tunnel_rtsp(h, int(port or 554))

    async def discover(self, gateway):
        params = discovery_params(gateway, self.args.probe_definitions, self.args.discover_timeout)
        if self.args.probe_definitions is not None and "udp_probes" not in params:
            log("SKIP probes: no compatible discover_probes capability/payload; running plain discovery")
        log(f"→ lan_discover timeout_ms={params['timeout_ms']} probes={[p['id'] for p in params.get('udp_probes', [])]}")
        p = await self.request("lan_discover", params, 15)
        res = (p.final or {}).get("result")
        problems, summaries = check_discovery(res, params, gateway)
        problems.extend(p.problems)
        if (p.final or {}).get("type") != "response":
            problems.append(f"discovery failed: {(p.final or {}).get('error')}")
        if res is not None:
            # Keep base64 available for a local verifier, but never print payloads in the log.
            path = os.path.join(self.args.out, f"discovery.{p.id}.json")
            with open(path, "w") as f:
                json.dump(res, f, separators=(",", ":"))
            log(f"  hosts={len(res.get('hosts', []))} truncated={res.get('truncated')} "
                f"probes_truncated={res.get('probes_truncated', False)} result={path}")
        for summary in summaries:
            log(f"  {summary}")
        log(f"{'FAIL' if problems else 'PASS'} discovery" + (f": {problems}" if problems else ""))
        self.failures += bool(problems)

    async def plan(self):
        args = self.args
        try:
            await asyncio.wait_for(self.metadata_event.wait(), 15)
        except asyncio.TimeoutError:
            log("no metadata within 15 s")
            return
        caps = self.metadata.get("capabilities", {})
        gateway = caps.get("lan_gateway", {})
        methods = gateway.get("methods") or []
        http_enabled = "http_request" in methods
        chunks_enabled = http_enabled and bool(gateway.get("response_chunks"))
        if not chunks_enabled:
            log("SKIP chunks: board does not advertise response_chunks")
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
            log(f"round {rounds}")
            if args.tunnel or args.rtsp:
                await self.tunnel_plan()
            if args.discover:
                if "lan_discover" in methods:
                    await self.discover(gateway)
                else:
                    log("SKIP discovery: board does not advertise lan_discover")
            if args.text_limit and args.serve and http_enabled:
                await self.text_limit_request(base + "limit.txt")
            for url, expected, label in (targets if chunks_enabled else []):
                await self.fetch_chunks(url, expected, label)
                if self.ws.closed:
                    return
            if args.serve and http_enabled and rounds == 1 and not args.no_text:
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
    session.source_payload = args.source_payload
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
    ap.add_argument("--tunnel", action="store_true", help="lan_tcp_dial checks: echo, 1 MB pull, closed port")
    ap.add_argument("--rtsp", action="append", default=[], metavar="HOST[:PORT]",
                    help="RTSP OPTIONS through a tunnel (repeatable), e.g. 192.168.1.33:554")
    ap.add_argument("--echo-port", type=int, default=7007, help="TCP echo server port for --tunnel")
    ap.add_argument("--source-port", type=int, default=7008, help="TCP source server port for --tunnel")
    ap.add_argument("--hold", type=int, default=0, metavar="S",
                    help="with --rtsp: keep the tunnel open up to S s for the call test (see hold())")
    ap.add_argument("--ack-pause", type=float, default=0, metavar="S",
                    help="hold acks back for S s once 256 KB have arrived on a tunnel (slow-server test)")
    ap.add_argument("--ack-timeout", type=float, default=15, help="seconds to wait for a tcp_ack before failing")
    ap.add_argument("--allow-ops", action="store_true", help="send device_access true if the board is read-only")
    ap.add_argument("--discover", action="store_true", help="test advertised LAN discovery each round")
    ap.add_argument("--discover-probes", nargs="?", const="builtin", metavar="JSON_FILE",
                    help="discover with Gree/miIO hellos, or probes from a JSON array; honors metadata limits")
    ap.add_argument("--discover-timeout", type=int, default=3000, metavar="MS", help="discovery window (1000..8000)")
    ap.add_argument("--text-limit", action="store_true", help="test text at advertised limit (--make-files/--serve)")
    ap.add_argument("--tls-cert", help="PEM certificate for local WSS tests")
    ap.add_argument("--tls-key", help="PEM key for local WSS tests")
    args = ap.parse_args()
    if not 1000 <= args.discover_timeout <= 8000:
        ap.error("--discover-timeout must be 1000..8000")
    args.probe_definitions = None
    if args.discover_probes:
        args.discover = True
        if args.discover_probes == "builtin":
            args.probe_definitions = DISCOVERY_PROBES
        else:
            with open(args.discover_probes) as f:
                args.probe_definitions = json.load(f)
            if not isinstance(args.probe_definitions, list) or not args.probe_definitions:
                ap.error("probe JSON file must contain a nonempty array")
    tls = None
    if args.tls_cert or args.tls_key:
        if not (args.tls_cert and args.tls_key):
            ap.error("--tls-cert and --tls-key must be supplied together")
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(args.tls_cert, args.tls_key)
    os.makedirs(args.out, exist_ok=True)
    if args.make_files:
        make_files(args.make_files)
        with open(os.path.join(args.make_files, "limit.txt"), "w") as text_file:
            text_file.write("x" * 131072)
    if args.serve:
        serve_files(args.serve, args.serve_port)
    args.source_payload = random.Random(1010).randbytes(1024 * 1024)
    if args.tunnel:
        start_echo_server(args.echo_port)
        start_source_server(args.source_port, args.source_payload)
    done = []
    server = await asyncio.start_server(
        lambda r, w: serve_connection(r, w, args, done), "0.0.0.0", args.port, limit=MAX_FRAME, ssl=tls
    )
    log(f"fake Connector on {'wss' if tls else 'ws'}://0.0.0.0:{args.port}/v1/connect — waiting for the board")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)

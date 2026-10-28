#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Show a board's screen on this computer and control it with the mouse, over USB.

For firmware with CONFIG_COMMA_SCREEN_MIRROR (ui/screen_mirror.h). Opens a page in the browser:
a click is a tap, holding the button for a second is a long press (on the idle screen: settings),
and the wheel or a vertical drag is a swipe. The board stops mirroring 30 s after this tool exits.

    python firmware/tools/screen_mirror.py --port /dev/cu.usbmodem101

Options: --save FILE writes each frame as a PNG; --log FILE keeps every other line the board
prints. Needs pyserial. Close any other serial monitor first: it would take the frames.
"""

import argparse
import base64
import http.server
import json
import struct
import sys
import threading
import time
import webbrowser
import zlib

import serial

KEEPALIVE_S = 10
GRAY = [i * 17 for i in range(16)]


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def decode(data):
    """Run-length coded levels (screen_mirror.h) to (width, height, 8-bit gray pixels)."""
    w, h = struct.unpack_from("<HH", data)
    out = bytearray()
    i = 4
    while i < len(data):
        b = data[i]
        i += 1
        run = (b & 15) + 1
        if b & 15 == 15:
            run = data[i] | data[i + 1] << 8
            i += 2
        out += bytes([GRAY[b >> 4]]) * run
    if len(out) != w * h:
        raise ValueError("frame size mismatch")
    return w, h, bytes(out)


def png(w, h, pixels, round_glass=True):
    rows = bytearray()
    r = w / 2
    for y in range(h):
        row = bytearray(pixels[y * w:(y + 1) * w])
        if round_glass:
            dy = y + 0.5 - r
            for x in range(w):
                if (x + 0.5 - r) ** 2 + dy * dy > r * r:
                    row[x] = 40
        rows += b"\0" + row
    def chunk(kind, body):
        c = kind + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>Comma screen</title>
<style>body{margin:0;background:#202020;color:#aaa;font:14px -apple-system,sans-serif;text-align:center}
img{margin:24px auto 8px;display:block;border-radius:50%;cursor:pointer;user-select:none;-webkit-user-drag:none}
#s{height:20px}</style></head><body>
<img id="f" width="699" height="699" draggable="false"><div id="s">waiting for the board...</div>
<div>click: tap &middot; hold 1 s: long press &middot; wheel or drag up/down: swipe</div>
<script>
const f=document.getElementById('f'),s=document.getElementById('s');let seq=-1,down=null,lastWheel=0;
const pt=e=>{const r=f.getBoundingClientRect();return[Math.round((e.clientX-r.left)*466/r.width),Math.round((e.clientY-r.top)*466/r.height)]};
const send=p=>fetch(p,{method:'POST'});
f.onmousedown=e=>{down={t:Date.now(),p:pt(e)};e.preventDefault()};
f.onmouseup=e=>{if(!down)return;const p=pt(e),dy=p[1]-down.p[1],held=Date.now()-down.t;
 if(Math.abs(dy)>40)send('/swipe?dir='+(dy<0?'up':'down'));
 else if(held>=900)send('/long?x='+down.p[0]+'&y='+down.p[1]);
 else send('/tap?x='+down.p[0]+'&y='+down.p[1]);down=null};
f.onwheel=e=>{e.preventDefault();const n=Date.now();if(n-lastWheel<400)return;lastWheel=n;send('/swipe?dir='+(e.deltaY>0?'up':'down'))};
async function poll(){try{const r=await(await fetch('/state')).json();s.textContent=r.status;
 if(r.seq!==seq){seq=r.seq;f.src='/frame.png?'+seq}}catch(e){s.textContent='tool stopped'}setTimeout(poll,150)}poll();
</script></body></html>"""


class Mirror:
    def __init__(self, port, save, log):
        self.port = port
        self.save = save
        self.log = log
        self.lock = threading.Lock()
        self.png = None
        self.seq = 0
        self.status = "waiting for the board..."
        self.parts = {}
        self.last_refresh = 0

    def write(self, line):
        with self.lock:
            self.port.write(line.encode() + b"\n")

    def refresh(self):
        now = time.monotonic()
        if now - self.last_refresh > 1:
            self.last_refresh = now
            self.write("comma screen refresh")

    def on_frame(self, fid, idx, total, hsh, b64):
        if fid not in self.parts:
            self.parts = {fid: {}}  # a newer frame replaces any unfinished one
        try:
            self.parts[fid][idx] = base64.b64decode(b64)
        except ValueError:
            self.refresh()
            return
        got = self.parts[fid]
        if len(got) < total:
            return
        data = b"".join(got[i] for i in range(total) if i in got)
        self.parts = {}
        if len(got) != total or fnv1a(data) != int(hsh, 16):
            self.refresh()  # damaged, for example by a log line in the middle
            return
        w, h, pixels = decode(data)
        image = png(w, h, pixels)
        self.png = image
        self.seq += 1
        self.status = "live, frame %d (%d bytes over USB)" % (fid, len(data))
        if self.save:
            with open(self.save, "wb") as f:
                f.write(image)

    def reader(self):
        buf = b""
        while True:
            buf += self.port.read(8192)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace").strip()
                if line.startswith("@@F "):
                    p = line.split(" ")
                    if len(p) == 6:
                        try:
                            self.on_frame(int(p[1]), int(p[2]), int(p[3]), p[4], p[5])
                        except (ValueError, IndexError, struct.error):
                            self.refresh()
                    else:
                        self.refresh()
                elif line.startswith("@@S off"):
                    self.status = "screen asleep: click to wake it (a tap on the idle screen also calls)"
                    self.seq += 1
                elif self.log and line:
                    self.log.write(line + "\n")
                    self.log.flush()

    def keepalive(self):
        while True:
            self.write("comma screen on")
            time.sleep(KEEPALIVE_S)


def serve(mirror, host, port):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, body, kind):
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/":
                self.reply(PAGE.encode(), "text/html; charset=utf-8")
            elif self.path == "/state":
                self.reply(json.dumps({"seq": mirror.seq, "status": mirror.status}).encode(), "application/json")
            elif self.path.startswith("/frame.png") and mirror.png:
                self.reply(mirror.png, "image/png")
            else:
                self.send_error(404)

        def do_POST(self):
            path, _, query = self.path.partition("?")
            q = dict(kv.split("=", 1) for kv in query.split("&") if "=" in kv)
            if path in ("/tap", "/long"):
                x, y = int(q.get("x", 0)), int(q.get("y", 0))
                mirror.write("comma touch %s %d %d" % ("tap" if path == "/tap" else "long", x, y))
            elif path == "/swipe" and q.get("dir") in ("up", "down"):
                mirror.write("comma touch swipe " + q["dir"])
            else:
                self.send_error(404)
                return
            self.reply(b"ok", "text/plain")

    server = http.server.ThreadingHTTPServer((host, port), Handler)
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbmodem101")
    parser.add_argument("--http", type=int, default=8466, help="local page port (default 8466)")
    parser.add_argument("--save", help="write each frame to this PNG file")
    parser.add_argument("--log", help="append the board's other output to this file")
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    port = serial.Serial(args.port, 115200, timeout=0.2)  # DTR/RTS left as opened: no reset
    mirror = Mirror(port, args.save, open(args.log, "a") if args.log else None)
    threading.Thread(target=mirror.reader, daemon=True).start()
    threading.Thread(target=mirror.keepalive, daemon=True).start()
    server = serve(mirror, "127.0.0.1", args.http)
    url = "http://127.0.0.1:%d/" % args.http
    print("screen mirror at %s (Ctrl-C to stop)" % url, flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        mirror.write("comma screen off")
        port.close()


if __name__ == "__main__":
    sys.exit(main())

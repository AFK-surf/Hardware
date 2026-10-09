# G3 · Firmware: `lan_tcp_dial`, a TCP tunnel through the T-Circle

Read first:
- `docs/connector-protocol.md`, `docs/reports/G1-report.md` and `A4-report.md`;
- the firmware: `t-circle-s3/main/connector.c`, `lan_http.c` (chunk mode, pacing, busy-during-call guard), `lan_udp*.c`.

Work on Hardware main, IDF 6.0.2.

## Why

Goal: the Comma agent can look at the office camera without the Mac ("看看办公室有没有人").

What we know (2026-10-09):
- The camera is a TP-LINK TL-IPC45AW at 192.168.5.33.
- It has **no HTTP snapshot**: ONVIF `GetSnapshotUri` answers ActionNotSupported, and port 8800 is closed.
- It only gives pictures as **RTSP** on port 554 (`/stream1` main, `/stream2` sub).
- ONVIF auth, PTZ, chunked binary transfer (G1 + #3002) and "model looks at an image" (G2) all already work.

The backend (G3) will run the RTSP client itself: DESCRIBE/SETUP/PLAY with Digest auth from the saved password, wait for the first keyframe, decode it to JPEG with ffmpeg, then reuse G2. For that it needs a **raw TCP tunnel** from the T-Circle to `camera:554`.

The Mac and station connectors already have exactly this method, `lan_tcp_dial`; the backend uses it for SSH through a gateway. **Implement the same protocol** so the backend can use the T-Circle with no special case.

## Protocol (copy of the existing one; the reference is `Cue/systems/connector/salix-connect/lan_tunnel.go`)

Read that file and `systems/station/src/connector/lan-tunnel.ts` on Cue `origin/main` first. If anything below differs from them, **the code wins**; note the difference in your report.

**Request** (server → device):

```json
{"id":"req_…","type":"request","method":"lan_tcp_dial","params":{"host":"192.168.5.33","port":554,"timeout_seconds":15}}
```

**Answer** (device → server), once the TCP connection is up:

```json
{"id":"req_…","type":"response","result":{"status":"connected","remote_address":"192.168.5.33:554","window":<W>,"max_frame":<F>}}
```

Or an `error` frame with the same `id` (connect refused / timeout / not permitted / busy).

**Then both directions** use `stream` frames under the same `id`:

```json
{"id":"req_…","type":"stream","stream":{"channel":"tcp","data":"<base64>"}}
{"id":"req_…","type":"stream","stream":{"channel":"tcp","eof":true}}
{"id":"req_…","type":"stream","error":"…","stream":{"channel":"tcp","eof":true}}
{"id":"req_…","type":"stream","stream":{"channel":"tcp_ack","seq":<n>}}
```

- `data`: bytes, at most `max_frame` each.
- `eof`: either side closed; on failure it carries `error`.
- `tcp_ack`: "I have written n bytes in total". It's sent by whichever side receives data.
- A sender keeps at most `window` bytes unacknowledged.
- The tunnel ends on eof from either side, on socket close, or on a scope or access downgrade.

The Mac uses `window` 512 KB and `max_frame` 32 KB. The T-Circle may advertise **smaller values**, since the server reads them from the answer. Choose them from what G1 learned:
- 8 KB frames always fit the socket send buffer without blocking;
- a blocked send stalls heartbeats.

So something like `max_frame` 8 KB and `window` 64–128 KB. Explain the choice.

**Capability:** add `"lan_tcp_dial"` to `capabilities.lan_gateway.methods` in metadata.

## Do

1. **Implement `lan_tcp_dial`:**
   - one or two tunnels at a time (say how many, and why);
   - dial with timeout ≤ 15 s;
   - a receive task: TCP → base64 → `stream` data frames;
   - a send path: `stream` data → TCP write → `tcp_ack`;
   - flow control by the window, in both directions;
   - close on eof, error, socket loss or access downgrade;
   - also close a tunnel that's idle with no data in either direction for, say, 60 s.
2. **Same gates as `http_request` / `udp_request`:**
   - access gate: read-only → error, no network;
   - private, link-local or own-subnet destinations only (reuse the existing check).
3. **Busy during a voice call, like G1:**
   - refuse a new tunnel while a call is dialling or connected: `error` `"busy: voice call active"`;
   - if a call starts while a tunnel is open, end it with eof and the same error.

   G1 showed that a concurrent transfer during a call froze Wi-Fi RX. Don't repeat that.
4. **Don't let TCP data pile up in Wi-Fi/lwIP RX buffers.**
   - Read the socket promptly, and let the window plus `tcp_ack` provide back-pressure toward the server.
   - Toward the camera, if the server is slow, stop reading the camera socket. TCP flow control will then slow the camera. Keep only small PSRAM buffers.
   - Measure the peak internal RAM; the floor during a call is ≥ 40 KB.
5. **Fix if real:** in G2 testing, a request to the closed port 8800 was reported as "the gateway reset".
   - Check whether a connection refused in `http_request` chunk mode (or text mode) can reboot or crash the board, and fix it if so.
   - Add a host test for connect-refused in both modes.
   - Possibly it was only a TCP RST passed on as an error message. Say which.
6. **Test without the backend:**
   - Extend `t-circle-s3/tools/fake_connector.py` (from G1) to open a tunnel and run a tiny check:
     - echo through a local TCP echo server;
     - then an RTSP `OPTIONS` to 192.168.5.33:554, which should answer `RTSP/1.0 200 OK`, readable from the office only.
   - Also pull ~1 MB through the tunnel and compare bytes.
   - Use host unit tests for framing, base64, the window/ack logic and the busy guard.
7. **The user is not in the office now.**
   - Write the code, unit tests and the fake-connector checks that run on the Mac, and commit.
   - **Don't flash.** Flashing happens when the user is back in the office; ask before each flash (serial `/dev/cu.usbmodem1101`, check with `lsof` first).
   - Plan the on-board steps so they need as few phone calls as possible.

## Report

Write `docs/reports/G3-report.md`: design, chosen window and frame sizes, RAM, host test results, and the on-board steps to run later.

Commit only your own files (explicit `git add`; other agents use this checkout). Don't push; no remote.

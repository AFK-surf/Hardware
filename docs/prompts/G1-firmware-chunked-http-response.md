# G1 · Firmware: stream large / binary HTTP responses back in chunks

Read first:
- `docs/connector-protocol.md` (§3 frames, §4 `http_request`, §6 limits);
- `t-circle-s3/main/lan_http.c` and `connector.c` (send path, TX lock, backlog);
- `docs/reports/A3-report.md` (http_request) and `V1-report.md` (send stalls).

Work on Hardware main, IDF 6.0.2.

## Why

Goal: the Comma agent can look at a LAN camera through the T-Circle ("看看办公室有没有人").

The office camera is a TP-LINK TL-IPC45AW at 192.168.5.33:
- ONVIF on port 2020, verified from a Mac with WS-Security digest auth;
- RTSP on 554.

One snapshot JPEG is **~800 KB**. Today `http_request` can't carry it:
- The body is capped at `LAN_HTTP_MAX_BYTES` (64 KB).
- It returns UTF-8 text with invalid bytes replaced, so binary data is corrupted.

The backend will compute ONVIF auth and reassemble the chunks (a separate task, G2). This step is the firmware half, and it can be built and tested now, without the backend.

## Contract (shared with G2; put it in `docs/connector-protocol.md`)

**Capability.** In `metadata`, add:

`capabilities.lan_gateway.response_chunks = {"max_total_bytes": 4194304, "chunk_bytes": 32768}`

The server only uses chunk mode on gateways that advertise this.

**Request.** `http_request` params gain `"response_mode": "chunks"`. Without it, behaviour is exactly as today: text body and 64 KB cap.

**Device → server, in chunk mode:**

1. Zero or more chunk frames, in order, `seq` starting at 0:

   ```json
   {"id":"req_…","type":"response_chunk","seq":0,"data_b64":"…"}
   ```

   Each frame carries at most `chunk_bytes` raw bytes, base64-encoded.

2. Then exactly one final frame:

   ```json
   {"id":"req_…","type":"response","result":{
     "status":200,"headers":{"Content-Type":["image/jpeg"]},
     "body_encoding":"chunks","chunks":25,"body_bytes":812662,
     "sha256":"<hex of all raw bytes>","truncated":false,
     "final_url":"http://…","method":"GET"}}
   ```

   - `truncated: true` if the body exceeded `min(max_bytes, max_total_bytes)`. Stop reading there.
   - `sha256` lets the server check what it reassembled.

3. On failure mid-stream, send one `error` frame with the same `id`. The server discards what it got.

## Do

1. **Implement chunk mode in `lan_http.c`. Stream it; never hold the whole body.**
   - Read up to `chunk_bytes`, encode and send it as one frame, then reuse the buffer.
   - Put the read and base64 buffers in PSRAM.
   - Measure the peak internal-RAM cost of one transfer. Budget: lowest internal free during a call stays ≥ 40 KB (it's ~90 KB now with V2 VAD-only).
2. **Back-pressure.**
   - Send through the existing TX path and lock.
   - Don't queue chunks faster than the socket drains: wait for each send to complete or the buffer to drain.
   - Keep the V1 no-progress timeout semantics: if a chunk can't be sent for 10 s, abort with an `error` frame.
3. **Don't hurt calls.** While a voice call is active:
   - pace chunk sends, e.g. a ≤ 64 KB/s budget, or yield to the voice sender between chunks;
   - voice uplink and downlink must not stall. Measure it: during a call, fetch a 1 MB file and watch `mic_dropped`, send times and audio.
4. **Same gates as today:** access gate (read-only → error, no network), private/link-local/own-subnet destinations only, no redirects.
5. **Advertise the capability** in metadata.
6. **Test without the backend:**
   - Write `t-circle-s3/tools/fake_connector.py`, a minimal local Connector server (websocket) that:
     - accepts the board's dial;
     - sends one `http_request` with `response_mode:"chunks"`;
     - reassembles the chunks, checks `sha256`, writes the file to disk.
   - Add a dev-only build flag to point the board at it, e.g. `-DCOMMA_CONNECT_URL_OVERRIDE=ws://<mac-ip>:<port>/v1/connect`. Default builds must be unaffected, and the board's saved staging credentials must stay untouched.
   - Serve test files from the Mac with `python3 -m http.server`: 10 KB, 800 KB JPEG, 5 MB (expect `truncated`), plus a binary file with all byte values. The reassembled file must be byte-identical.
   - If easy, also fetch the camera's snapshot URL. Its auth will be the backend's job; plain GET may return 401, and that's fine to report.
7. **Then flash the normal build back,** so the board is on staging again with the new capability. Check that ordinary `http_request` (text mode) still works through the agent, e.g. "lan_status".

Ask the user before each flash (serial `/dev/cu.usbmodem1101`; check with `lsof` that nothing holds it). Don't factory-reset.

## Report

Write `docs/reports/G1-report.md`. Include:
- throughput (KB/s), alone and during a call;
- peak internal RAM;
- chunk count and timing for the 800 KB file;
- call impact.

Commit only your own files (explicit `git add`; other agents use this checkout). Don't push; no remote.

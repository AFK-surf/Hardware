# G1 report: chunked binary `http_request` responses

2026-10-08 · `t-circle-s3/main/lan_http.c/.h`, `connector.c`, `main/CMakeLists.txt`, `tools/fake_connector.py` (new), `tools/test_fake_connector.py` (new), `tools/lan_http_test/`, `docs/connector-protocol.md` · IDF 6.0.2

> **Status:** chunk mode works on the board without a call (all files byte-identical, ~245 KB/s). **Transfers during a call are not solved** (see *Board*). The board runs the normal build on staging.

## Result

| Check | Result |
|---|---|
| Chunk mode in `lan_http.c`, streamed | **Done.** The body goes out frame by frame as it's read. Peak body RAM is one read buffer (2 KB), one raw frame buffer (32 KB) and one frame buffer (~44 KB), all in PSRAM and reused for every frame. |
| sha256, base64 | **Done.** The hash is PSA `psa_hash_*` (mbedTLS 4, hardware SHA). base64 is local (RFC 4648 vectors tested). |
| Back-pressure | **Done.** Chunks are sent from the LAN worker through the Connector's websocket client and its TX lock. Each frame is written only once the Connector socket passes lwIP's `select()` writable test, the same test V1 uses for voice (see *Design*). |
| 10 s no-progress → `error` | **Done.** No room for 10 s → `error` frame: `the T-Circle's link to the server made no progress for 10 s`. |
| Calls not hurt | **Code done; measured on the board: no.** During a call (`CONNECTING`/`UP`), sends are paced to ≤ 64 KB/s raw. |
| Same gates | **Done.** The access gate is checked at request time and again before every frame. LAN destinations only, no redirects. The code is shared with text mode. |
| Capability in metadata | **Done.** `lan_gateway.response_chunks = {"max_total_bytes":4194304,"chunk_bytes":32768}`. |
| Text mode unchanged | **Done (host).** All 99 earlier host checks pass. A request without `response_mode` never reaches the chunk path (tested). |
| Dev override flag | **Done.** `-DCOMMA_CONNECT_URL_OVERRIDE=ws://…`. The default build contains neither the override URL nor the dummy token (checked with `strings`). |
| `fake_connector.py` | **Done and tested without the board** (`test_fake_connector.py`, 4 tests). |
| Board: files byte-identical, throughput, RAM, call impact | **Pending.** |
| Normal build back on the board + text `http_request` via the agent | **Pending.** |

## Design

### Where frames are sent from

- **The LAN worker sends the chunk frames itself**, synchronously and in order, via `esp_websocket_client_send_text` (the client's own TX lock).
- **The final `response` or `error` frame** still goes through `EV_REPLY` to the Connector task, as before. It is posted only after the last chunk has been handed to the socket, so it always arrives after the chunks.
- **A new `s_client_mutex`** keeps the client alive while the worker uses it:
  - The worker holds it for one frame's write.
  - The Connector task takes it before it clears and destroys `s_client` at the end of a session.
  - A frame never goes out on a socket other than the one its request came in on: the session is checked under the mutex.

### Gate: why frames are 8 KB and not 32 KB

- **The V1 lesson:** a write that blocks while holding the websocket TX lock makes the websocket task's PONG wait for the lock (2 s `CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS`). The client then aborts the socket, and no `error` frame can be sent.
- **What `select()` guarantees:** when it says writable, lwIP has more than `TCP_SNDLOWAT` free, i.e. 12 KB of the 24 KB send buffer.
- **Frame size:** 8 KB raw is ~11 KB of JSON plus TLS overhead, so it always fits. The write never blocks, and the lock is held only for the copy.
- **Waiting:** the gate waits *outside* the lock, polling `select()` in 100 ms steps. After 10 s without room it gives up and sends the `error` frame.
- **Finding the socket:** the client doesn't expose its socket. Before each dial the Connector records which lwIP sockets are connected and to what (`getpeername`). On open, the Connector socket is the one new connection to the dial's port.
  - If it's ambiguous (e.g. a voice call dialed the same port at the same moment), the log says so. Frames are then 32 KB and written blocking with a 10 s timeout, as before V1.
  - Before every frame the device re-checks that the fd still has the same peer.
- **The contract is unchanged.** `chunk_bytes: 32768` is the advertised maximum, and frames may be smaller. `docs/connector-protocol.md` now says so explicitly ("count the frames"), and G2 counts frames anyway. Expect **100 frames for 800 KB, not 25**.

### Timeout and limits

| | Text mode | Chunk mode |
|---|---|---|
| Body cap | 64 KB | 4 MB (`min(max_bytes, 4194304)`) |
| `timeout_seconds` cap | 30 s | 110 s for the whole transfer (the server waits ≤ 120 s) |
| Body in the reply | UTF-8 text, invalid bytes replaced | `response_chunk` frames + `sha256` |

Timing at the call budget: 4 MB takes 64 s and 800 KB about 12.5 s, both inside 110 s.

### Risk to watch on the board: Wi-Fi RX buffers during a paced transfer

- **The build is zero-copy.** `CONFIG_LWIP_L2_TO_L3_COPY` is off, so received TCP data the device hasn't read yet keeps holding Wi-Fi RX buffers (64 dynamic).
- **During a call this adds up.** Pacing means the worker reads the LAN socket slowly. A fast LAN server can then fill the 64 KB TCP window: about 45 buffers pinned until they're read.
- **That could starve the voice downlink.** Watch `rx_ms` / `queue_ms` in the voice `stats` line during the call test.
- **Fixes if it shows up**, in order of preference:
  1. a lower pace;
  2. reading in smaller bites;
  3. a smaller TCP window for this one socket. That needs `LWIP_SO_RCVBUF` or a pcb tweak.

### Interaction with V1's voice-socket detection

`cue_voice.c` identifies its socket as "the one new established TCP connection" during its dial. If a chunk transfer opens its LAN connection in that same window, the call runs without V1's send gating (it logs `voice socket not identified`). This is rare (the window is the voice dial), but watch for it in the call test.

## Host tests (done)

`tools/lan_http_test/run.sh` builds the real `lan_http.c` on the Mac (ASan + UBSan) against a raw HTTP test server:

**246 passed, 0 failed.** That's the 99 earlier text-mode checks plus the chunk-mode checks:

- **Sizes:**
  - 10 KB → 2 frames;
  - 800 KB → 25 frames at 32 KB, or 100 frames at 8 KB;
  - 5 MB → truncated at exactly 4 194 304 B (128 frames);
  - 64 KB of every byte value, in 999-byte frames.
- **`max_bytes`:**
  - over the cap means the cap;
  - 100 000 → truncated;
  - exactly the body size → not truncated.
- **Body framings:** Content-Length, chunked transfer encoding with uneven pieces, and HTTP/1.0 close-delimited.
- **Frame sizes:** changing from frame to frame (1000 / 5000), and clamped above 32 KB.
- **Every frame:** exactly 4 keys, the right id and type, consecutive `seq`, valid base64, ≤ the size asked for. The reassembled bytes equal the source, and `sha256` matches CommonCrypto's.
- **Empty body:** 0 frames, sha256 of nothing; a 302 with its headers.
- **Errors:**
  - the sink fails on frame 3 → `error` frame, exactly 2 frames sent;
  - early close → `error`;
  - public destination → refused with no frames;
  - `response_mode:"binary"` → error;
  - `null` → text mode;
  - no sink → error.
- **Text mode** with a sink present: 64 KB cap, `truncated`, the sink never called.

`python3 tools/test_fake_connector.py` runs `fake_connector.py` against a Python stand-in for the board (a websocket client that streams 8 KB frames). **4 tests OK:**
- All five test files PASS, plus the text-mode request. The saved 800 KB file is byte-identical, and its per-chunk CSV has 100 rows.
- A dropped chunk is reported as a FAIL (seq gap).
- A wrong sha256 is reported as a FAIL.
- A flipped byte with a self-consistent sha256 is reported as a FAIL (`bytes differ from the served file at offset 8192`).

Builds:
- `idf.py build` (default): no warnings; app 0x1ebfe0 B, 52% of the partition free.
- `idf.py -B build-fake -DCOMMA_CONNECT_URL_OVERRIDE=ws://192.168.5.10:8765/v1/connect build`: OK. The placeholder IP must be rebuilt with the Mac's office IP.

## Board (2026-10-08 evening, office)

Board `Comma-5D28`, office Wi-Fi (`ToEverything-IOT`, ch 2, RSSI −50…−60), Mac at 192.168.5.13 running `fake_connector.py` and the file server. Flashed with `app-flash` only (NVS untouched).

### Without a call: done, all pass

| File | Frames | Bytes | Time | Throughput | Result |
|---|---|---|---|---|---|
| 10k.bin | 2 | 10 240 | 113 ms | (too small) | byte-identical |
| 800k.jpg | **100** | 819 200 | **3.27 s**, first frame after 120 ms, gap avg 32 / max 97 ms | **245 KB/s** | byte-identical |
| 1m.bin | 128 | 1 048 576 | 4.1 s | 248 KB/s | byte-identical |
| 5m.bin | 512 | **4 194 304, `truncated: true`** | 16.4 s | 250 KB/s | identical to the first 4 MB |
| allbytes.bin | 32 | 262 144 | 1.0 s | 245 KB/s | byte-identical |
| text mode `/` | — | 459, text body | 59 ms | — | old reply shape |

- Every frame passed the writable gate (`gated=yes`). Worst frame send 134 ms.
- **Peak internal RAM for a transfer: ~1.5 KB** (internal free 133 KB → 131.3 KB at the lowest).
- Camera: the board fetched `http://192.168.5.33/` (the camera's web page) in chunk mode: 200, 741 B. The ONVIF service answers a plain GET by closing the connection (it only takes SOAP POSTs with WS-Security), so the snapshot URI needs G2's ONVIF auth. Not tried with credentials.
- The first dial failed `Invalid websocket key`: `fake_connector.py` had the RFC 6455 GUID wrong, and the Python stand-in didn't check it. Both fixed; the test now checks `Sec-WebSocket-Accept`.

### During a call: **not solved**

Each row is one call with `--only 1m.bin --loop 0 --pause 3` running alongside.

| Call | Firmware | What happened |
|---|---|---|
| 1 | as committed (64 KB/s, 8 KB frames) | The first 5 s were fine. Then **the whole link froze**: voice `tcp_unacked` stuck, `rx_ms=0`, the router missed a ping, and the Connector socket (LAN only) stalled too (`send_max_ms=10012`). After 10 s the board hung up (`uplink dead`). Internal free went **133 → 91 KB**. |
| 2 | same, transfers stopped | Constant 1–3 s uplink stalls although nothing was transferring; ended 4410. The Wi-Fi stayed bad until a reboot. |
| baseline | same, nothing transferring, after reboot | **57 s, 0 mic frames dropped, worst send 185 ms.** The office network is fine. |
| 3 | + `CONFIG_LWIP_L2_TO_L3_COPY=y` | No freeze. 3 transfers completed at ~64 KB/s. 0 mic drops for 30 s, then 5–15 dropped/s while transfers ran. At 51 s **the server reset the voice TCP connection (RST)**, link healthy at the time. Cause unknown; compare V1's unexplained 65 s peer reset. |
| 4 | + copy, 4 KB frames at 32 KB/s during calls | Network clean: **0 mic frames dropped, worst send 192 ms**, transfers ran at 32 KB/s (1 MB in 32 s). But after the first answer the **mic read ~21 000 rms (clipping) for 45 s while open**, the VAD sent nothing, and the agent never heard the user. Never seen in other calls (0 s above 10 000 in the earlier logs, 5 s in call 3). Also `voice socket not identified (2 new connections)`: a transfer was connecting while the call dialed (the risk noted above). |

Reading it:
- **Call 1's freeze matches Wi-Fi RX buffer exhaustion.** Paced reads leave the LAN socket's data unread. With zero-copy RX, every unread packet holds a Wi-Fi buffer (internal RAM: −42 KB). When they run out, the radio drops everything, ACKs included. The IDF help text for `LWIP_L2_TO_L3_COPY` describes exactly this. With the copy setting the freeze was gone.
- **Call 4's clipping mic is not explained.** It showed up only on builds with the copy setting, but two calls aren't enough to blame it. CPU0 averaged 63 % in that call vs 36 % in the baseline (base64, sha256 and copies alongside voice).

### Left on the board tonight

- **Normal build, flashed 22:30**: the G1 code with 4 KB / 32 KB/s pacing during calls, **without** the copy setting (`sdkconfig` reverted to what calls were proven on). It dialed `wss://salix-staging.comma.surf` with the saved token, device `dev_jueVjQgP8FlzV8H0NpJVRg`, and metadata advertises `response_chunks`.
- **Risk until this is fixed:** once G2 is deployed and uses chunk mode during a call, call 1's freeze can happen. The server only uses chunks when it asks for a binary body, so nothing changes until then.

### Next

1. Don't read slowly at all. Pace by not *starting* the next LAN read until the frame is sent, and/or cap the LAN socket's receive window so a slow reader can't pin buffers. The window cap needs `LWIP_SO_RCVBUF` / `TCP_WND` per-pcb, or a small `SO_RCVBUF`-like trick through `tcp_recved` accounting.
2. Or make chunk mode refuse/defer during a call (`error`: "busy with a call, try after it"). It's simplest and safe, but G2's "snapshot during a call" goal would wait.
3. Repeat the copy-setting call to see whether the clipping mic follows it, and record a baseline call on the copy build with **no** transfers.
4. Ask the backend about the RST at 22:24:17 (call `vc_` in the serial log of that call).
5. Not done: `lan_status` through the agent on the normal build (needs the user to ask the agent).

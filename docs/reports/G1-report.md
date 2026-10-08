# G1 report: chunked binary `http_request` responses

2026-10-08 · `t-circle-s3/main/lan_http.c/.h`, `connector.c`, `main/CMakeLists.txt`, `tools/fake_connector.py` (new), `tools/test_fake_connector.py` (new), `tools/lan_http_test/`, `docs/connector-protocol.md` · IDF 6.0.2

> **Status: code done, host-tested. The board isn't done yet.** The board was unplugged and the user was out of the office, so nothing here has run on the T-Circle yet. The *Board* sections list what is still to measure. They get filled in when the board is plugged in again; each flash waits for the user's OK.

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

## Board (to do when it's plugged in)

Steps (ask before each flash; serial `/dev/cu.usbmodem1101`, `lsof` first; no factory reset):

1. Rebuild `build-fake` with the Mac's office IP and flash it. The board dials the fake Connector with the dummy token; the saved staging credentials stay in NVS untouched.
2. `python3 tools/fake_connector.py --make-files /tmp/g1-files --serve /tmp/g1-files`: 10 KB, 800 KB JPEG, 1 MB, 5 MB (truncated), all byte values, plus one text-mode request. All must PASS byte-identical.
3. Camera: `--fetch http://192.168.5.33/<snapshot path>`. A 401 is fine to report.
4. During a voice call (voice still goes to staging): `--only 1m.bin --loop 0 --pause 3`. Watch `mic_dropped`, `send_max_ms`, `rx_ms`/`queue_ms`, the `net` lines and the audio.
5. Flash the normal build back. Check that the board is on staging with `response_chunks` in metadata, and that a text `http_request` works through the agent ("lan_status").

| Measurement | Result |
|---|---|
| Throughput alone (KB/s) | |
| Throughput during a call (KB/s) | |
| 800 KB file: frames, total time, first-frame latency, gap avg/max | |
| Peak internal RAM: internal free before / lowest during a transfer, idle and during a call | |
| Call impact (`mic_dropped`, `send_max_ms`, downlink `rx_ms`/`queue_ms`, audio by ear) | |
| Camera snapshot GET | |
| Normal build on staging, text `http_request` via the agent | |

Each transfer logs one line on the board:
`connector: req_… chunks: N frames, B on the wire, T ms, send_max_ms=…, gated=yes, paced=no, internal before X min Y (lowest since boot Z)`, plus `lan_http: req_… -> 200, B in N chunks, T ms (K KB/s)`.

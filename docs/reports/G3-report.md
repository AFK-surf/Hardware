# G3 report: `lan_tcp_dial`, a TCP tunnel through the T-Circle

2026-10-09 · `t-circle-s3/main/lan_tunnel.c/.h` (new), `connector.c`, `lan_http.c`, `main/CMakeLists.txt`, `tools/lan_tunnel_test/` (new), `tools/lan_http_test/test.c`, `tools/fake_connector.py`, `tools/test_fake_connector.py`, `docs/connector-protocol.md` · IDF 6.0.2

> **Status:** done and verified on the board (2026-10-09, office). With the fake connector: tunnel checks, camera RTSP, slow server and the call test all pass (see *Board*). One known limitation is found: a stall when both directions block at once (see *Board*, last part). The board is back on the normal build on staging. The idle limit was raised from 60 s to 5 min at the user's request.

## Result

| Item | Result |
|---|---|
| `lan_tcp_dial`, same protocol as salix-connect / station | **Done.** One answer, then `stream` frames `tcp` / `tcp_ack` under the request id, both ways, with a window. |
| Tunnels at a time | **2** (see *Design*). |
| Dial timeout | `timeout_seconds`, capped at 15 s (a missing, ≤ 0 or > 15 value means 15). The server waits 20 s. |
| Receive task, send path, window both ways | **Done.** One task with a `select()` loop. |
| Close on eof / error / socket loss / access downgrade / 5 min idle | **Done.** |
| Access gate, LAN destinations only | **Done.** The same refusal text as the other methods; `lan_dest_check` is reused. |
| Busy during a call | **Done.** A dial during a call gets `error` `busy: voice call active` with no connect. A call that starts ends every open tunnel with `eof` + that error within one loop pass (≤ 20 ms). |
| No pile-up in Wi-Fi/lwIP RX buffers | **Done, verified on the board.** The LAN socket is read promptly, and its lwIP receive window is cut to 16 KB (see *Design*). A 35 s server stall cost no internal RAM. |
| Capability | `"lan_tcp_dial"` added to `capabilities.lan_gateway.methods`. |
| Item 5, "the gateway reset" on port 8800 | **Not a crash; a wording problem, fixed.** See *Item 5*. |
| Host tests | **Pass:** tunnel ~1160 checks, `http_request` 263, `udp_request` 747, fake connector 10 tests. |
| Fake-connector checks on the Mac | **Pass**, including RTSP `OPTIONS` to the real camera from this Mac (through the Python stand-in board, not the T-Circle). |
| Firmware builds | Default build and `build-fake`: 0 warnings, 52% of the app partition free. |

## Protocol: checked against the code (Cue `origin/main` 269f9dfb0)

I read `lan_tunnel.go`, `lan-tunnel.ts`, and also the server side that actually talks to the device: `salix_agent/ssh/device_relay.ex` and `salix_web/connector_socket.ex`. **The prompt's protocol matches**, with these points the prompt didn't state:

1. **The server ignores the device's `max_frame`.** `DeviceRelay` always sends up to its own `@max_frame` (32 KB) per frame. It reads only `window`, and only if it is ≥ 32 KB (otherwise it assumes 512 KB). So the T-Circle sends 8 KB frames but **accepts 32 KB frames from the server**. A frame over 32 KB, or one that isn't valid base64, ends the tunnel with `invalid tunnel frame`, like Go and TS.
2. **The server waits 20 s for the answer** (`SalixEnv.Protocol`: `"lan_tcp_dial" => 20_000`). A 15 s dial cap fits inside that.
3. **The gateway is picked by methods.** `LanGateway.resolve_gateway(…, ["lan_tcp_dial"])` and `control.ex`'s method list mean the T-Circle becomes eligible for `ssh.open` through a gateway too, not only for G3. See *Open points*.

Where the T-Circle differs from the Mac/station connectors, on purpose:

| | salix-connect / station | T-Circle |
|---|---|---|
| Tunnels | 8 | 2 |
| `window` / `max_frame` | 512 KB / 32 KB | 128 KB / 8 KB |
| Destinations | any | LAN only (`lan_dest`), like `http_request` |
| Idle | none | 300 s (5 min) with no data either way → `eof` `tunnel idle for 300 s` |
| Server `eof` with bytes still queued | closes at once (they are dropped) | writes what it holds for up to 2 s, then closes |
| Access turned off | `eof` "connector stopped the tunnel" | `eof` with the read-only refusal text |
| Voice call | n/a | refused / ended with `busy: voice call active` |

## Design

### Shape

- **`lan_tunnel.c`** holds all tunnel state, run by one task (`lan_tunnel`, priority 2, 6 KB stack in PSRAM) with one `select()` loop over the LAN sockets.
  - Dials are non-blocking, so a slow dial never stalls the other tunnel.
  - While any tunnel is open, the loop wakes every 20 ms to pick up commands. With none open it just blocks on the queue.
- **The Connector task** only parses and base64-decodes server frames, then passes them over a queue (PSRAM pointers): dial, data, ack, eof.
  - It answers the cheap gates itself: read-only, busy, id length.
  - The tunnel task checks access and the call again at dial time and on every pass.
- **Every frame to the server comes from the tunnel task:** answer, data, ack and eof.
  - That keeps them in order: the answer always comes before the first data.
  - Frames go out through the same writable gate as G1's chunks (`chunk_wait_room` + `send_on_session`, factored out of `chunk_send`).
  - A frame that can't be written within 10 s ends the tunnel silently, since nothing could carry an eof anyway.
- **The host's `check()`** (in `connector.c`) decides on every pass:
  - Connector socket gone → close silently;
  - read-only → eof with the refusal text;
  - call connecting or up → eof `busy: voice call active`.

### Why `max_frame` 8 KB

Same reason as G1:
- 8 KB raw is ~11 KB of JSON. That always fits in the 12 KB `select()` guarantees writable on the 24 KB send buffer.
- So a frame write never blocks while holding the websocket TX lock, and the websocket task's PING/PONG never waits behind it (the V1 failure).

### Why `window` 128 KB

- **Device → server**, the window bounds bytes *in flight*, which the server and the network hold, not the device.
  - The Cloudflare round trip is ~250 ms. 64 KB per RTT is ~256 KB/s, right at the ~245 KB/s the Connector socket managed in G1, so the window would be the bottleneck.
  - 128 KB allows ~500 KB/s, so the link stays the limit. It also clears `DeviceRelay`'s ≥ 32 KB rule.
- **Server → device**, it bounds what can queue on the T-Circle. That queue is PSRAM blocks allocated per frame as they arrive and freed as the LAN socket takes them.
  - RTSP requests are a few hundred bytes, so in practice it's tiny.
  - The worst case is 128 KB of PSRAM per tunnel.

### Why 2 tunnels

- An RTSP snapshot needs one TCP connection: DESCRIBE/SETUP/PLAY and the interleaved media all go over it.
- A second one lets a retry, or a second consumer, start while the first is still closing (eof frames crossing).
- **Cost per tunnel:**
  - one lwIP socket (10 in all; the Connector, voice, `http_request` and `udp_request`/`lan_discover` use up to 4–5);
  - up to 16 KB of pinned Wi-Fi RX buffers (next section);
  - ~0.5 KB of pcb/netconn.
- With 8, a stalled set could pin far more than the 64 dynamic RX buffers.

### Keeping TCP data out of the Wi-Fi RX buffers (item 4)

- **The problem (G1):** the build is zero-copy RX, so every received segment the device hasn't read holds a Wi-Fi buffer. The global `TCP_WND` is 64 KB, set for the voice socket.
- So "stop reading the camera when the server is slow" would let the camera fill 64 KB, about **45 of the 64 dynamic RX buffers**. That is the G1 freeze.
- **lwIP has no per-socket TCP receive window:** `SO_RCVBUF` only applies to UDP and raw sockets (checked in `api_msg.c`).
- **What the code does instead:**
  1. Right after the connect succeeds, it **cuts that pcb's receive window to 16 KB in the tcpip thread** (`tcpip_api_call` sets `rcv_wnd`, `rcv_ann_wnd` and `rcv_ann_right_edge`).
     - The LAN host hasn't sent anything yet: RTSP and SSH clients speak first; an SSH server sends a ~40 B banner.
     - So the first segment the device sends already advertises 16 KB.
     - `tcp_recved()` only gives back what the application read, so the cut lasts for the life of the connection.
  2. **Unread data is therefore ≤ 16 KB, about 11 RX buffers per tunnel**, even when the server stalls. TCP slows the camera down.
  3. On the LAN, 16 KB per ~5 ms RTT is still ~3 MB/s, far above the tunnel's ~250 KB/s.
  4. **Before close, the cut is given back.** A fully read socket then closes with FIN; lwIP sends RST if `rcv_wnd` is below max.
- The socket is otherwise read whenever the window has room, up to 4 reads (32 KB) per pass per tunnel. So nothing sits in lwIP longer than the gated send takes.
- **This touches lwIP internals** (`lwip/priv/sockets_priv.h`, `struct tcp_pcb`). It compiles on IDF 6.0.2, but **the host test can't exercise it** (no lwIP on the Mac). Board step 2 checks it. If it misbehaves, set `LAN_TUNNEL_SOCKET_WINDOW` to 0 (cut = 0, nothing changes) and fall back to window-limited reads only.

### RAM

| | Internal | PSRAM |
|---|---|---|
| Static (`.bss`), always | **1.2 KB** (`s_t`, 2 × 600 B) + ~0.2 KB queue | — |
| Task | — | 6 KB stack |
| Buffers, allocated once | — | 8 KB raw read + 11.4 KB frame |
| Per open tunnel | ~0.5 KB socket/pcb; **≤ 16 KB pinned RX buffers** worst case; small cJSON temporaries for answer/ack/eof frames | server data, ≤ 128 KB worst case (RTSP: bytes) |

- **During a call:** tunnels are refused, so the call floor (G1: 133 KB free in calls; A3: min 114 KB) drops only by the ~1.4 KB of static state. That stays well above the ≥ 40 KB required.
- **Measured peak internal RAM: not yet.** Each tunnel logs `internal min` when it closes; board step 2 reads it.

## Item 5: "the gateway reset" on port 8800

- **Verdict:** I found no crash or reboot path. The board almost certainly answered with an error **message** that read like a reset.
- **Why:** lwIP reports a refused connect (an RST in answer to the SYN) as **ECONNRESET**, not ECONNREFUSED.
  - `tcp_in.c` raises `ERR_RST`, and `err.c` maps it to `ECONNRESET`.
  - esp-tls reads it with `SO_ERROR`.
  - So `http_request` answered `connect to 192.168.5.33:8800 failed: Connection reset by peer` in both text and chunk mode.
  - The agent turned "reset by peer" into "the gateway reset".
- **Code path checked:**
  - On connect failure, `lan_http_run` takes the same `goto done`, with `error_frame` and `chunks_free`, in both modes.
  - Nothing is dereferenced that a failed connect leaves NULL.
  - The new host test runs this path under ASan + UBSan, clean.
- **Fix:** ECONNREFUSED or ECONNRESET at connect time now says `connect to 192.168.5.33:8800 refused: nothing is listening on that port`. `lan_tcp_dial` uses the same wording, and ECONNABORTED as well.
- **New host test** (`test_connect_refused`), in both modes, two ways each:
  - a real refusal (a closed loopback port);
  - lwIP's ECONNRESET, injected.
  - It checks the refused wording, that "reset" doesn't appear, and that no chunks were sent.
  - It fails 7 checks with the fix reverted.
- **Not checked:** the board's serial log from the G2 test. If the board really rebooted then, that log would show it; I have no log from that day.

## Host tests

All on the Mac, with real firmware sources and stubbed ESP headers, under ASan + UBSan.

**`tools/lan_tunnel_test/run.sh`, new: ~1160 checks, 0 failed.**
- The count varies by a few between runs because some checks sit in loops over frames.
- Five runs in a row: all passed, ~12 s each.
- The real tunnel task runs on a pthread, with a FreeRTOS queue/task stand-in. The test thread plays the Connector task and the server. Every dial is redirected to a local server.

Checks:
- **base64:** RFC 4648 vectors; strict rejection of bad padding, non-canonical bits, bad characters and odd lengths; every byte value; output bound.
- **Answer:** exactly the 4 result keys, `remote_address` = the requested `ip:port`, window and max_frame.
- **Echo:** an RTSP `OPTIONS` line echoes back, and the device acks exactly the bytes it wrote.
- **1 MB echo:** the server sends 32 KB frames inside the device's window and acks each frame. The bytes match, device frames are ≤ 8 KB (and exactly 8 KB at the largest), and the final ack equals 1 MB.
- **Window:** a source sends 1 MB with no acks; the device stops at **exactly 128 KB**. Acking 64 KB releases exactly 64 KB more. Then everything arrives byte-identical, followed by a plain eof (no error).
- **Server eof:** the LAN socket closes, and the device sends nothing more. With 100 KB still queued, all 100 KB reach the LAN host before the close.
- **Dial errors:**
  - refused → `… refused: nothing is listening on that port`;
  - a never-answered SYN with `timeout_seconds: 1` → `timed out after 1 s` in 1.0–1.5 s;
  - eof while dialing → `tunnel closed before the connection opened`.
- **Destinations and params**, all with **no connect attempted:**
  - public, loopback, the T-Circle itself, subnet broadcast;
  - missing host or params;
  - port 0, 70000, `"554"`, 55.5.
- **Gates:**
  - call at dial → busy, no connect; read-only at dial → refusal, no connect;
  - a call mid-tunnel → eof busy **in < 200 ms**, LAN socket closed;
  - read-only mid-tunnel → eof refusal;
  - Connector socket gone → LAN socket closed, no frames at all, none for the old session.
- **Limits:** a third tunnel → `tunnel capacity exhausted`; a duplicate id → `tunnel already exists`. Frames for unknown ids are ignored; non-tunnel channels are not taken.
- **Bad frames:** bad base64 → eof `invalid tunnel frame`; 32 KB + 3 → the same; exactly 32 KB → accepted and echoed.
- **Overrun:** a LAN host that never reads, plus a server that ignores the window (5 × 32 KB) → eof `tunnel window exceeded`, socket closed.
- **LAN reset:** LAN RST → eof `192.168.5.33:554 reset the connection`.
- **Idle:** built with a 2 s idle limit here → eof `tunnel idle for 2 s` at 2.0 s.

Mutation checks: each change below was made, the suite re-run, and the change reverted.

| Change | Failed checks |
|---|---|
| No mid-tunnel `check()` | 7 |
| No inbound window limit | 2 |
| No drain on server eof | 1 |
| Old refused wording | 1 |
| Window ×4 (both guards) | 1 |

**`tools/lan_http_test/run.sh`: 263 passed, 0 failed.** That's 251 before plus 12 refused checks. One older check expected the old `failed` wording and now expects `refused`.

**`tools/lan_udp_test/run.sh`: 747 checks, 0 failed** (shares `lan_dest.c`, so it was re-run).

**`python3 tools/test_fake_connector.py`: 10 tests OK.** That's the 4 G1 tests plus 6 tunnel tests against a Python stand-in board that implements `lan_tcp_dial` like the firmware (128 KB window, 8 KB frames):
- echo + 1 MB pull + refused + RTSP to a stand-in camera all PASS;
- a byte flipped in the board's data → FAIL (echo and pull);
- 16 KB board frames → FAIL `over max_frame 8192`;
- a board that never acks → FAIL `no ack from the board`;
- `--hold` through a stand-in call → PASS (busy eof, busy refusals, accepted after the call);
- `--ack-pause 1` (acks held back mid-pull) → PASS.

**Against the real camera from this Mac** (the stand-in board dialing 192.168.5.33:554 from the Mac, not the T-Circle): `RTSP/1.0 200 OK`, `Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER, SET_PARAMETER, MULTITRANS`, 19 ms round trip. `OPTIONS` needs no auth.

## `fake_connector.py` additions

- `--tunnel` starts a TCP echo server (`:7007`) and a 1 MB source server (`:7008`) on the Mac, then runs three checks:
  - **echo:** a line, then 1 MB each way, with throughput;
  - **pull:** 1 MB plus a plain eof, byte-compared;
  - **refused:** a dial to a closed port must be refused, and the message must not say "reset".
- `--rtsp HOST[:PORT]` sends RTSP `OPTIONS` through a tunnel and expects `RTSP/1.0 200`.
- `--ack-pause S` holds acks back for S s once 256 KB have arrived (a slow server, with the websocket still healthy).
- `--hold S` (with `--rtsp`) is the call test:
  - it keeps the tunnel open (an OPTIONS every 10 s) until the board ends it, and expects `busy: voice call active`;
  - it then redials every 5 s, logging each busy refusal, until a dial is accepted (call over).
- The server side behaves like `DeviceRelay`: frames ≤ 32 KB, the board's window honoured, an ack after every data frame. It checks the answer keys, frame keys and sizes against `max_frame`, base64, ack order, and `lan_tcp_dial` in metadata.

## Board (2026-10-09, office)

Board `Comma-5D28` on office Wi-Fi (RSSI −53), Mac at 192.168.5.13 running `fake_connector.py`. Every flash was `app-flash` only (NVS untouched), each one after asking the user.

### Step 1: tunnel checks, `build-fake`

| Check | Result |
|---|---|
| Echo, 1 MB each way | **PASS**, 6.2 s (166 KB/s each way at once), largest frame 8192 B |
| Pull 1 MB | **PASS**, byte-identical, 4.1 s (**249 KB/s**, G1's ceiling), plain eof |
| Closed port | **PASS**: `connect to 192.168.5.13:64367 refused: nothing is listening on that port` |
| Camera RTSP `OPTIONS` 192.168.5.33:554 | **PASS**: `RTSP/1.0 200 OK`, dial 157 ms, round trip 50 ms |
| Metadata | `methods: ["http_request","udp_request","lan_discover","lan_tcp_dial"]` |
| `receive window cut by 49151` | In the log for every tunnel: the 16 KB cut takes |

**RAM.**
- Internal free with the Connector up and no tunnel: 131.9 KB.
- Lowest sampled during tunnels (`internal min`):
  - echo / pull: 131.1 KB;
  - RTSP: 131.6 KB;
  - so **~1 KB** for these runs.
- The tunnel samples RAM after each frame it sends. The lowest value seen while traffic was stalled, both directions blocked (step 2), was **117.3 KB, ~15 KB below idle**. That matches the ≤ 16 KB of RX buffers the window cut allows.
- The "lowest since boot" (115 KB) dates from boot, before any tunnel.

### Step 2: slow server (`--ack-pause`)

- **Pull with acks held for 35 s after 256 KB: PASS.**
  - The board stopped at one window.
  - Internal free stayed at **131 559 B** in the stats lines throughout the pause.
  - Heartbeats kept going (`last_heartbeat` 8–9 s).
  - The pull finished byte-identical once acks resumed.
- **Echo with acks held: a stall, and once an RST.** This is the known limitation below.

### Step 3: the call test (one call), `--rtsp 192.168.5.33:554 --hold 120`

- **The camera tunnel was open 87 s** (OPTIONS every 10 s, all `200 OK`).
- **When the call started, the board ended it in the same millisecond** as the voice dial: `eof` `busy: voice call active`.
- **Redials during the call:** 10, all refused `busy: voice call active`.
- **After hang-up,** the first redial was accepted.
- **PASS** (0 failures).
- **The call itself was normal:**
  - agent greeting, VAD sending the user's speech;
  - internal free at its lowest in the call: **91 KB** (floor ≥ 40 KB).

### Step 4: normal build back

- The normal build was flashed at 11:19. The binary has no override URL, dummy token or debug strings.
- It dialed `wss://salix-staging.comma.surf` with the saved token: `device=dev_jueVjQgP8FlzV8H0NpJVRg`, generation 24.

### Known limitation: a stall when both directions block at once (lwIP)

**Seen with:** the echo test plus acks held back. The board stops reading the LAN host (window full toward the server), while the LAN host stops reading because it is blocked writing to the board. Both TCP windows reach 0.

**Mechanism.** Found with a debug build (`-DCOMMA_TUNNEL_DEBUG=1`, one lwIP pcb snapshot per second):
1. The board has data for the LAN host, so lwIP sends zero-window probes. **Each probe advances `snd_nxt` by 1** (`tcp_out.c:2244`).
2. When the board reads again, its pcb shows the full window open (`rcv_wnd` 16384, `ann` 16384), and lwIP sends the window update. That is a pure ACK with seq = `snd_nxt`: one past what the peer has accepted.
3. Per RFC 793, a receiver with a zero window accepts a zero-length segment only at `SEG.SEQ == RCV.NXT`. **The Mac drops the window updates.**
4. It learns about the open window only from its own persist probes, every few seconds and backing off. It sends 16 KB, stops again, and so on.
5. After a 10 s pause, data resumed only after 18 s. After a 35 s pause, the Mac reset the connection at 71 s.

**Not caused by the window cut, by this reasoning:** none of the steps depend on the receive window's size. Not A/B-tested on the board; `-DCOMMA_TUNNEL_SOCKET_WINDOW=0` builds the comparison.

**Who it affects:**
- **Not RTSP:** the camera always reads, and the board only sends it requests of a few hundred bytes.
- **Not one-way bulk:** the pull passed.
- **It needs bulk data both ways at once together with a slow server,** e.g. an `scp` through the tunnel in both directions.

**Possible fix, not done:** write to the LAN socket only as much as the peer's window has room for, read from the pcb in the tcpip thread. The rest waits in the PSRAM queue. lwIP then never enters persist on that pcb, so `snd_nxt` never runs ahead.

## Board steps as planned

Before each flash: `lsof /dev/cu.usbmodem1101` must be empty, and **ask the user first**. Flash with `app-flash` only (NVS untouched).

**Step 1: dev build, no call.**

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
cd t-circle-s3
idf.py -B build-fake -DCOMMA_CONNECT_URL_OVERRIDE=ws://<mac office ip>:8765/v1/connect build   # 192.168.5.13 today
idf.py -B build-fake -p /dev/cu.usbmodem1101 app-flash
python3 tools/fake_connector.py --allow-ops --tunnel --rtsp 192.168.5.33:554
```

- **Expect:** echo, pull, refused and rtsp all PASS.
- **Note** the KB/s for echo and pull: about 245 KB/s toward the server is G1's ceiling.
- **In the serial log,** each `lan_tunnel … closed:` line shows `send_max_ms` and `internal min`. The tunnel's peak internal use is internal free before minus `internal min`. Expect ≲ 20 KB during the pull and ~2 KB for RTSP.

**Step 2: is the window cut working?** Same build, no call.
- **Expect** the log line `connected to … (receive window cut by 49151)`.
  - 0 means the cut didn't take: report it, and `LAN_TUNNEL_SOCKET_WINDOW` falls back to off.
- **Slow-server test:** `python3 tools/fake_connector.py --tunnel --ack-pause 10`.
  - The fake server stops acking for 10 s after 256 KB of the pull, while the websocket and heartbeats stay healthy.
  - The board must stop at one window (128 KB unacked) and stop reading the source; TCP then holds the source back.
  - The board's internal RAM must not drop by more than ~20 KB during the pause.
  - Heartbeats must keep going (`last_heartbeat` in the stats line).
  - The pull must still finish byte-identical (PASS).

**Step 3: one phone call** (the only one needed).

```sh
python3 tools/fake_connector.py --rtsp 192.168.5.33:554 --hold 120
```

- Within 120 s, **tap the board to call** and stay on ~20 s, then hang up.
- **Expect:**
  - `PASS hold: … 'busy: voice call active'` within a second of the call connecting;
  - a few `dial during the call: 'busy: voice call active'`;
  - `dial accepted again`;
  - `PASS busy refusals`.
- **During the call,** the voice `stats` line should look like a normal call (no `uplink dead`, mic frames not dropped).

**Step 4: the normal build back.**
```sh
idf.py build && idf.py -p /dev/cu.usbmodem1101 app-flash
```
- The board then dials staging with its saved token, and metadata advertises `lan_tcp_dial`.
- The end-to-end "看看办公室有没有人" waits for backend G3.

## Open points

0. **The two-way stall above:** a known limitation for now, with the fix sketched there.

1. **SSH through the T-Circle.**
   - Advertising `lan_tcp_dial` makes the T-Circle a candidate gateway for `ssh.open` as well (the backend resolves gateways by method).
   - That works within the LAN rule, but the **5 min idle limit will cut an idle SSH session** unless the client sends keepalives.
   - If that matters: raise the limit for port 22, or ask the backend to send SSH keepalives (OpenSSH `ServerAliveInterval` defaults to off).
   - Also open: which gateway the backend prefers when the Mac and the T-Circle both qualify.
2. **RTSP main stream vs the link.**
   - `/stream1` from a 3 MP camera can be 2–4 Mbit/s, above the ~2 Mbit/s the Connector socket carries. The tunnel won't break: TCP slows the camera down, and the 16 KB window cut keeps that safe for RAM.
   - But the camera may drop frames, and the first keyframe can take longer.
   - `/stream2` (sub stream) is the safer default for a snapshot; the backend's call.
3. **Tunnels during calls stay off,** like G1's chunk mode, until the RX-buffer freeze is solved (G1 *Next*). "Look at the camera during a call" therefore gets `busy: voice call active`.
4. **Item 5 board check:** if the G2-day serial log still exists, look for a reboot around the port-8800 request. I didn't have it.

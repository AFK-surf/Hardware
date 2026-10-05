# A2 report: minimal Connector on the T-Circle

2026-10-05 · `t-circle-s3/main/connector.c/.h` · board `/dev/cu.usbmodem1101` on `ToEverything-IOT` · staging `wss://salix-staging.comma.surf/v1/connect`

## Result

| Check | Result |
|---|---|
| Dial, `connected`, metadata | Verified. Lands on `dev_M3Z6mxvKhQSYEwyYTber-g` (the device the token was first registered with by `salix-connect --device`), read-only (`scope=local_file_read`). |
| Shows in Settings › Devices | Verified by the user: T-Circle listed as connected. |
| Heartbeats | Every server heartbeat answered. `last_heartbeat` stays 0–14 s and there were no timeouts in about 10 min. |
| Backend drop + reconnect | Verified twice: dialing with A1's `mini_connector.py` on the same token made the server close the board's socket (1011). The board redialed within 0.8 s and 2 s, on the same device with a new `connection_generation`. The backoff doubled after the 4 s session as designed. |
| Wi-Fi drop | **Not verified.** There was no way to cut the office AP, so the user chose to skip it. The code path: Wi-Fi loss shows up as a websocket error/close or as 45 s without frames, and then the task waits on `wifi_wait()` before redialing. |
| Voice call with the Connector up | Partly. The call connected and ran normally for 75 s (two-way speech, `mic_dropped=0`, `send_ms` 12–58). At 16:57:46 it **closed with 4410**. The Connector stayed connected through the call and afterward. |
| Requests | Not exercised on staging (nothing sent the device a request). `device_access` is implemented per the spec, `http_request` and unknown methods get an `error` right away. |

### Was the 4410 the Connector?

Probably not, but one call can't prove it. The 4 s before the close match the office-Wi-Fi stall described in the plan (C1): downlink fell to `rx_ms=200` and then 700–800 ms per second, while uplink sends slowed to 106–236 ms at the same time (`mic_backlog_ms` 20 → 260, 11 frames dropped). Both directions stalled together, which points at the radio link, not the CPU or a lock. During the call the Connector did nothing but reply `{"type":"heartbeat"}` (~20 bytes) every 15 s. It has its own client, a lower priority (task 2, websocket task 3, versus voice at 5/6) and no shared lock. The last Connector frame before the close was at about 16:57:31. A clean A/B would take a few calls with `CUE_CONNECTOR_TOKEN` unset and a few with it set, in the same place.

## Heap

From the `connector: stats` line (free bytes):

| Point | Internal | PSRAM |
|---|---|---|
| Wi-Fi up, before the first dial | 85 219 | 8 202 796 |
| Connector connected, idle | 60 895 (min 59 023) | 8 180 120 |
| Connector + voice call | 25 823 (min **23 227**) | 8 080 612 |
| After the call | 60 291 | 8 115 080 |

The Connector costs about **24 KB internal** (two 6 KB task stacks, websocket buffers, lwIP/TLS socket state; mbedTLS buffers already go to PSRAM via `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`) and about 22 KB PSRAM (16 KB RX buffer and TLS). The voice call takes another ~35 KB internal. The low point of 23 KB internal during a call is workable, but A3's HTTP client will need more. If it gets tight, move the two task stacks to PSRAM (`xTaskCreateWithCaps`) or shrink the Connector's `buffer_size`.

## What the firmware does

- `connector_start()` runs right after `wifi_start()` in `main.c`. That is the only shared change: two lines in `main.c` plus the CMake source. Voice code and `wifi.c` are untouched. The task waits for an IP with the existing `wifi_wait()`.
- Credentials come from one function, `connector_credentials()`, which reads `CUE_CONNECTOR_URL` / `CUE_CONNECTOR_TOKEN` from `secrets.h`. B1 swaps this one function for NVS. When the values are missing, the Connector logs one line and stays off (the build still works).
- Dial: `connect_url?name=T-Circle&alias=tcircle&os=esp-idf&arch=xtensa&scope=<scope>`, `Authorization: Bearer`, plus a per-boot `X-Salix-Connector-Instance-ID`. The scope is sent on every dial and in every metadata frame.
- Access state lives in NVS (`connector/allow_ops`) and defaults to read-only. `device_access` saves it, re-sends metadata and only then replies `{"allows_operations": b}`.
- Every request gets an answer: `http_request` gets salix-connect's read-only refusal (or "not supported by this firmware yet" when allowed), anything else gets `unknown method: <name>`. A text frame over 16 KB is dropped; if a top-level `"id"` can be read from its first chunk, it still gets `request too large for this device`.
- Liveness: reply to every server heartbeat. 45 s without any frame → reconnect.
- Reconnect: backoff 1 s → 60 s, delay drawn from the upper half (jitter), reset after a connection that lasted ≥60 s. On a 401/403 handshake it stops until reboot (waiting for re-provisioning).
- The websocket event handler never sends. It queues frames (PSRAM copies) to the Connector task, which does all writes.

## Serial excerpts

Boot and connect:
```
16:48:27 I (1709) connector: access: read-only, instance 9d5770ac-cd95-4fa4-8bbe-f34ee71520ac
16:48:32 I (6875) wifi: got IP: 192.168.5.17
16:48:32 I (6875) connector: before first dial: heap_internal=85219 psram=8202796
16:48:32 I (6882) connector: dialing wss://salix-staging.comma.surf/v1/connect?name=T-Circle&alias=tcircle&os=esp-idf&arch=xtensa&scope=local_file_read
16:48:36 I (10531) connector: socket open
16:48:36 I (10538) connector: sent metadata scope=local_file_read
16:48:38 I (13000) connector: connected run=run_hrgxuzvu-xSRORM5 device=dev_M3Z6mxvKhQSYEwyYTber-g generation=3
16:48:57 I (31596) connector: stats connected=yes last_heartbeat=5s reconnects=0 heap_internal=60895 (min 59023) psram=8180120
```

Backend closes the socket (a second dial on the same token), and the board comes back:
```
16:50:30 W (124709) connector: closed code=1011 handshake=0 after 114s
16:50:30 I (124711) connector: reconnecting in 761 ms
16:50:35 I (130256) connector: connected run=run_ZUo8eqBSWa2aXUFc device=dev_M3Z6mxvKhQSYEwyYTber-g generation=5
16:50:40 W (134656) connector: closed code=1011 handshake=0 after 4s
16:50:40 I (134658) connector: reconnecting in 1950 ms
16:50:45 I (140373) connector: connected run=run__mtXq4bpa9c0QDJt device=dev_M3Z6mxvKhQSYEwyYTber-g generation=7
16:51:29 I (184368) connector: stats connected=yes last_heartbeat=14s reconnects=2 heap_internal=60495 (min 58499) psram=8180112
```

Voice call with the Connector up:
```
16:56:26 I (481542) cue: dialing wss://salix-staging.cue.surf/v1/agent-groups/grp1_…/voice/sessions
16:56:31 I (485785) cue: session.started call_id=vc_01M45MES2RVAW26XRP2FDSN257 format=pcm16_24k max_duration_s=3600
16:56:33 I (488376) connector: stats connected=yes last_heartbeat=2s reconnects=2 heap_internal=25823 (min 23227) psram=8080612
16:57:34 I (549376) connector: stats connected=yes last_heartbeat=3s reconnects=2 heap_internal=25823 (min 23227) psram=8078912
16:57:41 I (556630) cue: stats rx_ms=1500 rx_peak=9920 queue_ms=540 marks=5 send_ms=14 send_max_ms=42 mic_backlog_ms=20 mic_dropped=0 mic=muted mic_peak=0
16:57:42 I (557647) cue: stats rx_ms=200 rx_peak=8704 queue_ms=0 marks=0 send_ms=12 send_max_ms=79 mic_backlog_ms=40 mic_dropped=0 mic=muted mic_peak=0
16:57:43 I (558667) cue: stats rx_ms=800 rx_peak=10432 queue_ms=597 marks=6 send_ms=35 send_max_ms=216 mic_backlog_ms=260 mic_dropped=0 mic=muted mic_peak=2012
16:57:44 I (559667) cue: stats rx_ms=700 rx_peak=8064 queue_ms=305 marks=4 send_ms=106 send_max_ms=236 mic_backlog_ms=240 mic_dropped=6 mic=muted mic_peak=0
16:57:45 I (560688) cue: stats rx_ms=700 rx_peak=6656 queue_ms=252 marks=3 send_ms=119 send_max_ms=142 mic_backlog_ms=260 mic_dropped=5 mic=muted mic_peak=0
16:57:46 E (561383) cue: close 4410: playback behind (firmware bug)
16:57:46 W (561383) cue: error 4410: the client stopped reading agent audio
16:58:04 I (579379) connector: stats connected=yes last_heartbeat=3s reconnects=2 heap_internal=60291 (min 23227) psram=8115080
```

## Notes for later steps

- The user's token had already been registered by `salix-connect --device` on the Mac (that process is stopped). The firmware took over the same `device_id`. Two live clients on one token keep kicking each other off (1011 each time the other dials), so only one may run.
- Voice still points at `salix-staging.cue.surf` in `secrets.h` and works. The Connector uses `salix-staging.comma.surf`; the template now has the comma.surf URL.
- B1 (`codex/ble-provisioning`) rebases onto this commit and replaces `connector_credentials()` with its NVS reader.
- The board runs this firmware now, with the staging token from `secrets.h`.

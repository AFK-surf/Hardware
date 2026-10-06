# V1 report: voice uplink stalls

2026-10-06 · `t-circle-s3/main/cue_voice.c`, `net_watchdog.c/.h` (new), `wifi.c/.h`, `connector.c`, `main.c` · board `Comma-5D28` on `ToEverything-IOT` (192.168.5.17, BSSID `e6:60:4d:28:51:66`, ch 2) · staging

> **Where the code is.** The firmware changes for the stall fix, the instrumentation and the watchdog wiring were swept into `8f0850d` (a concurrent D1 session's commit, 16:05) while they were still uncommitted here. That commit referenced `net_watchdog.c` without containing it, so it didn't build; the V1 commit adds `net_watchdog.c/.h` and this report.

## Result

| Check | Result |
|---|---|
| Root cause | **The board's Wi-Fi link goes dead while still showing as connected** ("Wi-Fi zombie"): association, beacons and RSSI look fine, but no traffic passes in either direction. Voice suffers first because it is the only connection with continuous traffic. See *Root cause*. |
| A slow uplink degrades instead of dropping the call | **Done.** Sends are gated on the voice socket's lwIP send buffer, so nothing blocks holding the TX lock or times out; a 10 s no-progress limit replaces the 3 s send timeout. In 3 calls on the new firmware, short slowdowns (worst send 601 ms, mic frames dropped) were all ridden out. |
| `output.played` first after recovery | **Kept.** Marks go out first on every pass, and only while the socket is writable; audio waits behind them. |
| Instrumentation | **Done.** Every second during a call: `net` line with RSSI, channel, PHY mode, voice-socket TCP state. Stalls > 1 s logged with all of it (at 1, 2, 4, 8 s and when over). PHY *rate* isn't available (no getter in IDF 6.0.2). |
| Recovery from a Wi-Fi zombie | **Done, verified by simulation only.** New gateway watchdog: rejoin Wi-Fi, then restart if that doesn't help. Both paths work on the board with faked ping misses (*Watchdog*). |
| 65 s peer reset (call `vc_01M480886A6B4BVKTNP2V6ZFEW`) | **Handed to the user** to forward to the backend (salix voice logs: server or Cloudflare, and why). No answer yet. |
| RAM (lowest internal during a call ≥ 40 KB) | **Met.** Lowest internal free during calls: **105.7 KB**. Watchdog stack is in PSRAM; Connector-idle internal free unchanged at ~153 KB. |
| Test plan (5 office calls of 2+ min, 2 hotspot calls) | **Partly done; the rest is left to everyday use** (the user's call: enough calls today). See *Left to watch in everyday use*. |

## Root cause

### What the logs show

From the B1d serial log (before this work) and today's V1 log:

- **In every bad call the downlink starved too, not just the uplink.** During the stalls the playback queue ran dry (`queue_ms=0`), the speaker amp toggled on and off every few hundred ms (it switches off after 150 ms without audio), and in the last second before the 15:08:47 write timeout `rx_ms` was 0. A server-side or uplink-only problem doesn't look like that; the whole link degrades.
- **The Connector didn't notice** because it barely sends anything: a heartbeat every ~30 s survives a 10–20 s blackout.
- **Call 4 today (15:54) caught the dead link with the new instrumentation.** From 15:54:30:
  - `tcp_unacked` froze at 14000 bytes, `nrtx` went 0 → 1 → 2 and the RTO backed off 2.5 → 5 → 10 s: nothing the board sent got acknowledged.
  - Nothing was received (`rx_ms=0`) and `peer_wnd` stayed at 65535: the server wasn't refusing data, it just never got it.
  - RSSI was −56…−62 the whole time: the radio signal was fine.
  - No `wifi: disconnected` event.
- **After that the board was dead to the network for good.** For the next 2 minutes every Connector dial and voice dial ended in `ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`, and the Mac on the same subnet couldn't ping 192.168.5.17. The Mac reached `salix-staging.comma.surf` (401 in 0.2 s), Cloudflare and Baidu directly over `en0` with no proxy, so the office network and the internet were fine.
- **A reset fixed it** (15:57): IP in 7 s, Connector up, pings answered.

So the earlier "uplink stalls" and today's permanent blackout are the same thing at different lengths. The board's Wi-Fi link stops passing traffic while staying associated; short blackouts used to kill the call through the 3 s send timeout; a long one leaves the board offline until reset.

Why the link dies is still open: the ESP32 TX path, or the AP dropping the station without a deauth the board receives. The board only does 2.4 GHz, so it never joins the 5 GHz BSSID of `ToEverything-IOT`. But band steering or 802.11v/k handling on the AP side could still strand it. Every join now logs the BSSID, so a move shows up.

### Why 3 s → 10 s alone wasn't enough

In `esp_websocket_client` 1.8.0 there were three ways a short stall closed the call:

1. **Any send that times out aborts the connection.** That includes the 2 s `send_text` used for `output.played`, not just audio.
2. **A blocked audio send holds the TX lock.** If the server's PING arrives meanwhile, the websocket task waits `CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS` (2 s) for the lock to send the PONG. When that fails, the read returns `ESP_FAIL` and the client aborts. A plain 10 s send timeout would just have moved the failure here.
3. **The websocket layer polls once with the full timeout before writing a frame** (`Error transport_poll_write(0)`, the message seen at 15:08:47).

## Fixes

### Sender: wait for room, don't block (`cue_voice.c`)

- **Finding the voice socket.** Before dialing, the board records the local ports of all existing lwIP TCP connections (`tcp_active_pcbs`, read on the tcpip thread via `tcpip_api_call`; core locking is off in this build). On `WEBSOCKET_EVENT_CONNECTED`, the one new established connection is the voice socket. If there is more than one new connection, it's ambiguous: the board logs that and falls back to plain blocking sends with the 10 s ceiling. In today's calls it was found every time (local ports 62895–62898).
- **When the sender writes.** It writes only when the socket would pass lwIP's own `select()` writable test (`snd_buf > TCP_SNDLOWAT`, queue below `TCP_SNDQUEUELOWAT`). An audio frame also needs `snd_buf` ≥ frame + 256 bytes and 16 free queue slots. When that holds, neither the websocket poll nor the socket write can block, so the lock is held only for the copy. In-flight data is the same as before (it's the same test `select()` applies), so throughput doesn't change.
- **While waiting** the sender keeps trimming the mic backlog to 300 ms (oldest audio dropped). After a stall at most ~0.7 s of audio is ahead (in-flight frames + 300 ms), well inside the server's 5 s catch-up allowance (#2504).
- **`UPLINK_STALL_LIMIT_MS` = 10 s** with no room for a frame → the device hangs up, logged as `uplink dead, hanging up`. Reasoning (also in the code comment):
  - The server gives up on a client that doesn't read for 8 s (`slow_reader`).
  - An uplink that hasn't drained 100 ms of audio in 10 s almost always means the downlink is dead too, so by then the server has closed and its close code says why.
  - A shorter limit hangs up on stalls the server tolerates; a longer one only keeps a dead call on screen.
  - 10 s is also the ceiling on the send itself, in case the socket wasn't identified.
- **`network_timeout_ms` 10000**: kept. It is also the write timeout for the client's own PING/PONG, which aren't gated. A failed PING/PONG write is ignored by the client (no abort), and 10 s matches the stall limit.
- **`pingpong_timeout_sec` 20**: kept. It is longer than the stall limit, so a dead link ends with the device's own logged hangup.
- Session start/end keep a 2 s timeout.

### Instrumentation (`cue_voice.c`)

- **Every second**, next to the existing `stats` line:
  `net rssi=-61 ch=2 phy=bgn/HT20 ps=0 tcp_unacked=… (max …) tcp_sndbuf=… tcp_q=… peer_wnd=… cwnd=… nrtx_max=… rto_ms=… srtt_ms=…`
  The max values are the worst the sender saw in that second. `srtt` has 500 ms resolution (lwIP's slow timer), so it mostly reads 0 or 500.
- **Stall lines** come from the per-second loop, not the sender: in call 4 the sender itself was stuck inside a send waiting for the TX lock (held by the websocket task's PING, which was waiting for room). A stall is logged at 1, 2, 4 and 8 s, then `stall over`:
  `uplink stall waited_ms=… <wifi> <tcp> mic_backlog_ms=… mic_dropped=… rx_ms(last s)=…`
  The first V1 build logged stalls only from the sender, which is why call 4 has no stall lines; that was fixed and flashed at 15:59.
- **`send_max_ms`** now includes a send that is still waiting, so a stall shows while it lasts.
- **TCP send queue depth:** lwIP has no socket option for it; it comes from the pcb (`snd_buf`, `snd_queuelen`, `snd_nxt - lastack`).

### Gateway watchdog (`net_watchdog.c`, `wifi.c`, `connector.c`)

| | |
|---|---|
| Probe | ICMP echo to the router (the DHCP gateway) every 10 s while Wi-Fi is up. A raw socket per probe, 2 s timeout, so no socket slot is held between probes. |
| Rejoin when | 3 missed pings in a row, **or** 3 Connector dials in a row that failed before any HTTP response (handshake 0, never opened). Dial-only rejoins at most once per 10 min: a backend or internet outage can't be fixed by rejoining. |
| Rejoin | `esp_wifi_disconnect()`; the existing keep-connected handler joins again. Refused during pairing mode or a scan. |
| Restart when | no IP within 60 s of the rejoin, or the router still misses 3 pings after it. Never in the middle of a call (it waits for the call to end; a dead link ends one within 10 s). |
| Routers that drop ICMP | Until the router has answered once since boot, misses don't count for either the rejoin or the restart. The office router answers in 3–4 ms. |
| Logs | Every join: `wifi: associated bssid=… ch=…`. Rejoin: cause, miss and dial counts, BSSID, channel, RSSI. Recovery: total time, time to IP, new BSSID/channel/RSSI, `same BSSID` / `BSSID changed`. Restart: cause. |

Task stack 4 KB in PSRAM: the task does no flash writes.

## Calls

Office Wi-Fi, first V1 build (sender gating and `net` lines; stall lines still only from the sender):

| # | Start | call id | Length | End | Worst send | Mic frames dropped | RSSI |
|---|---|---|---|---|---|---|---|
| 1 | 15:49:32 | `vc_01M4830VSW9BW5BN4AJAAE6HSD` | 133.3 s | `caller_hangup`, 1000 | 277 ms | 59 | −69…−47 |
| 2 | 15:51:53 | `vc_01M48355PMRPF7Z63269WJ69NS` | 29.3 s | `caller_hangup`, 1000 | 601 ms | 75 | −60…−48 |
| 3 | 15:52:35 | `vc_01M4836ENDX5XMHS9AFGZY5X9B` | 71.7 s | `caller_hangup`, 1000 | 126 ms | 0 | −69…−52 |
| 4 | 15:53:53 | `vc_01M4838TJ0EHF6M7XB7CMDPJ1M` | 47 s | Wi-Fi zombie at 15:54:30; client read failed 15:54:40 | (frozen 10 s) | 389 | −70…−54 |

- On the old firmware, calls 1–3's slow patches would have cost dropped mic frames; with the gating none of them closed the call.
- Call 4 is the root-cause capture above. After it no call could connect until the 15:57 reset.
- One more short call (16:03, `vc_01M483T14QVMF7MNYSJ4FBNN1T`, 3.6 s, `caller_hangup`) was on the build with the stall-line fix.
- Lowest internal free during calls: **105.7 KB**.

For comparison, the B1d calls before V1 (same office Wi-Fi):
- 15:01 call: dropping ~5 mic frames/s throughout, then the peer reset at 65 s.
- 15:06 call: closed by the 3 s write timeout after 124 s (`Error transport_poll_write(0)`).

## Watchdog (simulated on the board)

The zombie state can't be produced on demand, so a bench hook fakes ping misses. It is compiled in only with `#define NET_WATCHDOG_SIMULATE 1|2` at the top of `net_watchdog.c`. (Passing `-D` through `idf.py -DCMAKE_C_FLAGS` doesn't work: IDF caches it in `build/toolchain/cflags` and ninja doesn't rebuild.)

| Test | Log |
|---|---|
| **Rejoin** (3 misses after 60 s uptime, then real pings) | 16:34:26/36/46 three misses → 16:34:47 `rejoining Wi-Fi: router stopped answering ping … bssid=e6:60:4d:28:51:66 ch=2 rssi=-49` → associated 16:34:48 → IP 16:34:51 → `recovered in 7017 ms (IP after 4017 ms) … same BSSID`. Connector came back by itself. |
| **Restart** (misses forever after 60 s) | 16:43:09–29 three misses → rejoin at 16:43:30 → IP 16:43:33 → three more misses → 16:43:57 `restarting: router still not answering after rejoining Wi-Fi` → `rst:0xc (RTC_SW_CPU_RST)` → IP 5 s later. |

The board now runs the normal build (no hook), flashed 16:45.

## Left to watch in everyday use

- **A real Wi-Fi zombie and how the watchdog recovers from it.** Look for `netdog:` lines in the serial log. Points to note:
  - how long recovery takes;
  - whether a rejoin was enough or it took a restart;
  - whether the BSSID changed (band steering or roaming on `ToEverything-IOT`).
- **Phone hotspot vs office Wi-Fi.** Not tested. If zombies only happen on the office AP, the AP (steering, station timeouts) is the prime suspect.
- **The 65 s peer reset** (`vc_01M480886A6B4BVKTNP2V6ZFEW`, RST at 15:02:14 CST / 07:02:14 UTC): waiting on the backend's answer from the salix voice logs.
- **Long calls on the stall-line build.** Office calls of 2+ minutes with the `uplink stall` lines, to see what a stall that recovers looks like: RSSI, `nrtx`, `peer_wnd`.
- **In-flight headroom.** The `srtt` readings (≤ 500 ms) don't show whether in-flight data ever limits throughput. If stalls turn out to be throughput-bound rather than blackouts, revisit the 24 KB send buffer.

## Notes

- **Serial logger:** opening the port with pyserial defaults asserts RTS, which holds the ESP32-S3 in reset (the port goes silent). Open with `dtr=False, rts=False` before `open()`.
- **The B1e session factory-reset the board at 15:20** (10 s BOOT test), and the board then hung at boot until replugged. The user re-provisioned it at 15:48.

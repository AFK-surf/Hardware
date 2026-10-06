# V1 · Firmware: find and fix the voice uplink stalls

Read first: `t-circle-s3/main/cue_voice.c` (the header comments explain every earlier fix), `git log` for `81297a7`, `docs/reports/B1-report.md` (B1c and B1d call notes) and `docs/reports/A3-report.md` (RAM, calls).

## Problem

On 2026-10-06, 3 of 5 tap-to-calls on office Wi-Fi (`ToEverything-IOT`) ended without the user hanging up. Wi-Fi stayed up and the Connector's socket kept its heartbeats the whole time; only the voice socket's uplink choked. Two failure modes:

1. **Write timeout (the firmware closes the call itself).** The first ~100 s were clean (`mic_dropped` 0, `send_max` ≤ 156 ms). Then the uplink slowed, a send blocked, `transport_poll_write(0)` timed out, and the client aborted. `SEND_TIMEOUT_MS` is 3000 (`cue_voice.c:60`), and an `esp_websocket_client` send that times out tears the connection down. The backend now tolerates much longer stalls (#2504: slow_reader 8 s, catch-up burst 5 s), so the device's own 3 s is now the tightest limit in the chain.
2. **Peer reset after 65 s** (`Connection reset by peer`). That call dropped ~5 mic frames per second throughout, with `send_max` around 300 ms, so the uplink was slow from the start.

## Do

1. **Stop killing the call on a short stall.** Make a slow uplink degrade instead of disconnect:
   - Raise the send timeout to ~10 s (stay below the server's 8 s + margin logic; reason it out in the code comment). While a send is blocked, keep capping the mic backlog (drop the oldest audio, as now) so nothing bursts past the server's 5 s allowance afterwards.
   - Confirm `output.played` marks still go out first when the link recovers.
   - Check `network_timeout_ms` (10000) and `pingpong_timeout_sec` (20) against this.
2. **Instrument, so the next stall explains itself.** Once a second during calls, log:
   - RSSI and PHY rate (`esp_wifi_sta_get_ap_info`);
   - the TCP send queue depth on the voice socket, if lwIP exposes it;
   - send time, `mic_backlog_ms`, `mic_dropped`, `rx_ms`.
   
   On a stall (send > 1 s) log one line with all of it.
3. **The 65 s peer reset:** collect the call id and time. Ask the user to forward them to the backend team (salix voice logs), and find out whether the server or Cloudflare closed it and why.
4. Test: 5 calls of 2+ minutes on office Wi-Fi with the board on USB for serial. Then 2 calls on a phone hotspot (the user enables it) to compare networks. Record per call: duration, how it ended, worst stall, RSSI range.

## Constraints

- Ask the user before flashing; the board is shared. Serial is `/dev/cu.usbmodem1101`; check nothing else holds it first.
- The board is provisioned over BLE (NVS). Don't factory-reset it.
- Don't regress RAM (lowest internal free during a call must stay ≥ 40 KB; A3 measured 114 KB).

## Report

Write `docs/reports/V1-report.md`. Commit (don't push).

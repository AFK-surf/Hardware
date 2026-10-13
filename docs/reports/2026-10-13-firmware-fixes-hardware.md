# Firmware fix validation — 2026-10-13

Working-tree fixes for the [code review](2026-10-09-code-review.md): credential-bound operation
permission, request lifetime/deadlines, HTTP interim responses and failed reads, and nonblocking
offline dialing. The five findings are grouped into four implementation areas.

## Software validation

All four normal board builds passed with ESP-IDF 6.0.2 and locked dependencies. Normal build caches
have no Connector URL override or local test CA. The device configuration suite passed 76 checks,
including failed NVS writes and credential-generation races; the call UI suite passed 14 checks
across six scenarios, including cancellation and pairing while offline.

Existing host suites also passed: HTTP 296 and UDP 762 checks for both board profiles; discovery
runner 7,605 / 4,533 (default / DevKit), discovery core 2,387 / 2,386; Connector JSON 133, resample
48,625 and microphone path 236,896. TCP tunnel tests passed (the exact count varies with scheduling).
The 26 Python tests passed, with existing unclosed-socket ResourceWarnings in simulator tests.

## T-Circle-S3

Board suffix `5D28`, ESP32-S3 revision 0.2, 8 MiB PSRAM. Normal firmware was flashed without erasing
NVS; saved setup and existing operation permission survived. The board joined its stored Wi-Fi and
connected to the staging backend. The user completed a call and confirmed microphone, playback and
tap-to-hang-up were normal. The server reported `caller_hangup`, duration 30.100 s; sampled call
statistics reported `mic_dropped=0`. Connector heartbeats continued through the call, with zero
reconnects during the normal-firmware observation before the test image was installed.

A separate `build-regression-t-circle-s3` image uses a local Connector with a dummy token. Saved
credentials are neither read out nor changed. The controlled HTTP peer records received requests,
so cancellation checks include evidence that queued side effects did not reach the destination.
After the checks, the normal firmware was restored with flash hash verification. At 20:09:36 CST
the board connected to the staging backend again with its original operation permission. The
temporary test servers were stopped. `tcircle-restore-flash.log` and `tcircle-restored-serial.log`
record this restoration and subsequent physical tests.

Passed on the board:

- Read-only HTTP refusal and operation permission changes with matching metadata.
- Separate-read `100 Continue` and same-write `103 Early Hints` followed by the final 200 response,
  in text and chunk modes; interim headers omitted.
- Close-delimited response completed by FIN succeeds; an injected TCP reset returns an error in
  both text and chunk modes.
- Advertised 65,536-byte text limit and 1 MiB chunk transfer, three rounds each, with exact binary
  payload and SHA-256 verification for chunks.
- An HTTP POST whose one-second budget expires in the worker queue never reaches the HTTP peer.
- Revoked and disconnected queued HTTP POSTs never reach the peer; new requests succeed after
  reconnection. Original operation permission is restored.
- Three rounds of UDP binary echo, including zero and high-bit bytes. Revoked and disconnected
  queued UDP datagrams never reach the controlled UDP peer.
- Three rounds of LAN discovery, TCP echo (1 MiB each direction), TCP pull (1 MiB), and refused-port
  handling. Discovery returns 14 / 16 / 17 hosts and explicitly reports `truncated=true`; this is
  bounded discovery, not evidence that every device on the subnet was found. Tunnel echo takes
  6.0–6.1 s and pull 4.2–4.3 s. No failures.

Local evidence: `../hw-logs/firmware-fixes-20261013/` relative to the repository root, including
`tcircle-serial.log`, `tcircle-regression-server.log`, `tcircle-regression.json`,
`tcircle-regression-serial.log`, `tcircle-regression-udp.json`,
`tcircle-regression-udp-server.log`, and `tcircle-gateway-server.log`.

## StickS3

Board suffix `ED20`, ESP32-S3-PICO-1 revision 0.2, embedded 8 MiB flash and 8 MiB PSRAM. The full
HTTP/UDP regression suite passed, including three rounds of 65,536-byte text, 1 MiB chunks and
binary UDP echo, queued HTTP deadline, HTTP/UDP revocation and disconnect cancellation, and recovery.
The initial operation permission was enabled and was restored after the suite.

Three rounds of discovery and TCP tunnel echo/pull/refused-port tests passed with zero failures.
Discovery returned 13 / 15 / 16 hosts with `truncated=true`. Tunnel echo (1 MiB each direction) took
6.1–6.2 s; 1 MiB pull took 4.0–4.2 s. Evidence has the prefix `sticks3-` in the same local log directory.
Normal firmware was restored with flash hash verification and connected to staging at 20:15:43 CST,
with the original permission still enabled. The user confirmed microphone, playback and KEY1
hang-up were normal. `caller_hangup` duration was 22.300 s and Connector reconnects remained zero.
Unlike the T-Circle run, some microphone-drop samples were nonzero (maximum 27 per stats interval);
the user reported no audible problem, but this is not a zero-drop result.

The user then requested louder playback. The StickS3-only codec setting was raised from -6 dB to
-3 dB; the microphone gain stays at 24 dB. The normal build passed without compiler warnings and
was flashed with hash verification; the boot log confirms `speaker -3.0 dB`. In the additional
36.600 s call, the user confirmed the volume was suitable and there was no distortion. Hang-up was
normal, Connector reconnects remained zero, and this second call logged no nonzero microphone drops.
The start of this call did include an 8 s agent-response wait (`ringback off, mic open`); it did not
disconnect. Evidence: `sticks3-volume-flash.log`, `sticks3-volume-serial.log`, and
`sticks3-volume-sha256.txt`. The board is left on this louder normal firmware.

## ESP32 DevKit

Board suffix `6AD8`, classic ESP32-D0WD revision 1.0, no PSRAM, CP2102 USB-UART. The full controlled
HTTP/UDP regression suite passed, including the board's 8,192-byte text cap, 4,096-byte chunks
carrying a 1 MiB payload, and UDP binary echo, three rounds each. Queued expiration, revocation,
disconnect cancellation and new-session recovery also passed. Permission was enabled before the
test and was restored. TCP tunnels are correctly absent from metadata and are not tested on this
board. The local Connector in this run uses WS, not TLS; after this workload the minimum free
internal heap was 99,860 bytes, recovering to 156,260 free with a 110,592-byte largest block.

Three discovery rounds passed (12 / 14 / 16 hosts, `truncated=true`, results below the 12 KiB cap).
The Mac then entered idle sleep at 20:26:03 CST and woke at 20:34:15, confirmed by `pmset` in
`devkit-host-sleep.log`. This paused the local test service and serial capture. The board's uptime
continued, its reconnect counter rose from 2 to 14, and it reconnected after the Mac woke. The fake
server reran discovery for three additional passing rounds (13 / 13 / 14 hosts). This interruption
is a host-sleep event, not evidence of a spontaneous firmware reboot. After recovery, free internal
heap was 156,172 bytes, largest block 110,592, minimum still 99,860.

Normal firmware was restored with flash hash verification, connected to staging over WSS at
20:37:28 CST, and reported fresh heartbeats with zero reconnects. Normal WSS idle heap was 130,152
bytes free, minimum 117,012, largest block 81,920. Evidence has the prefix `devkit-` in the same
local log directory. The serial monitor was closed before the board swap.

## Waveshare AMOLED

The user reattached the board reporting a blank screen and a suspected electrical problem.
USB identified board suffix `219C` (ESP32-S3-PICO-1, 8 MiB flash / 8 MiB PSRAM). The initial serial
log showed that it was running the StickS3 image (`ef89713-dirty`, `board m5stack-sticks3`), with
M5PM1 failures and an ST7789 display setup. PSRAM self-test passed. This matches the incorrect flash
and NVS erase recorded in [H4 section 0](H4-report.md#0-incident-during-the-hardware-bring-up-the-waveshare-was-flashed-by-mistake).
The board was unprovisioned on arrival.

After checking the MAC over esptool, the correct normal Waveshare bootloader, partition table and
application were flashed without an NVS erase. Flash hashes verified. At 21:29:40 CST the board
reported `board waveshare-amoled-1-32`, CST820 touch ID `0xb7`, CO5300 initialization at 466x466,
and its first frame queued in 48 ms. It entered BLE setup as an unprovisioned device. The user
then confirmed that the display had recovered. The user completed BLE setup and Wi-Fi provisioning;
the new configuration was saved at 22:00:59 CST. The first staging TLS connection timed out, then
recovered at 22:01:16; operation permission was enabled through the backend at 22:01:43.

All 35 controlled HTTP/UDP regression checks passed. These include interim responses and FIN/reset
handling in both response modes; three rounds of the 65,536-byte text cap, exact 1 MiB chunks and
binary UDP echo; queued expiration, revocation and disconnect cancellation; and successful requests
after reconnection. The original enabled operation permission was restored.

Three rounds of discovery and TCP tunnel echo/pull/refused-port tests passed with zero failures.
Discovery returned 12 / 14 / 14 hosts, each with `truncated=true`. Tunnel echo (1 MiB each direction)
took 5.8–6.1 s and 1 MiB pull took 3.9–4.1 s. After this local WS workload, free internal heap was
170,415 bytes, minimum 153,207, largest block 94,208. The two Connector reconnects were caused by
the deliberate disconnect regression and switching between test servers; the board did not reboot.

Normal firmware was restored with flash hash verification. At 22:08:21 CST it reconnected to
staging over WSS using the saved setup, with operation permission still enabled. The restored
boot initialized touch and the panel and queued its first frame in 46 ms. At 22:08:44 the Connector
reported a heartbeat 8 s old, zero reconnects, 167,279 bytes free internal heap, minimum 165,883,
and largest block 94,208. This run has no voice test: the board has no speaker and its firmware
advertises gateway functions only. Temporary servers and the serial monitor were stopped after
verification; all four boards are left on their normal firmware.

Evidence in the same local directory: `waveshare-inspect-serial.log`, `waveshare-recovery-flash.log`,
`waveshare-recovered-serial.log`, `waveshare-online-serial.log`, `waveshare-regression.json`,
`waveshare-regression-server.log`, `waveshare-regression-serial.log`, and
`waveshare-gateway-server.log`. Restoration is recorded in `waveshare-restore-flash.log` and
`waveshare-restored-serial.log`.

## Still pending

- Physical offline UI test: deferred because the user cannot disconnect the board's Wi-Fi in the
  current setup. The host simulation passed; do not count it as a physical button/touch test.
- Real-board credential replacement across identities/workspaces. Host tests cover the policy and
  failed-save paths; ordinary permission toggles do not establish this hardware result.
- The previously observed intermittent 45-second Connector silence remains unresolved. It did
  not reproduce in the normal T-Circle call/idle observation; this is not a demonstrated fix.

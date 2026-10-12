# H2b · Firmware: DevKit discovery during BLE setup, and honest `truncated`

You are continuing H2 (UDP probes in `lan_discover`) on an existing branch. Work only in the worktree
`~/Desktop/AFK/Hardware-h2`, branch `codex/discover-probes` (head `b0c3d9b`). ESP-IDF 6.0.2
(`. ~/.espressif/tools/activate_idf_v6.0.2.sh`).

Read first:
- `docs/prompts/H2-firmware-discover-udp-probes.md` (the H2 contract) and `docs/reports/H2-report.md`;
- `docs/reports/H1-report.md`, sections on the DevKit memory profile and setup order;
- `devices/README.md`, `devices/sdkconfig.esp32-devkit`;
- the firmware: `firmware/main/lan_discover.c`, `lan_discover_core.c/.h`, `provisioning.c`, `connector.c`,
  `heap_diag.h`, `memory_caps.h`, `Kconfig.projbuild`;
- the app side of setup, read-only: `~/Desktop/AFK/comma-android` (search for the BLE provisioning flow
  and what it does after the board reports `connector_up`);
- the backend, read-only: `~/Desktop/AFK/Cue` at `f0a91d861`,
  `systems/apps/salix_agent/lib/salix_agent/tools/lan_gateway.ex` (how it reads `truncated` and
  `probes_truncated`).

## Background

The ESP32 DevKit has no PSRAM. After setup it has about 93 KB of internal RAM free while BLE is up,
with a largest free block of about 45–51 KB. After BLE shuts down it has about 130–140 KB free.

The backend runs `lan_discover` (now with `udp_probes`) as soon as a gateway comes online. After a
first setup from the phone app, that happens while the phone is still connected over BLE. In the test
below the phone stayed connected for 30 s after `connector_up`.

`b0c3d9b` already reduced the DevKit host table (6 services / 6 SSDP entries per host, `host_t` 212 B).
That was enough for the scan to run with BLE up, but not for the result.

## Hardware evidence (`b0c3d9b`, DevKit, local WSS, home Wi-Fi, 2026-10-12)

The board was set up from the phone app; the fake Connector then sent three `lan_discover` calls with
probes `gree` and `miio`, 10 s apart, `timeout_ms` 5000. Heap lines are from
`CONFIG_COMMA_HEAP_DIAGNOSTICS=y` (bytes, internal RAM).

| Round | BLE | Result |
| --- | --- | --- |
| 1 | up, phone connected | scan done: 11 hosts, 3 Gree answers; then `out of memory for the discovery result`, 0 B |
| 2 | up, phone connected | same: 11 hosts, 3 Gree answers, then out of memory for the result |
| 3 | off (phone disconnected 2 s before) | PASS: 12 hosts, 3 Gree answers (399 B each, `"t":"pack"`, not cut), 10,322 B |

```text
16:31:38 heap: lan_begin       free=92776  largest=51200 min=86244
16:31:43 heap: discovery_peak  free=45904  largest=36864 min=39112
16:31:43 lan_discover: ...: 11 hosts (arp 9 of 253 swept, 0 failed; mdns 14 answers; ssdp 18 answers;
         probes 32 sent, 3 answered, 15 resent), truncated, 5009 ms, 0 B
16:32:06 prov: disconnected, reason 0x216
16:32:06 prov: BLE off
16:32:13 heap: discovery_peak  free=84872  largest=36864 min=38532
16:32:13 lan_discover: ...: 12 hosts (arp 11 of 253 swept, 0 failed; mdns 17 answers; ssdp 36 answers;
         probes 39 sent, 3 answered, 19 resent), truncated, 5010 ms, 10322 B
```

With about 46 KB free and a 36 KB largest block, a result capped at 12 KiB should fit, so the failing
allocation is not yet understood.

In every round, `truncated` and `probes_truncated` were both true, also in round 3, whose result
(10.3 KB) is under the 12 KiB cap. Round 3 had 11 ARP hosts × 2 probes = 22 first sends expected, but
`39 sent, 19 resent` means only 20 first sends went out. The code marks `probes_truncated` **and**
`truncated` for that. The backend reads `truncated` as "hosts or entries were dropped", so this is
misleading.

## Do

1. **Find the failing allocation.** Log the requested size, caps and largest free block when an
   allocation fails during discovery (the failed-alloc callback is already installed in `app_main`).
   Report which allocation it is and why it failed with 36 KB contiguous free.
2. **Keep discovery from running while BLE holds memory, on boards without PSRAM.** Board-local; no
   protocol, app or backend change. A Kconfig option, enabled in the DevKit overlay, off by default so
   T-Circle does not change.
   - (a) After the `connector_up` status has been notified over BLE, end the BLE session a few seconds
     later (for example 3 s) instead of waiting for the phone to disconnect. First check in
     `comma-android` what the app does after `connector_up`, and state in the report why ending BLE
     then is safe for its flow (it must not show a setup failure).
   - (b) If `lan_discover` arrives while BLE is still up, wait for BLE to go off, bounded (about 10 s),
     before allocating anything. If BLE is still up after that, run anyway and fail as today.
   - The setup order itself (Connector starts while BLE is up, so `connector_failed` can still be
     reported) stays as it is.
3. **Fix the cause of 1 as well**, if it is a real bug or waste (for example a buffer that could be
   freed before the result is built), and not only the BLE timing.
4. **Make `truncated` honest.**
   - Add `send_failed` and `unsent` counts to the discovery log line, and log the first `errno` of a
     failed `sendto`.
   - A failed send is retried on a later tick within the send window, and is only counted as unsent if
     it is still unsent at the end.
   - Unsent or unanswered probes, a full answer budget, or a cut answer set **only** `probes_truncated`.
     `truncated` keeps its existing meaning: a host, service, SSDP entry or string was dropped, or the
     result cap rolled back a host. A host rolled back at the result cap still sets both.
   - Update `docs/connector-protocol.md` and the H2 report so the two flags are defined this way.
5. **Tests.** Extend `firmware/tools/lan_discover_test` (default and `--devkit`): failed sends retried,
   `unsent` at the end, `truncated` vs `probes_truncated` in each case, and the BLE-wait logic if it can
   be tested on the host. Keep every existing suite passing (C suites under ASan/UBSan, the Python suite
   in `tools/.venv`), and build both boards with `tools/board.sh`. Show that the T-Circle generated
   sdkconfig differs only by any new keys at their defaults.

## Rules

- Do not flash either board. Claude retests on hardware after your commit, so write in the report the
  exact steps and the log lines to expect, for: setup from the app followed immediately by discovery
  with probes (BLE up), three rounds with BLE off, and the new log counts.
- No change to the protocol beyond the meaning of the two flags above, nor to the app or backend.
- Do not commit Wi-Fi passwords, tokens or test certificates.
- One commit on `codex/discover-probes`. Don't merge, don't push.

## Report

Append a section to `docs/reports/H2-report.md`: the failing allocation and its cause, what you
changed and why, the BLE timing and why it is safe for the app, the new flag semantics, pacing and
memory numbers if they changed, and test counts. Reply with the commit hash and a short summary.

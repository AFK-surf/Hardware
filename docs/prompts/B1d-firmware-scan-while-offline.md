# B1d · Firmware: Wi-Fi scan must work when the stored network is unreachable

Read first: `~/Desktop/AFK/Hardware/docs/reports/B1-report.md` (sections B1b and B1c), `docs/ble-provisioning.md`. Work on Hardware main (`t-circle-s3`, IDF 6.0.2). Serial: `/dev/cu.usbmodem101`.

## Problem

This is the open issue from B1c. A provisioned board whose stored network is gone (moved house, router changed) enters pairing mode, but `wifi_scan` answers "busy" forever, so the app can't list networks and the only way out is a factory reset. Changing Wi-Fi is the main reason pairing mode exists, so this blocks a core flow. F2 (go back to the stored network after a failed try) makes it more likely to hit.

Likely cause, to confirm: the station keeps retrying the stored network, and ESP-IDF refuses or aborts scans while a connect is in progress.

## Fix

- While a BLE client is connected in pairing mode, stop automatic reconnect attempts to the stored network so scans can run. Resume them when the client disconnects or pairing mode ends (keep F2's "go back right away" behaviour).
- A scan request must always get an answer within ~10 s: either a list (possibly empty) or a real error. Never return "busy" forever. If a connect is in flight, cancel it (`esp_wifi_disconnect`) before scanning, and retry once.
- Keep the stored credentials untouched until new ones connect (B1c rule).

## Verify on the board (ask the user before flashing)

1. Provisioned board, stored network reachable: pairing mode → scan works → leave → back online (regression).
2. Stored network unreachable: provision the board to a network that then disappears (a phone hotspot the user turns off works well) → pairing mode → scan lists the office networks within 10 s → provision `ToEverything-IOT` from the app or `tools/ble_provision.py` → online.
3. One tap-to-call afterwards.

## Report

Append a "B1d" section to `docs/reports/B1-report.md`. Commit (don't push).

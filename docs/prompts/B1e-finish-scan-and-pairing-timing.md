# B1e · Firmware: finish B1d (scan while offline) + relax pairing timing

The B1d session closed before testing or committing. Pick it up, then do one more pairing change.

Read first: `docs/prompts/B1d-firmware-scan-while-offline.md`, `docs/reports/B1-report.md` (B1b, B1c), `docs/reports/B2-report.md` (B2d section), `docs/ble-provisioning.md`. Work on Hardware main (`~/Desktop/AFK/Hardware/t-circle-s3`, IDF 6.0.2). Serial: `/dev/cu.usbmodem101`.

## State

- B1d's work is **uncommitted** in the main checkout: `t-circle-s3/main/wifi.c`, `wifi.h` and `provisioning.c` (~65 lines), plus `docs/ble-provisioning.md`. The intent: while an app is connected in pairing mode, hold reconnects to the stored network; cancel an in-flight attempt for a scan; resume when the app leaves or pairing mode ends; `busy` only during a `wifi_credentials` trial (≤ ~30 s). Review it with `git diff`, finish it, don't discard it.
- The board is provisioned as `dev_KokfkxljPn6yuBAjq3ucJA` (read-only), by the Android app in B2d. `dev_DYYh…` and earlier IDs are stale.

## 1. Finish B1d

Verify on the board (ask the user before flashing):
1. Stored network reachable: pairing mode → scan works → leave → back online.
2. Stored network gone: the user turns on a phone hotspot, you provision the board to it, the user turns the hotspot off → pairing mode → scan lists the office networks within 10 s → provision `ToEverything-IOT` → online.
3. One tap-to-call afterwards. If a call drops with sends slowing then blocking, record the time, close code and `send_ms`/`mic_backlog_ms`.

## 2. Pairing timing (from the B2d real-device test)

The user often failed to finish pairing in time. There are two 30 s windows: one from the board's security request to tapping Pair, and one from the code appearing on screen to finishing entry.
- Don't request security as soon as a client connects. Let pairing start when the phone first touches an encrypted characteristic, so the app controls the timing right after its own instructions. Check this still works with the app's scan-before-connect trick (B2d section A1).
- Widen the passkey and pairing timeouts to about 90 s, if NimBLE allows it.
- Keep the code on screen until pairing succeeds or times out, and show "Pairing timed out" on screen when it does.

Verify with the Android app: add the device from factory state twice, once waiting ~60 s before tapping Pair.

## Report and commit

Append "B1d" and "B1e" sections to `docs/reports/B1-report.md`. Update `docs/ble-provisioning.md` if the pairing trigger changes. Commit (don't push). Leave the board provisioned and read-only, and tell the user the final `device_id`.

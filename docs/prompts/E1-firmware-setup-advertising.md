# E1 · Firmware: say "I need setup" in the BLE advertising

Read first: `docs/ble-provisioning.md` (the BLE contract) and `t-circle-s3/main/provisioning.c` (`advertise()`). Work on Hardware main, IDF 6.0.2.

## Why

New feature, three parts (E1 firmware, E2 Android, E3 backend):
1. The user plugs in a new T-Circle and opens the Comma Android app.
2. The app scans briefly over BLE.
3. If an unprovisioned Comma device is nearby, the Comma agent posts a message in the chat: "I found a Comma device nearby (Comma-5D28). Set it up?", with a **Connect** button that opens Add device with that device already chosen.

The product ships overseas (Pixel, Samsung), and the scan only runs while the app is open, so no background scanning. To tell "a new device that needs setup" from "the user's own device in pairing mode", and to name the device without connecting to it, the phone needs a few bytes in the advertising.

## Contract (shared with E2)

Keep the advertising data as it is: flags + the 128-bit service UUID. The app's scan filter stays on that UUID.

Add **manufacturer-specific data** to the **scan response**, next to the name. The scan response has room; the advertising packet is nearly full.

| Byte | Field | Value |
|---|---|---|
| 0–1 | company id (little endian) | `0xFFFF` for now (the Bluetooth SIG id reserved for testing; a real id comes before production) |
| 2 | format version | `0x01` |
| 3 | model | `0x01` = T-Circle-S3 |
| 4 | state | `0x00` = unprovisioned, `0x01` = provisioned, pairing mode |
| 5–6 | short id | the last two bytes of the Wi-Fi MAC, the same as the `XXXX` in `Comma-XXXX` (`5D28` → `0x5D, 0x28`) |

The phone ignores data with an unknown version, so later versions can append fields.

## Do

1. Implement the table above in `advertise()`. Check that the scan response still fits in 31 bytes. Log the bytes once at advertising start.
2. Update `docs/ble-provisioning.md` → *Advertising*: the table, plus the note on the test company id.
3. Update `t-circle-s3/tools/ble_provision.py` (or add a `--scan` mode) so it prints name, state and short id from the scan. It's the test without a phone.
4. **Verify** (ask before flashing; serial `/dev/cu.usbmodem1101`, check with `lsof` that nothing holds it):
   - unprovisioned (only if the user agrees to a factory reset; otherwise skip this and say so) → state `0x00`;
   - provisioned, pairing mode (hold BOOT 3 s) → state `0x01`;
   - the B2 Android Add device flow still finds and sets up the device (the user does this on the phone).

## Don't

- Don't change the GATT service, pairing or security.
- Don't change when the device advertises (unprovisioned: always; provisioned: only in pairing mode; never in a call).

## Report

Write `docs/reports/E1-report.md`. Commit only your own files (`git add` explicit paths; other agents use this checkout). Don't push; the repo has no remote.

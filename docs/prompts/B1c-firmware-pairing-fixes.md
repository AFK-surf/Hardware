# B1c · Firmware: two pairing fixes from the B2c test

Read first: `~/Desktop/AFK/Hardware/docs/reports/B2c-report.md` (sections F1, F2), `docs/ble-provisioning.md`, `docs/reports/B1-report.md`. Work on Hardware main (`~/Desktop/AFK/Hardware/t-circle-s3`, IDF 6.0.2).

## Fix

- **F1: holding BOOT keeps factory-resetting.** After a 10 s hold wipes NVS and reboots, the key is still down, so the new boot starts counting from 0 and wipes again (3 wipes in the test). Fix: if BOOT is already pressed at boot, ignore it until it is released; only a press that starts after boot counts. Keep the 3–10 s pairing-mode and ≥10 s reset semantics.
- **F2: after a failed Wi-Fi try in pairing mode, the board stays off its network.** A provisioned board in pairing mode got a wrong password from the app (`wifi_failed`); the app left; the board stayed without Wi-Fi until the 5-minute pairing timeout (offline 5.5 min, Connector down, calls impossible). Fix: on `wifi_failed`, or when the BLE client disconnects without saving new credentials, reconnect to the **stored** network right away. Pairing mode may stay open (advertising) until its timeout. Never overwrite the stored credentials with ones that failed.

## Verify on the board

The serial port is now `/dev/cu.usbmodem101`. Ask the user before flashing; the board is shared.

- F1: hold BOOT 15 s → exactly one wipe.
- F2: provisioned board → pairing mode → write a wrong password with `t-circle-s3/tools/ble_provision.py` (or ask the user to do it from the app) → disconnect → board is back on its old Wi-Fi and Connector within ~15 s; then a tap-to-call works.
- Regression: factory provisioning from the app still works (ask the user to run Add device once).

The board is currently provisioned as `dev_yAA_sfKRFE_vb1d_f_70WA` (read-only). Don't factory-reset it unless the test needs it; if you do, the user re-adds it from the phone.

## Report

Append a "B1c" section to `docs/reports/B1-report.md` and commit (don't push).

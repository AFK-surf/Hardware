# F1 · Firmware: connect with the device's own name, not "T-Circle"

Read first: `t-circle-s3/main/connector.c`, `provisioning.c`, `device_config.c/.h`, and `docs/ble-provisioning.md`. Work on Hardware main, IDF 6.0.2 (`. ~/.espressif/tools/activate_idf_v6.0.2.sh`).

## Problem

During setup, the Android app sends `device_name` in the BLE credentials JSON (e.g. `"Comma-5D28"`, the board's BLE name). The app also mints the connector token with the same name.

The firmware stores `device_name` (`provisioning.c:222` → `device_config.c:48`) but never uses it:
- the Connector dials `"%s?name=T-Circle&alias=tcircle&..."` (`connector.c:638`);
- `metadata` hard-codes `"hostname":"T-Circle"` (`connector.c:365`).

The backend takes the dial's `?name=` over the token's name. So in Comma's Settings › Devices the board shows as "T-Circle" instead of "Comma-5D28". The app's later rename only sometimes runs.

The backend matches a nearby-device setup offer to the device that came online by a name ending in `-<short id>` (or by device_id). With "T-Circle", the name match never works, so the offer card can stay "Setting up…" and the "already connected" check can't see the device.

## Do

1. Build the dial URL with the saved name:
   - `name=<URL-encoded device_name>`;
   - if no name is saved (old provisioning), fall back to the BLE name `Comma-XXXX` (`s_name` logic in `provisioning.c`), never "T-Circle".
2. Keep `alias` stable and lower-case: derive it from the same name, e.g. `comma-5d28`. Check that the protocol doc and backend accept that alias format.
3. Use the same name for `hostname` in `metadata`.
4. URL-encode properly: a user may rename the device to something with spaces or non-ASCII.
5. Update `docs/connector-protocol.md` / `docs/ble-provisioning.md` where they mention "T-Circle" as the dial name.
6. Build. Ask the user before flashing (serial `/dev/cu.usbmodem1101`; check with `lsof` that nothing holds it).
   - Don't factory-reset; the board keeps its current setup.
   - After flashing, confirm in the serial log that the dial uses the saved name.
   - Have the user check that Settings › Devices shows that name, not "T-Circle".

## Don't

- Don't change the BLE advertising, pairing or credential format.
- Don't print tokens or keys in logs.

## Report

Write `docs/reports/F1-report.md`. Commit only your own files with explicit `git add`. Other agents use this checkout. Don't push; the repo has no remote.

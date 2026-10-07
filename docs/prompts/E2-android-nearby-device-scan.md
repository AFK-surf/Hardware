# E2 · Android: find a new Comma device when the app opens

Work in the Cue repo, `clients/apps/android`. Start from the Add device code (#2699): `settings/devices/` (`BleLink.kt`, `AddDeviceFlow.kt`, `AddDeviceSession.kt`, `TCircleProtocol.kt`, `PairedDeviceStore.kt`). Also read `~/Desktop/AFK/Hardware/docs/ble-provisioning.md`, the BLE contract with the device.

## Why

New feature, three parts:
1. The user plugs in a new T-Circle (Comma's ESP32 device) and opens the Comma app.
2. The app scans briefly over BLE.
3. If an unprovisioned Comma device is nearby, the **agent** posts a message in the chat: "I found a Comma device nearby (Comma-5D28). Want to set it up?", with a **Connect** button. Tapping it opens Add device with that device already chosen.

Parts:
- **E1 (firmware)** adds model, state and short id to the advertising.
- **E3 (backend)** takes the phone's report and posts the message with a structured card.
- **This part:** scan, report, render the card, open the flow.

The product ships overseas (Pixel, Samsung). Scan **only while the app is in the foreground**; no background scanning. Visible text is English.

## Advertising contract (from E1)

- Advertising data: the 128-bit service UUID `f2d20001-eb8e-4a64-9ec7-ab0cbb8107c8`; filter on it.
- Scan response: name `Comma-XXXX`, plus manufacturer data:

| Byte | Field | Value |
|---|---|---|
| 0–1 | company id | `0xFFFF` (test id for now) |
| 2 | format version | `0x01`; ignore other versions |
| 3 | model | `0x01` = T-Circle-S3 |
| 4 | state | `0x00` unprovisioned, `0x01` pairing mode |
| 5–6 | short id | e.g. `0x5D 0x28` → `"5D28"` |

Only **state `0x00`** triggers an offer. Pairing mode means the user is already handling their own device. Devices with no manufacturer data (old firmware) are ignored.

## Do

1. **Scan on open.**
   - When the app comes to the foreground, the user is signed in, Bluetooth is on and `BLUETOOTH_SCAN` is granted: scan up to 8 s with the service-UUID filter, using the scanning code that Add device already has (`AndroidDeviceScanner`). Stop early once a match is found.
   - At most once per foreground, and not while Add device is open.
2. **Permission.** Today `BLUETOOTH_SCAN` ("Nearby devices") is requested only inside Add device, so a brand-new user won't have it. Ask once, at a sensible moment after sign-in, with a short reason ("Find Comma devices near you"). If it's denied, don't ask again; Add device still asks as it does today. Describe the moment you chose in the report; the user will judge it on the phone.
3. **Report.**
   - `POST /v1/comma/workspaces/:workspace_id/nearby-devices` with `{request_id, reporter_device_id, devices:[{name, model, state, short_id, rssi}]}`. Take the final shape from E3's PR; it may change.
   - Keep a local list of reported short ids so the same device isn't reported again within 30 minutes. The server dedupes too.
4. **Card.**
   - Render E3's `device_setup_offer` card under the agent message: **Connect** and **Not now**.
   - On the phone whose device id is `reporter_device_id`:
     - **Connect** posts `started` and opens Add device;
     - Add device scans and **auto-selects** the device named `Comma-<short_id>` (no list step). If it isn't found within the usual timeout, fall back to the normal list.
   - On other phones: the card shows "Set up from the phone near it" and no Connect.
   - **Not now** posts `dismiss`.
   - `done` and `expired` show a quiet resolved state ("Set up" / "Expired") with no buttons.
5. **Tests:** parse the manufacturer data (good, wrong version, too short, wrong company id); the scan → report decision (state, dedupe, not during Add device); the card's states; auto-select by name.
6. **On the phone.** The test phone `23112153D000003` is shared. Ask the user before installing, and check what's in the foreground first. The device must be unprovisioned (factory reset: hold BOOT 10 s). Ask the user before doing that, since it removes the board's current setup.
   - Then:
     1. open the app → the message appears within a few seconds;
     2. Connect → setup completes without picking from a list;
     3. reopen the app → no second message.

## Don't

- No background or periodic scanning, no foreground service.
- Don't change the pairing, Wi-Fi or credential steps of Add device.

## Report

Commit on a branch, open a draft PR as usual, and summarise in the PR description: the permission moment, timings (open → message), and what you verified on the phone.

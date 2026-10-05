# B1 · Firmware: BLE provisioning (pairs with B2 Android)

Read first: `~/Desktop/AFK/Hardware/docs/connector-plan.md` (in Chinese).

## Goal

A freshly flashed T-Circle can be set up from the Comma Android app with no compiled-in secrets. The app gives it Wi-Fi and its credentials over BLE; the device stores them and comes online.

## Design (change it only with a reason; write the final contract to `docs/ble-provisioning.md` for B2)

- **Advertise** only while unprovisioned, or in pairing mode: name `Comma-XXXX` (last 2 MAC bytes) plus one 128-bit service UUID.
- **Security:** LE Secure Connections with a passkey shown **on the round screen** (6 digits, large). The app user types it. Encrypted, authenticated characteristics only.
- **GATT characteristics:**
  - `wifi_scan`: write to start a scan; notify a JSON list `[{ssid, rssi, secure}]`. 2.4 GHz only; say so in the UI text.
  - `wifi_credentials`: write `{ssid, password}`.
  - `credentials`: write JSON:
    - `server`: for example `https://salix-staging.cue.surf`
    - `workspace_id`, `group_id`
    - `connector_token`
    - `voice_key` (`salix_vk_…`)
    - `device_name`
  - `status`: notify `{state: idle|wifi_connecting|wifi_failed(reason)|online|connector_up, ip?, device_id?}`.
  - `info`: read `{fw_version, mac, model: "t-circle-s3"}`.
- **Storage:** NVS (use NVS encryption if it's practical on this board; otherwise explain the risk). The voice code and the A2 Connector read credentials through one accessor that replaces `secrets.h`. Keep `secrets.h` only as a dev override flag.
- **Factory reset:** hold BOOT for 10 s (show a countdown on screen). It wipes NVS credentials and Wi-Fi, then re-enters pairing.
- **Screen states:** pairing code, connecting, done, and the error reason.
- **Stack:** NimBLE. BLE must not run during calls (coexistence hurts Wi-Fi throughput, and calls are fragile on lossy Wi-Fi, see `git log -2`). Stop advertising once provisioned.

## Do

1. Implement, build, then ask the user before flashing (one board, shared).
2. Test with nRF Connect or a small Python `bleak` script before B2 is ready. Commit the script under `tools/`.
3. Verify end to end:
   - from factory state, provision → online → place a call;
   - reboot keeps the credentials;
   - factory reset works.

## Report

Write `~/Desktop/AFK/Hardware/docs/reports/B1-report.md`. Commit (don't push).

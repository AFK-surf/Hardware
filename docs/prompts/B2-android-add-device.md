# B2 · Android: "Add device" flow for the T-Circle

Repo: `~/Desktop/AFK/comma-android` (Kotlin). Read first:
- `~/Desktop/AFK/Hardware/docs/connector-plan.md` (in Chinese)
- The GATT contract from B1: `docs/ble-provisioning.md`. Until B1 merges it lives only on branch `codex/ble-provisioning`, so read it in the worktree `~/Desktop/AFK/Hardware-b1/docs/ble-provisioning.md`, together with `docs/reports/B1-report.md` there. It supersedes the design in the B1 prompt: paginated Wi-Fi scan, `status` stops at `saved` (check "online" in the backend device list), optional `connector_server`, and pairing mode entered by holding BOOT 3–10 s.

## Goal

Signed-in user → Settings › Devices › Add device. The app finds the T-Circle over BLE, takes the passkey shown on its screen, lets the user pick Wi-Fi and enter the password, mints the device's credentials from the backend, writes them, and waits until the device is online.

## Context

- Voice credentials today: `app/src/main/java/surf/comma/android/voice/VoiceCredentials.kt`, which calls `POST /v1/comma/workspaces/:ws/voice-api-keys` with a name and `expires_at`. Mint one key per device, named after the device. Ask the user about expiry; the plan defaults to long-lived and revoked on unpair.
- Connector token: `POST /v1/comma/workspaces/:ws/connector-token`. Backend: `~/Desktop/AFK/Cue/systems/apps/comma_web/lib/comma_web/router.ex:2046, 4689-4712`. Pass a device name. Ask whether to set a first-registration window; a token with no expiry does not expire. The response includes `device_id` and `connect_url`. Pass the connector server to the device exactly as `docs/ble-provisioning.md` specifies (`connector_server`). It's the Salix address, not the API base URL (A1 report, item 5).
- Device list and status: `GET /v1/comma/workspaces/:ws/devices[/:id]`.
- Follow the repo's AGENTS.md/CLAUDE.md, Comma design tokens and the existing settings-page patterns. Visible text is English.

## Do

1. Add BLE permissions and a runtime permission flow (Android 12+ `BLUETOOTH_SCAN`/`CONNECT`).
2. Build the flow in 5 screens or fewer:
   - Scan: list devices matching the service UUID.
   - Pair: system passkey entry, telling the user the code is on the device screen.
   - Wi-Fi: the device's scanned list plus a password field; note "2.4 GHz only".
   - Setup: mint both credentials, write them, then show `status` notifications live.
   - Done: device name and online state, with a "Test call" hint.
3. Handle errors: wrong Wi-Fi password, mint failure (don't leave orphan tokens; revoke on abort), BLE disconnect mid-way, timeout.
4. Unpair from the device detail screen: revoke the connector token and voice key, remove the device, and tell the user to factory-reset it (hold BOOT 10 s).
5. Write unit tests for the state machine and the JSON codec (JVM tests; androidTest doesn't run in CI).

## Don't

- Don't install on the user's phone (`23112153D000003`) without asking. The shared emulator `emulator-5554` may be used by other agents, so check the foreground app first. BLE needs real hardware: ask the user when you need the phone and the T-Circle.
- Don't log tokens or keys.

## Report

Write `~/Desktop/AFK/Hardware/docs/reports/B2-report.md`. Work on a branch (`codex/add-device-ble`); don't push or open a PR unless asked.

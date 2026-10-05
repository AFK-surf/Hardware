# B2b · Android: "Allow operations" toggle on the device detail screen

Repo: `~/Desktop/AFK/comma-android`. Continue on the B2 branch `codex/add-device-ble` in worktree `~/Desktop/AFK/comma-android-add-device` (head `96a5a6e`). Read `~/Desktop/AFK/Hardware/docs/reports/B2-report.md` and `~/Desktop/AFK/Hardware/docs/connector-plan.md` (in Chinese) first.

## Why

A T-Circle (or any Connector) starts read-only. LAN requests only go through after the user allows operations. A1 found that the server does not check access for device-placed requests, so the device-side gate is the **only** boundary and the user's switch for it matters (`docs/reports/A1-report.md`). The Android device detail screen (Settings › Devices › a device) shows Status, Type and Remove, but has no access switch. The Mac client doesn't show the T-Circle at all. So right now there is no UI anywhere to turn T-Circle operations on.

## Do

1. On the device detail screen, add a switch "Allow operations", with the subtitle "Lets Comma act through this device. Off keeps it read-only."
   - State comes from the device's `allows_operations` in `GET /v1/comma/workspaces/:ws/devices/:id` (see `~/Desktop/AFK/Cue/systems/apps/comma_core/lib/comma/devices.ex`, around :60-100).
   - Toggling calls `PUT /v1/comma/workspaces/:ws/devices/:device_id/access` with `{"allow_operations": true|false}` (router `comma_web/router.ex:2031`; the web client does the same in `clients/packages/app/src/api/client.ts:1417`). The server forwards it to the device as `device_access` and returns the device's answer.
   - Show it for all Connector devices (computers too) when the device is online. When offline, disable it and say "Device is offline."
   - On error, revert the switch and show the reason. `device_access_unavailable` means the device didn't confirm.
   - Turning it **on** asks for confirmation once: "Comma will be able to send requests on your local network through this device."
2. Follow the existing settings patterns and design tokens; visible text is English.
3. Unit tests for the request and the state handling (JVM).
4. Build the staging debug APK. **Ask the user before installing on the phone** `23112153D000003`. The phone already runs this branch's debug build, so `adb -s 23112153D000003 install -r` works without uninstalling.
5. Verify on the phone with the T-Circle online: off → on → off, each confirmed in the device detail after a refresh.

## Don't

- Don't touch the T-Circle board or its provisioning. A3 may be testing on it.
- Don't log tokens.

## Report

Append a "B2b" section to `~/Desktop/AFK/Hardware/docs/reports/B2-report.md`. Commit on the branch (don't push).

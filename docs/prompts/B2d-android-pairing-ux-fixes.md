# B2d · Android: pairing UX fixes from the B2c test

Repo `~/Desktop/AFK/comma-android`, branch `codex/add-device-ble` in worktree `~/Desktop/AFK/comma-android-add-device` (head `8bdd09b` or later). Read first: `~/Desktop/AFK/Hardware/docs/reports/B2c-report.md` (section B2) and the screenshots in `~/Desktop/AFK/Hardware/docs/reports/B2c-screens/`.

## Fix

- **A1: the pairing request is a notification, not a dialog.** On this Nothing phone the system shows "Pairing request · Tap to pair with Comma-XXXX" as a heads-up notification. The app only says "Type it into the pairing request on your phone". Change the copy so the user knows to tap the notification ("Pair & connect") and then enter the code from the device screen. Also try calling `createBond()` from the app before the first encrypted access, if that makes Android more likely to show a dialog; keep whichever works on this phone.
- **A2:** the "Working…" icon on the Setting up screen doesn't spin. Animate it (follow the repo's motion rules).
- **A3:** the Wi-Fi password is shown in plain text. Mask it and add a show/hide toggle.
- **A4:** the password field doesn't focus or open the keyboard when the dialog opens. Fix that.
- **A5:** the device list still showed the T-Circle as Online about 5 minutes after its Connector dropped. Find out whether the app shows a stale value or the backend reports it that way: compare the app with `GET /v1/comma/workspaces/:ws/devices/:id` at the same moment. Fix the app if it's stale. If it's the backend, write down the exact field values and timing for the backend team; don't work around it.
- Minor: Comma devices (T-Circle) and the "Signed in" phone/computer rows use the same icon. Give Comma hardware its own icon if the icon set has a fitting one; if it doesn't, leave it.

## Verify

JVM tests for any logic you change. Build the staging debug APK. **Ask the user before installing** on phone `23112153D000003` (it already runs this branch, so `install -r` works). Check A1–A4 on the phone; pairing needs the T-Circle in pairing mode (the user holds BOOT 3–10 s). Coordinate with the user: B1c may be flashing the board.

## Report

Append a "B2d" section to `~/Desktop/AFK/Hardware/docs/reports/B2-report.md`. Commit on the branch (don't push).

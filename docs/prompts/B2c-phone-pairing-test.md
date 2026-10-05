# B2c · Real-device test: add the T-Circle from the Android app

Read first:
- `~/Desktop/AFK/Hardware/docs/connector-plan.md` (in Chinese)
- `docs/ble-provisioning.md`
- `docs/reports/B1-report.md` (with B1b), `B2-report.md` (with B2b) and `A3-report.md`

## Goal

Prove the out-of-box flow on real hardware. Start from a factory-reset T-Circle. The user adds it from the Comma Android app (Settings › Devices › Add device), it comes online, calls work, and LAN requests work once the user allows operations. Until now this flow only had unit tests and a Mac BLE script.

## Setup (only one USB cable exists)

- **The cable goes to the T-Circle** (`/dev/cu.usbmodem1101`), so you can read serial logs. The phone is not on USB; the user operates it by hand and sends you screenshots when needed. Don't flash unless something must be fixed, and ask first.
- Phone `23112153D000003` already runs the B2/B2b debug build (`codex/add-device-ble`, `8bdd09b`) and is signed in to staging.
- The board runs Hardware main with A3 on top. Check `git log` and confirm A3 is committed before you start; if it isn't, stop and tell the user.
- Office Wi-Fi for the board: `ToEverything-IOT` (2.4 GHz). The user types the password in the app.

## Do

1. **Factory reset the board.** Ask the user to hold BOOT for 10 s (not 3–10 s: that's pairing mode). Confirm in serial that NVS is wiped and it advertises `Comma-XXXX`.
2. **Add device on the phone** (user does the taps):
   - Scan → pick `Comma-XXXX`.
   - Enter the 6-digit code from the round screen.
   - Pick `ToEverything-IOT` and enter the password.
   - Setup → Done.
   
   Record the time of each step, and compare the BLE `status` sequence in serial with what the app shows.
3. Check:
   - a **new** device appears online in Settings › Devices (new `device_id`, new token);
   - the old `dev_M3Z6mxvKhQSYEwyYTber-g` stays and shows offline. Have the user unpair it from the phone and confirm the app revokes its token and voice key (B2 report);
   - T-Circle starts read-only. The user turns on Allow operations, then asks Comma in chat: "call lan-test's lan_status on my new T-Circle". The `lan-test` MCP is bound to the old device; rebinding it to the new one is part of the test. Run `tools/lan_mcp_server.py` on the Mac if it isn't up (see the A3 report).
   - one tap-to-call of ~1 minute works with the new voice key.
4. **Failure paths** (one each):
   - wrong Wi-Fi password: the app shows it, no credentials are minted (check the device list);
   - backing out mid-setup: no orphan device or token is left.
5. Note any UX problems (copy, waits, confusing screens) with the user's screenshots.

## Don't

- Don't install anything on the phone or run adb against it without asking. The user is driving it.
- Don't print or commit secrets.

## Report

Write `~/Desktop/AFK/Hardware/docs/reports/B2c-report.md`: step timings, pass/fail per check, the new `device_id`, and any bugs for B1 (firmware) or B2 (Android) with the exact symptom. Commit the report (don't push).

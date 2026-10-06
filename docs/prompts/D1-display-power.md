# D1 · Firmware: cut screen power (drop the logo animation, sleep the screen)

Read first: `t-circle-s3/main/display.c` and `main.c`, `docs/reports/A3-report.md` (A3 moved the framebuffers to PSRAM), and the V1 report if it exists. Work on Hardware main. Start after V1 has committed, so you don't collide on the board.

## Why

The T-Circle is meant to sit plugged in 24/7 as a home gateway. The user says the screen runs too hot and power-hungry ("太烧了"). Today:
- the Cue logo animation (101 frames at 50 fps, full-screen SPI redraws) plays during every call;
- while idle, the finished logo stays on screen with the backlight on all the time.

## Do

1. **Remove the logo animation.** During a call, show a simple, mostly static call screen instead. Redraw only on state changes: dialing, connected, agent speaking, ended. A small indicator that changes a few times per second at most is fine. Remove the embedded animation frames (`cue_logo_frames.bin`) if nothing else uses them; that also frees flash.
2. **Sleep the screen when idle.** Turn the backlight off (and put the GC9D01 in sleep if the driver supports it) after ~15 s idle. Tap or BOOT wakes it. These still wake it: an incoming state that needs the user (pairing mode with the passkey on screen, "Pairing timed out", a provisioning error), and a call starting. Keep the passkey on screen for the whole pairing window (B1e), and don't let sleep hide it.
3. Make sure a tap on a sleeping screen still starts a call: one tap wakes and dials, not two. Pick the behaviour that matches the "tap to call" UX and explain it in the report.
4. **Measure.** Report the board's current at 5 V, idle and in a call, before and after. Use a USB power meter if the user has one, or estimate with the ESP-IDF power APIs. Also report the chip temperature (`temperature_sensor`) idle and after a 2-minute call.

## Don't

- Don't touch Wi-Fi, Connector or voice code beyond what the display needs.
- Ask the user before flashing; the board is shared.

## Report

Write `docs/reports/D1-report.md`. Commit (don't push).

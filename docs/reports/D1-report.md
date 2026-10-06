# D1 report: screen power and hang-up tones

2026-10-06 · `t-circle-s3/main/display.c/.h`, `main.c`, `cue_voice.c/.h`, `playback.c/.h`, `ui_font.c`, `ui_card.h`, `cue_logo.bin/.h` (new), `tools/gen_cue_logo.py` (was `gen_cue_logo_frames.py`), `tools/gen_ui_fonts.py`, `tools/card_preview.c` · board `Comma-5D28` on `ToEverything-IOT` · staging

## Result

| Check | Result |
|---|---|
| Logo animation removed | **Done.** The 101-frame animation is gone; the idle screen is the logo at rest (its last frame), drawn once. `cue_logo_frames.bin` (1.29 MB) is replaced by `cue_logo.bin` (12.8 KB). App image **2.66 MB → 1.38 MB**. |
| Call screen | **Done.** Text cards, redrawn only when the text changes: `Calling / Comma` → `Comma / 0:42 / Listening` or `Speaking` (time ticks once a second) → `Call ended 2:03` or `Call dropped 0:47 / Tap to call again` for 3 s → logo. |
| Hang-up tones | **Done, heard on the board.** Normal end: one soft 620 Hz tone, 200 ms. Dropped: three 950 Hz beeps, 120 ms on / 100 ms off. Connected (see below): 660 Hz 80 ms, 40 ms gap, 990 Hz 100 ms. All synthesized in `playback.c` from one table, quieter than the ringback. |
| Screen sleep | **Done, seen on the board.** After 15 s without activity: backlight off, `DISPOFF` + `SLPIN` to the GC9D01. A tap, BOOT, any card (pairing, errors, notices) or a call wakes it. It never sleeps during a call, while a call is closing, during pairing mode, while BOOT is held, or while a notice or end card is up. |
| One tap on a dark screen | **Wakes and dials** (verified). See *Tap on a sleeping screen*. |
| Tap during the tone starts a new call | **Done, verified, with a 500 ms guard.** A tap while the last call is still closing is queued and dials as soon as the call has closed. But taps within 500 ms of a hang-up tap are ignored, so a double tap to hang up doesn't redial (the 200 ms tone falls inside that guard; see *Tap on a sleeping screen*). |
| Current at 5 V | **Estimated only, no USB meter.** See *Power*. |
| Chip temperature | Measured: see *Temperature*. |
| Agent doesn't greet | **Fixed, verified.** If the agent hasn't spoken 8 s after `session.started`, the ringback stops, a short "connected" tone plays (two rising notes) and the mic opens. See *Notes*. |
| Pairing passkey stays on | **Code-checked, not run on the board.** The screen is held awake for as long as `provisioning_active()`, which covers the whole BLE window including the passkey. |

## What changed

### Display (`display.c`)

- One 50 KB PSRAM framebuffer instead of two; with redraws this rare, a draw just waits for the previous transfer.
- `display_show_card()` / `display_show_logo()` skip the redraw when the content is unchanged, so the call screen can be refreshed from the 10 ms UI loop without costing SPI traffic.
- **Sleep:** `BL_OFF`, wait for any transfer in flight, `DISPOFF`, `SLPIN` (0x10). The driver has no sleep op, so the commands go through the panel IO directly.
- **Wake:** `SLPOUT` (0x11), 120 ms, redraw, `DISPON`, backlight on. The redraw isn't strictly needed (the panel RAM survives `SLPIN`), but it costs ~10 ms and can't show a stale frame. A change requested while asleep waits for the wake.
- Backlight polarity confirmed on the board: GPIO18 low = on (named `BL_ON` / `BL_OFF` now).
- New API: `display_wake()` (tap/BOOT), `display_keep_awake(bool)` (set every UI loop by `main.c`), `display_asleep()`. `display_set_animating()` and `display_run_logo()` are gone; the task is `display_run()`.
- The large font gained `:` for the call time (`gen_ui_fonts.py` reproduces the old `ui_font.c` byte for byte before the change).

### Call screen (`main.c`)

- `Calling` stays up while dialing **and** while the ringback plays: until the agent first speaks, nothing has really happened yet. The time starts when the ringback stops.
- `Speaking` shows as soon as agent audio is playing; `Listening` comes back only after 600 ms of quiet, so the text doesn't flicker between sentences. In the calls today that was a change every few seconds at most.
- The end card waits until `cue_voice` has decided how the call ended (set just as the tone starts), so it never says "ended" for a drop.
- A failed `cue_voice_start()` plays the dropped tone and shows `Call failed / Tap to try again`.

### Hang-up tones (`playback.c`, `cue_voice.c`)

- `playback_tone(NORMAL | DROPPED)` drops queued agent audio and plays the tone once. `playback_ringback(true)` cancels a tone, so a redial cuts it short.
- `cue_voice` picks the tone the moment the call loop exits, **after `board_audio_set_recording(false)`**, so the tone never reaches the uplink. (The mic is also gated while the speaker plays, but it's closed first anyway.)
- **Normal** when any of: the caller tapped, the device's own 60 s idle hangup, or the server ended it cleanly (`session.ended` and/or close 1000, with no other close code). **Dropped** for everything else: an error close (44xx), `uplink dead` (10 s stall), a lost connection, a dial that never connected.
- Agent audio that still arrives between hangup and the server's `session.ended` is now dropped. Before, it played after the hang-up; on the first D1 build it played right after the tone (seen at 17:12:06).
- The amplifier now also switches off 150 ms after the ringback or a tone, as it already did after agent audio.

### Closing state and the queued tap (`cue_voice.c`, `main.c`)

`cue_voice_ending()` is true from the moment a call is over (hung up, closed, lost) until its socket is torn down and a new call can start. Meanwhile `cue_voice_state()` may still read `UP`, so before this a second tap was taken as another hang-up. Now:
- a tap while ending is logged (`tap while the last call is closing: dialing once it has closed`) and dials as soon as `cue_voice_can_start()`, within 6 s;
- the tone is 200 ms but closing takes ~2 s after a connected call, and **~5.5 s when hung up while still dialing**: `esp_websocket_client_stop()` waits for the TLS handshake in progress. The queued dial covers that.

## Tap on a sleeping screen

**One tap wakes the screen and dials.** The device does one thing. A tap means "call", and the ringback confirms it right away even before the screen is lit (wake takes ~130 ms). Making the first tap only wake the screen would turn every call into a two-tap ritual and break "tap to call". An accidental tap costs one more tap to hang up.

Exceptions, as before: in pairing mode a tap leaves pairing (on a provisioned device), and while BOOT is being held a tap does nothing. Both only happen with the screen on, because pairing and BOOT keep it awake.

**Double tap to hang up.** With only the queued tap, a quick double tap to hang up became hang up + redial: the second tap landed while the call was closing. That happened by accident in testing (17:25:43). At the user's request, taps within **500 ms** of a hang-up tap are now ignored (`HANGUP_GUARD_US`, logged as `tap N ms after hanging up, ignored`). Verified at 17:50:18 and 17:50:34: second taps 160 ms and 148 ms after the hang-up were ignored, and nothing redialed. A tap after 500 ms, still during closing, is queued as before. This guard only follows a hang-up tap: after a drop or a server end, any tap counts.

## Power

**No USB power meter, so these are estimates, not measurements.** ESP-IDF has no current-measurement API on the S3.

| | Before (V1) | After (D1) | Basis |
|---|---|---|---|
| Idle, screen | backlight on 24/7 + panel active | backlight off, panel in `SLPIN` after 15 s | Backlight of a sub-1" IPS panel: typically 10–20 mA. GC9D01 active ~2–4 mA, sleep < 0.1 mA. |
| Idle, total at 5 V | ~110–140 mA | ~95–120 mA | Saving **~12–24 mA** (screen only). ESP32-S3 at 240 MHz, Wi-Fi associated with power save off (`ps=0`), no light sleep: ~90–110 mA by itself, and D1 doesn't change that. |
| In call, screen | backlight on + 50 fps full-screen SPI (2.56 MB/s from PSRAM, CPU expanding every frame) | backlight on + ~1 redraw/s | Removing the animation saves an estimated **3–8 mA** (SPI/DMA, PSRAM reads, CPU work). |
| In call, total at 5 V | ~140–190 mA | ~135–180 mA | Wi-Fi TX/RX at 48 KB/s each way and the speaker amp dominate. |

So the screen was a real but minor share of the board's power. The chip itself (CPU at 240 MHz, Wi-Fi radio always listening) is most of it, and also most of the heat (below). Changing that (Wi-Fi power save, CPU frequency scaling or light sleep) is outside D1, since it touches Wi-Fi and voice latency. **Recommended next**, measured with a real USB meter.

## Temperature

Internal sensor (`temperature_sensor`, ±2 °C, die temperature, not the glass). Logged by the firmware now: `main: temp chip=… idle|call`, every 60 s idle and every 10 s in a call.

| | Before (V1 + logging) | After (D1) |
|---|---|---|
| Idle | 66–67 °C (minutes 0–4 after boot, screen on) | 62 → **58 °C** and still falling (minutes 0–6 after boot, screen off from 15 s) |
| Call | 67 → **70 °C** after a 138 s call (17:08–17:10) | 65 → **67–68 °C** after a 108 s call (17:13–17:15) |

Same room, same Wi-Fi, runs 20 minutes apart (17:03–17:10 before, 17:11–17:33 after).

- **Idle: about 8 °C cooler with the screen asleep**, and still falling when logging stopped. That's more than the 12–24 mA estimate would suggest on its own. On a board this small the backlight's heat reaches the die, and/or the backlight draws more than a typical small panel. Only a meter can say which. Either way, the screen was a bigger part of the idle heat than I expected.
- **Call:** the D1 call started 2 °C cooler and ended 2–3 °C cooler. That's within the sensor's ±2 °C, and the backlight stays on in both, so most of it is the starting point. The animation itself made no clear difference to the die temperature.
- What's left (58 °C idle) is the chip: CPU at 240 MHz with Wi-Fi power save off. That's the follow-up in *Power*.

## Verification on the board

| Test | Log |
|---|---|
| Sleep after 15 s | `display: screen off` 15 s after boot and 15–18 s after each end card (17:11:22, 17:16:04, 17:17:31, 17:25:24, 17:27:23). User confirmed the screen goes dark. |
| One tap wakes and dials | 17:11:54 `cue: dialing` and `display: screen on` in the same 120 ms. User confirmed. |
| Normal end tone | 17:12:04, 17:15:46, 17:17:13: `call over: ended (tap=1 …)`. User heard the single tone. |
| Dropped tone | Couldn't switch the Wi-Fi off, so a bench hook (`CUE_VOICE_SIMULATE_DROP 1` in `cue_voice.c`, off in the committed code) makes the voice socket look full from 20 s into the call. The call then ends through the real 10 s stall path: 17:23:54 and 17:25:06 and 17:26:30, `uplink dead, hanging up waited_ms=10000` → `call over: dropped (tap=0 idle=0 session_ended=0 close=0)`. User heard the three beeps. A real Wi-Fi loss ends through the same path (or a websocket error, also classified as dropped), but that wasn't produced on the board. |
| Tap during the tone redials | 17:25:36 hang-up + second tap 170 ms later → queued → 17:25:42 `dialing the tap made while the last call was closing` (closing took 5.5 s: hung up while still dialing). |
| RAM | Lowest internal free across today's D1 runs 113.4 KB vs 112.7 KB before: unchanged. The display task stack stays in PSRAM. |

The board now runs the committed build (hook off), flashed 17:27.

## Notes

- The serial logger the V1 session left running (PID 20786, `v1.log`) was stopped with the user's OK to free the port.
- `idf.py` still names the image `cue_logo.bin` (the project name); that's unrelated to the deleted frames file.
- **The agent didn't greet once (17:48, `vc_01M489T0NZJ7PX6VP4G921BFMN`)**: the session started and the downlink carried only silence (`rx_peak=0`, no transcript) for 2+ minutes. The ringback plays until the agent first speaks, and the mic stays muted while it rings, so the call was stuck ringing: the caller couldn't speak and the 60 s idle hangup never fired (silence frames count as activity). That's a backend hiccup (the next call greeted at once), but the firmware waited forever on it; this predates D1.
  **Fixed at the user's request** (`GREETING_WAIT_US` in `cue_voice.c`): 8 s after `session.started` with no agent speech, `playback_tone(PLAYBACK_TONE_CONNECTED)` stops the ringback and plays two rising notes. The mic stays gated while any tone plays (`playback_speaking()` now counts tones), then opens after the usual 300 ms echo tail. The queue it clears holds only the server's paced silence, because the ringback stops on the first speech. A greeting that arrives later plays normally. Logged as `agent silent N ms after session.started: ringback off, mic open`.
  Verified with a test build at 1 s instead of 8 (18:01, `vc_01M48AJK3XTRXYZG8KM4DH6J3B`): ringback off at 1014 ms, mic open 540 ms later, greeting 4 s after that, caller speech transcribed. The user heard the tone clearly and could tell it from the end tones. The board runs the 8 s build. One call today took 10 s to greet (17:23:24 → 17:23:34); on calls like that the tone now plays 2 s before the greeting.
- Seen twice, not caused by D1: after `session.end` the server's `session.ended` sometimes doesn't arrive within the 1 s wait, and the client logs `Client was not started` during teardown (17:12:06). The call still ends normally.

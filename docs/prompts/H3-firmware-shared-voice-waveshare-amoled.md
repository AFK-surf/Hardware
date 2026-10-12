# H3 · Firmware: shared voice and screen code, and the Waveshare ESP32-S3-Touch-AMOLED-1.32

Work on a new branch from local `main` (head `7fe8d34` or later), in a new worktree
`~/Desktop/AFK/Hardware-h3`, branch `codex/waveshare-amoled`. ESP-IDF 6.0.2
(`. ~/.espressif/tools/activate_idf_v6.0.2.sh`). Build with `tools/board.sh BOARD build`.

Read first:
- `README.md`, `devices/README.md`, `devices/AGENTS.md` (the checklist for adding a board; follow it);
- `docs/reports/H1-report.md` (multi-board layout, BOOT handling, pairing methods, DevKit),
  `docs/reports/H2-report.md` (the last hardware numbers), `docs/connector-protocol.md`,
  the BLE setup contract it links;
- the firmware: `firmware/main/` (`board.h`, `app_controls.h`, `main.c`, `provisioning.c`,
  `voice_state.h`, `memory_caps.h`, `Kconfig.projbuild`, `CMakeLists.txt`, `idf_component.yml`),
  `firmware/boards/t_circle_s3/*` and `firmware/boards/esp32_devkit/board.c`, `tools/board.sh`,
  `firmware/partitions.csv`, `devices/sdkconfig.*`;
- the reference driver for the new board, read-only:
  `~/Desktop/Projects/voice-box-agent/firmware/main/boards/waveshare_amoled_1_32/` (README,
  `board_audio_waveshare.c`, `board_ui_waveshare.c`, `button_waveshare.c`), the pin map in
  `~/Desktop/Projects/voice-box-agent/firmware/managed_components/waveshare__esp32_s3_touch_amoled_1_32/include/bsp/esp32_s3_touch_amoled_1_32.h`,
  and the "AMOLED 踩坑记录" section of `~/Desktop/Projects/voice-box-agent/firmware/README.md`.
  That project is ours; it uses IDF 5.4, the Waveshare BSP and LVGL. Take facts and working
  sequences from it, not its structure.

## Why

The project will be published as open firmware for four boards: T-Circle-S3, Waveshare AMOLED 1.32,
M5Stack StickS3 and ESP32 DevKit. Today the whole voice call (mic, AFE, Cue voice session, playback,
call UI) and the screen card UI live in `firmware/boards/t_circle_s3/`, mixed with the T-Circle's
pins and drivers. The Waveshare board (this step) and the StickS3 (H4) need the same call behaviour
with different audio, display and input hardware. Copying the T-Circle folder twice would leave three
diverging copies of a 1,265-line voice client.

So H3 has two parts, in this order: **(A) split** the T-Circle code into shared voice/UI code and a
small board layer, with no behaviour change on the T-Circle; **(B) add** the Waveshare board on that
layer.

## The new board (facts from the reference project and its BSP)

- ESP32-S3-PICO-1, 8 MB flash, 8 MB octal PSRAM. USB-Serial/JTAG console.
- Display: CO5300 AMOLED, 466×466, QSPI. CS 10, PCLK 11, DATA0–3 12–15, RST 8, no backlight pin
  (brightness by panel command). Component `espressif/esp_lcd_co5300`.
- Touch: CST820 on I2C (SDA 47, SCL 48), RST 7, INT 6.
- Audio: ES8311 codec, one I2S port, full duplex: MCLK 38, SCLK 39, WS 41, DOUT 42, DIN 40; codec
  control on the same I2C bus. Speaker power amplifier enable on GPIO46 (the reference drives it
  from the application, not from the codec driver, so recording can force it off).
  Component `espressif/esp_codec_dev`.
- BOOT key on GPIO0, active low.

Known pitfalls from the reference (read the section for details):
- A display buffer allocated in PSRAM made the QSPI transfer fail under memory pressure (Wi-Fi start),
  and a lost transfer froze the UI forever. Keep the DMA source buffer in internal DMA-capable RAM, sized
  to fit, and treat a failed `draw_bitmap` as an error that is logged and recovered, never waited on.
- No shadows/gradients/large animated areas: software rendering of a 466 px screen starves the CPU.
- CO5300 needs even-aligned window coordinates (check the driver; align partial updates).
- `esptool --after hard-reset` can leave this chip in download mode on macOS; a USB replug cures it.
- The reference disabled touch because its panel was damaged. Do not assume touch is unreliable on a
  healthy panel, but do not make touch the only way to start or end a call.

## A. Split shared code from the T-Circle

Target layout (names are suggestions; keep the existing style):

- `firmware/voice/` (or `firmware/main/voice/`): `cue_voice.c/.h`, `playback.c/.h`,
  `voice_clean.c/.h/.lf`, and the call controller that today sits in `boards/t_circle_s3/board.c`
  (tap to call / hang up, guards, "Call ended"/"Call dropped" cards, listening/speaking states,
  temperature log). Built when `CONFIG_COMMA_HAS_VOICE`.
- `firmware/ui/` (or similar): `ui_card.c/.h`, `ui_font.c/.h`, the logo asset, and the card layout,
  drawn into a board-provided framebuffer of any size (160×160 and 466×466). Built when
  `CONFIG_COMMA_HAS_DISPLAY`.
- `firmware/boards/<board>/`: only pins and drivers: audio I/O, panel, touch/keys, power.

The board layer exposes a narrow interface, for example:
- audio: `board_audio_init`, `set_speaker_enabled`, `set_recording`, `read_mono` at the mic rate,
  `play_pcm16_mono` at the playback rate, and the two rates (today `MIC_SAMPLE_RATE_HZ` and
  `AUDIO_SAMPLE_RATE_HZ` are T-Circle constants in `board_audio.h`);
- display: init, framebuffer size, flush a region, sleep/wake, brightness;
- input: events for "call key/tap" and "BOOT held" (the BOOT hold logic stays in shared
  `app_poll`).

Nothing in shared code may include a board's private header (today `cue_voice.c` includes
`board_touch.h`; remove that coupling).

**Acceptance for A (before any Waveshare code):**
- T-Circle and DevKit build. The T-Circle generated `sdkconfig` is identical except for new keys at
  their defaults; app size within ±2 % of `main`; memory placement of the voice path unchanged
  (`voice_clean.lf` still applies).
- Same log lines, same `cue:` stats fields, same cards and timings, same pairing passkey flow.
- All host suites pass (C under ASan/UBSan, Python in `tools/.venv`).
- Commit A on its own (commit 1), so it can be retested on the T-Circle before B is reviewed.

## B. Add the Waveshare board

- `COMMA_BOARD_WAVESHARE_AMOLED_1_32`, board ID `waveshare-amoled-1-32`, overlay
  `devices/sdkconfig.waveshare-amoled-1-32`, `tools/board.sh` target `esp32s3`, an 8 MB flash
  setting, octal PSRAM, and a partition table that fits 8 MB with the same app and `model`
  partitions as the T-Circle (state the offsets).
- Voice and display on; pairing with a passkey shown on the screen, as on the T-Circle.
- **Input.** Start/end a call with a short BOOT press (released before the pairing hold threshold)
  **and** with a screen tap. BOOT 3–10 s pairing, >10 s factory reset stay exactly as in `app_poll`.
  State in the report how a short press is told apart from the start of a hold.
- **Audio.** ES8311 is full duplex on one I2S port, so mic and speaker share one sample clock, while
  the shared pipeline uses 16 kHz mic (AFE) and 24 kHz playback. Pick one I2S rate and resample the
  other direction in the board layer, or propose a cleaner split; justify with CPU numbers from the
  `cue:` stats. Keep the echo gate behaviour (mic muted while agent speech plays). Set mic gain and
  speaker volume so `mic_rms` while talking and `echo_rms` are in the same range as the T-Circle
  (H1/H2 reports), and start conservative on volume.
- **PA.** GPIO46 low at boot and whenever not playing; pulled low before the codec is reset.
- **Display.** 466×466 cards and logo, same content as the T-Circle, scaled for the larger round
  screen (round mask: keep text inside the circle). Screen sleeps like the T-Circle's. Full framebuffer
  in PSRAM if needed, but the DMA source must follow the pitfall above.
- **Dependencies.** Add `esp_lcd_co5300`, `esp_codec_dev` and a CST820 driver (or a small driver of
  your own over `i2c_master`) to `idf_component.yml` with exact versions that support IDF 6.0, gated so
  the DevKit (`esp32`) build does not pull them, and commit the updated lock. Do not add the
  Waveshare BSP or LVGL. Do not run `update-dependencies` for unrelated packages.
- **License.** Code copied or adapted from the reference project is ours; code adapted from a
  component or BSP keeps its license header and is listed in the report.

## Rules

- Do not flash any board; Claude tests on hardware. Write the exact test steps and expected log lines.
- No change to the Connector protocol, BLE contract, app or backend. `metadata` may differ only in
  the board ID and the board's own limits.
- Do not change T-Circle or DevKit behaviour or limits.
- Do not commit Wi-Fi passwords, tokens, test certificates, generated sdkconfig or build outputs.
- Two commits on `codex/waveshare-amoled` (A, then B). Don't merge, don't push.

## Report

Write `docs/reports/H3-report.md`: the new layout and board interface, what moved where, proof that
the T-Circle is unchanged (config diff, size, log lines), the Waveshare pin map, partition table,
audio rate choice, input rules, display buffer placement, dependency versions and licenses, memory
(internal free/largest, PSRAM free, stack watermarks with `COMMA_HEAP_DIAGNOSTICS` in a test build),
host test counts, and the hardware test plan below. Update `README.md` and `devices/README.md`
(Waveshare row: "built, not yet tested on hardware"). Reply with both commit hashes and a short
summary.

## Hardware test plan (Claude runs it; write the expected lines)

1. T-Circle after commit A: boot, a call (tap to call, talk, tap to hang up), BOOT 3–10 s pairing,
   one discovery. Compare `cue:` stats with the H2 numbers.
2. Waveshare: full flash backup first (Claude), then: boot log, logo, BLE setup from the app with the
   passkey on screen, Connector online, a call started by BOOT short press and one by tap, both
   directions audible, `cue:` stats, screen sleep/wake, discovery with probes, `http_request`.
3. Waveshare: BOOT 3–10 s pairing mode and >10 s factory reset.

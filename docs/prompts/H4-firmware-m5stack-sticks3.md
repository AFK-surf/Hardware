# H4 · Firmware: M5Stack StickS3 (voice, screen, two keys)

Work in a new worktree `~/Desktop/AFK/Hardware-h4`, on a new branch `codex/m5-sticks3` from
`codex/waveshare-amoled` (head `ef89713`, H3: shared `firmware/voice/` and `firmware/ui/`). H3 is
not merged into `main` yet, and H4 builds on its board layer. ESP-IDF 6.0.2
(`. ~/.espressif/tools/activate_idf_v6.0.2.sh`). Build with `tools/board.sh BOARD build`.

Read first:
- `devices/AGENTS.md` (checklist for adding a board; follow it), `README.md`, `devices/README.md`;
- `docs/reports/H3-report.md`: the board interfaces (`voice/board_audio.h`, `voice/board_input.h`,
  `ui/board_display.h`), how the T-Circle and Waveshare implement them, and **the Waveshare
  hardware findings on the ES8311 mic** (the mic delivered ~1.9× real time);
- `docs/reports/H1-report.md` and `H2-report.md` for the T-Circle reference numbers (`cue:` stats,
  `mic_rms`, `echo_rms`), `docs/connector-protocol.md`;
- the firmware: `firmware/main/`, `firmware/voice/`, `firmware/ui/`, `firmware/boards/*`,
  `tools/board.sh`, `firmware/partitions*.csv`, `devices/sdkconfig.*`;
- the reference driver, read-only:
  `~/Desktop/AFK/muse-gadget-sdk/esp32/components/muse/boards/board_m5stack_sticks3.c`, its overlay
  `esp32/devices/sdkconfig.muse-m5stack-sticks3`, and `components/muse/muse_audio.c` (how it takes
  the mic from a stereo I2S frame). Apache-2.0, © Meta Platforms. Code adapted from it keeps that
  copyright and license header, and the report lists it. That project uses LVGL and
  `esp_codec_dev`; take facts and working sequences from it, not its structure.

## The board (from the reference; M5 K150 schematic v0.6)

- ESP32-S3-PICO-1-N8R8: 8 MB flash, 8 MB octal PSRAM. USB-Serial/JTAG console.
- **Power chip M5PM1** on I2C 0x6E (SDA 47, SCL 48, 100 kHz). It owns the rails, and nothing else works
  until it is set:
  - its GPIO2 (`L3B`) powers the LCD and the audio side and connects the codec to I2C;
  - its GPIO3 enables the speaker amp (AW8737), which has a pull-down, so the GPIO must be push-pull;
  - registers: `0x10` GPIO direction, `0x11` GPIO out, `0x12` GPIO in, `0x13` open-drain (1 = OD,
    the default), `0x06` power config (bit 3: 5 V out, bit 4: green LED), `0x22`/`0x24` battery/USB mV
    (16-bit little-endian).
  - The init order in the reference: push-pull for L3B and SPK → direction out → amp off → L3B on →
    5 V out off → LED off → 20 ms. The PMIC keeps its state across ESP resets, so code that only works
    after a cold power-up, or only after another firmware ran, is a bug.
- LCD: ST7789, 1.14", 135×240, in the controller's 240×320 at offset x 52, y 40. SPI3: SCLK 40,
  MOSI 39, CS 41, DC 45, RST 21. Backlight on GPIO38 via LEDC PWM. No touch.
- Audio: ES8311 at its default I2C address on the same bus. One duplex I2S port: MCLK 18, BCLK 17,
  WS 15, DOUT 14, DIN 16. **One mic, on the left slot of a stereo frame** (reference: stereo slot mode,
  `mic_slot = 0`). The amp enable goes through the PMIC (`pa_pin` routed to its GPIO3).
- Keys: KEY1 on GPIO11 (the big front key under the screen), KEY2 on GPIO12 (side). Active low. A key
  that woke the board can still be down at boot.
- No BOOT key that the user can reach in normal use. Flashing needs manual download mode (Claude
  handles that).

## Do

1. **Board.** `COMMA_BOARD_M5STACK_STICKS3`, board ID `m5stack-sticks3`, overlay
   `devices/sdkconfig.m5stack-sticks3`, `tools/board.sh` target `esp32s3`, 8 MB flash, octal PSRAM,
   and a partition table with the app and `model` partitions like the T-Circle (state the offsets).
   Voice on, display on. Pairing: passkey shown on the screen, as on the T-Circle.
2. **PMIC.** A small driver for the registers above, run first in `board_init`. Log what it reads
   back (L3B, SPK, battery and USB mV) once at boot. Amp off at boot and whenever nothing plays.
3. **Audio.** Implement `voice/board_audio.h` for the ES8311:
   - one I2S rate for both directions, in **stereo** slot mode; the mic is slot 0 (de-interleave in
     the board layer); playback is written to both slots, or as the codec needs;
   - the shared pipeline wants 16 kHz mic and plays 24 kHz: pick the I2S rate and resample in the
     board layer (`voice/resample.c` exists and is tested), and justify it with the CPU cost;
   - the Waveshare test showed the mic at ~1.9× real time with the H3 driver. Do not repeat it. Add a
     log line once per call, 10 s after the mic opens: mic samples delivered vs elapsed ms (must be
     16,000 ± 1 % per second), and a host test of the de-interleave plus resample counts;
   - start with the mic gain and speaker volume conservative (state the values); Claude tunes them on
     hardware against the T-Circle's `mic_rms` (~400 speaking) and `echo_rms`;
   - keep the echo gate (mic muted while agent speech plays) as on the T-Circle.
4. **Display.** Implement `ui/board_display.h` for 135×240 portrait. H3's UI draws square cards
   (`UI_CARD_SIZE`): extend `firmware/ui/` to a width × height card, without changing what the
   T-Circle or Waveshare draw (prove it, e.g. the 22 cards byte-identical as in H3). Logo, call
   states, pairing code (six digits must be readable), Wi-Fi cards. Screen sleep as on the other
   boards; backlight off while asleep. Keep the DMA source in internal RAM (H3 pitfall).
5. **Input.** KEY1 short press starts or ends a call. KEY2: hold 3–10 s → pairing mode, > 10 s →
   factory reset, with the same countdown cards as BOOT on the other boards (`app_poll`). A short
   KEY2 press only wakes the screen. Any press on a dark screen wakes it; KEY1 on a dark screen also
   dials, as a tap does on the T-Circle. A key held at boot is ignored until released.
6. **Dependencies.** `esp_codec_dev` (H3 used 1.6.2 for Waveshare before removing it) and the
   ST7789 driver from `esp_lcd` (built into IDF), exact versions, gated so that only this board pulls
   new components, with its own lock file, as H3 did for Waveshare. No LVGL, no M5 BSP.
7. **Tests and builds.** All host suites (C under ASan/UBSan, Python in `tools/.venv`) and the new
   audio test. Build all four boards. T-Circle, DevKit and Waveshare generated configs and image
   sizes must be unchanged (show the diff and sizes).

## Rules

- Do not flash any board. Claude tests on hardware.
- No change to the Connector protocol, BLE contract, app or backend. `metadata` may differ only in
  the board ID and the board's own limits.
- No behaviour change on the other three boards.
- Do not commit Wi-Fi passwords, tokens, test certificates, generated sdkconfig or build outputs.
- One commit on `codex/m5-sticks3`. Don't merge, don't push.

## Report

Write `docs/reports/H4-report.md`: pin map, PMIC sequence and readback, partition table, audio rate
choice and its CPU cost, gain/volume values, display layout at 135×240, input rules, dependencies
and licenses (Meta Apache-2.0 attribution), memory (internal free/largest, PSRAM, stack watermarks
from a `COMMA_HEAP_DIAGNOSTICS` test build), test counts, proof that the other boards are unchanged,
and the hardware test plan with expected log lines:

1. flash backup (Claude, ROM mode: `esptool --no-stub read-flash`, the stub read fails on these
   USB-JTAG boards on macOS), full flash, boot log including the PMIC readback;
2. BLE setup from the app with the passkey on screen; Connector online;
3. a call by KEY1: ringback, greeting audible, the agent hears the caller, hang up by KEY1; `cue:`
   stats over 60 s and the mic rate line;
4. KEY2 holds: pairing mode, then factory reset;
5. screen sleep/wake; discovery with probes; `http_request` (text and 1 MiB chunks).

Update `README.md` and `devices/README.md` (StickS3 row: "built, not yet tested on hardware").
Reply with the commit hash and a short summary.

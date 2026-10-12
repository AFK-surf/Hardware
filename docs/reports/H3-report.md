# H3 — Shared voice/screen code, and the Waveshare ESP32-S3-Touch-AMOLED-1.32

2026-10-12. Base `8ca50cd` (main). Worktree `~/Desktop/AFK/Hardware-h3`, branch
`codex/waveshare-amoled`. ESP-IDF **6.0.2**.

| Commit | Content |
| --- | --- |
| A `bfb9e73` | Split: shared `firmware/voice/` and `firmware/ui/`, T-Circle board layer. No behaviour change. |
| B (this report's commit) | Waveshare AMOLED board, 24→16 kHz resampler, BOOT-press calls, size-aware fonts/logo. |

Nothing was flashed. **The Waveshare board is built, not yet tested on hardware**; A must be
retested on the T-Circle before B is reviewed. No merge, no push, no protocol/BLE/app/backend change.

## 1. Layout and board interface

```
firmware/
  main/                 Connector, LAN, Wi-Fi, BLE setup, BOOT holds (app_poll), Kconfig  (unchanged roles)
  voice/                shared voice call, built with CONFIG_COMMA_HAS_VOICE
    cue_voice.c/.h        comma.voice.v1 session (moved, 2 lines changed)
    playback.c/.h         agent audio, ringback, tones (moved, unchanged)
    voice_clean.c/.h/.lf  V2 AFE: VAD, optional nsnet2 (moved, unchanged)
    call.c/.h             call controller, from the T-Circle board.c
    resample.c/.h         24 -> 16 kHz mic resampler (B; only the Waveshare links it)
    board_audio.h         board interface: audio
    board_input.h         board interface: tap and BOOT
  ui/                   shared round screen, built with CONFIG_COMMA_HAS_DISPLAY
    display.c/.h          what to show, sleep timer, logo/card drawing, board.h for display boards
    ui_card.c/.h          card layout for UI_CARD_SIZE = CONFIG_COMMA_DISPLAY_SIZE
    ui_font.h, cue_logo.h
    160/  ui_font.c, cue_logo.bin   (T-Circle, moved, byte-identical)
    466/  ui_font.c, cue_logo.bin   (Waveshare, B)
    board_display.h       board interface: panel
  boards/t_circle_s3/     board.c (init order, help text), board_audio.c, board_touch.c, panel.c
  boards/waveshare_amoled_1_32/  board.c, board_pins.h, board_audio.c, board_touch.c, panel.c,
                                 waveshare_deps/ (managed drivers, this build only)
  boards/esp32_devkit/    unchanged
```

A board folder's `*.c` are all built (`main/CMakeLists.txt` globs the selected folder); shared
sources are added by feature. Nothing in `voice/` or `ui/` includes a board header:
`cue_voice.c`'s unused `board_touch.h` include is gone, and session.start's client is now
`CONFIG_COMMA_CLIENT_SOURCE "/0.1"` (still `t-circle/0.1` on the T-Circle).

**Audio** (`voice/board_audio.h`): `board_audio_init`, `board_audio_set_speaker_enabled`,
`board_audio_set_recording`, `board_audio_read_mono` (mono PCM16 at `MIC_SAMPLE_RATE_HZ`) and
`board_audio_play_pcm16_mono` (at `AUDIO_SAMPLE_RATE_HZ`). The two rates are the pipeline's
(24 kHz speaker/wire; mic 16 kHz with V2, else 24 kHz). A board whose hardware runs at another rate
resamples inside its own layer.

**Input** (`voice/board_input.h`): `board_input_init`, `board_input_poll` (returns
`BOARD_INPUT_TAP` when a <280 ms touch lifts), `board_input_boot_held` (debounced). BOOT holds stay in
`app_poll`; the short-press rule is shared in `call.c` (section 6).

**Display** (`ui/board_display.h`): `board_display_init` (panel up and dark, framebuffer
allocated), `board_display_frame` (framebuffer once free, or NULL), `board_display_flush` (send it;
an error means "not on the panel, try later"), `board_display_sleep`, `board_display_wake`,
`board_display_on`, `board_display_light`. `ui/display.c` keeps what to show and when to sleep. A
failed frame is logged (`display: frame not sent: …`), kept dirty and retried 500 ms later; the loop
never waits on the panel.

**Call controller** (`voice/call.h`): `call_voice_clean_init` (AFE at boot), `call_init`
(temperature sensor, playback task), `call_run_ui(help)` (the UI loop: tap/BOOT to call, guards,
call cards, notices, temperature log). Each board's `board_init` calls them in the T-Circle's
original order: audio, AFE, input, temperature+playback, display.

New Kconfig: `COMMA_DISPLAY_SIZE` (160 default, 466 for the Waveshare), `COMMA_BOOT_PRESS_CALLS`
(off; on for the Waveshare), and the `COMMA_BOARD_WAVESHARE_AMOLED_1_32` choice.

## 2. T-Circle is unchanged (commit A, rechecked after B)

**Config.** The generated `sdkconfig`, regenerated from defaults after B, differs from `8ca50cd` only by
new keys at their defaults:

```text
# CONFIG_COMMA_BOARD_WAVESHARE_AMOLED_1_32 is not set     (B)
CONFIG_COMMA_DISPLAY_SIZE=160                              (A)
# CONFIG_COMMA_BOOT_PRESS_CALLS is not set                 (A)
```

`dependencies.lock` and `dependencies.lock.esp32` are unchanged (not touched by any build).
DevKit's generated `sdkconfig` is byte-identical and its image is the same 1,261,219 bytes.

**Size** (`esp_idf_size`, IDF 6.0.2):

| T-Circle, bytes | `8ca50cd` | A | B |
| --- | ---: | ---: | ---: |
| Total image | 2,032,163 | 2,032,451 | 2,032,451 |
| Flash code | 1,516,766 | 1,516,990 | 1,516,990 |
| Flash data | 351,640 | 351,704 | 351,704 |
| DIRAM | 179,001 | 179,001 | 179,001 |
| External BSS (`voice_clean.lf`) | 33,876 | 33,876 | 33,876 |

+288 bytes (0.014 %). Archive deltas: `libmain.a` +231 (new function boundaries, the two display
error strings), `libesp_stdio.a` +56; every other archive is equal. DIRAM and the external BSS are
identical, so `voice_clean.lf` still moves nsnet3's 19 KB `.bss` to PSRAM. B adds nothing to the
T-Circle image: `resample.c` is compiled but dropped at link, and the BOOT-press and diagnostics
code is compiled out.

**Log lines.** Every `ESP_LOG*` format string of the old T-Circle files is present with the same tag,
except the help line, which `board.c` now passes to `call_run_ui("%s")` with identical text. Added
strings are on paths the T-Circle cannot reach: `display: framebuffer still busy` (its frame call
blocks as before), `display: frame not sent` (its flush still `ESP_ERROR_CHECK`s), the BOOT-press
line (compiled out). `cue:` stats are produced by unchanged code. Pairing passkey flow is untouched
(`provisioning.c` unchanged).

**Cards.** `tools/card_preview.c` renders all 22 cards byte-identically with the old and new
`ui_card.c`/fonts at 160 px; card spacing is now `UI_CARD_SIZE * {8, 14, 48} / 160`, which is
exactly 8, 14 and 48 at 160. Timings (`NOTICE_US`, `CALL_OVER_US`, `DIAL_PENDING_US`,
`HANGUP_GUARD_US`, `LISTENING_AFTER_US`, 15 s screen sleep) are the same constants, moved.

## 3. Waveshare board

| Item | Value |
| --- | --- |
| Kconfig / board ID / client source | `COMMA_BOARD_WAVESHARE_AMOLED_1_32` / `waveshare-amoled-1-32` / `waveshare-amoled-1-32` |
| Overlay / target | `devices/sdkconfig.waveshare-amoled-1-32` / `tools/board.sh waveshare-amoled-1-32` → `esp32s3` |
| Flash / PSRAM | 8 MB DIO 80 MHz / 8 MB octal 80 MHz (the reference's working modes) |
| Features | voice, display 466, passkey + MITM, BOOT-press calls, discovery; all LAN limits = T-Circle |
| Image | 2,186,843 bytes; 4 MB app partition 48 % free; DIRAM 180,409 (+1,408 vs T-Circle: resampler state); external BSS 33,876 |

Config against the T-Circle's: only flash size (8 MB), board choice/ID/client source, display size,
`COMMA_BOOT_PRESS_CALLS=y`, and esp_codec_dev's menu (only `CODEC_ES8311_SUPPORT=y`). The overlay
repeats the T-Circle's PSRAM/ESP-SR/run-time-stats keys (needed for the shared voice code). Metadata
differs only in `client_source`/component key; limits are the T-Circle's.

### Pin map (Waveshare BSP `esp32_s3_touch_amoled_1_32.h`)

| Function | GPIO |
| --- | --- |
| I2C SDA / SCL (ES8311 control + CST820, one bus, I2C0) | 47 / 48 |
| I2S MCLK / SCLK / WS / DOUT (to codec) / DIN (from codec), I2S0 | 38 / 39 / 41 / 42 / 40 |
| Speaker PA enable (active high) | 46 |
| CO5300 QSPI CS / PCLK / D0–D3 / RST (SPI2) | 10 / 11 / 12–15 / 8 |
| CST820 RST / INT (INT unused, input pull-up) | 7 / 6 |
| BOOT (active low) | 0 |

### Partition table

The T-Circle's `partitions.csv` unchanged; it ends at 0x510000, inside 8 MB (0x800000):

| Name | Offset | Size |
| --- | --- | --- |
| bootloader / partition table | 0x0 / 0x8000 | — |
| nvs | 0x9000 | 0x6000 |
| phy_init | 0xF000 | 0x1000 |
| factory (app) | 0x10000 | 0x400000 (4 MB) |
| model (ESP-SR) | 0x410000 | 0x100000 (1 MB) |
| unused | 0x510000 | 0x2F0000 (2.94 MB) |

### Dependencies and licenses

| Package | Version | Why | License |
| --- | --- | --- | --- |
| `espressif/esp_lcd_co5300` | **2.1.0** (IDF ≥ 5.4) | panel driver; the version the reference ran on this board | Apache-2.0 |
| `espressif/esp_codec_dev` | **1.6.2** (IDF ≥ 4.0; uses `i2c_master` on IDF ≥ 5.3) | ES8311 registers only; latest non-beta (2.0 is beta) | Apache-2.0 |
| `espressif/cmake_utilities` | 0.5.3 | pulled by esp_lcd_co5300 | Apache-2.0 |

They are declared in `boards/waveshare_amoled_1_32/waveshare_deps/idf_component.yml` (public
requirements), a component that only the Waveshare build adds, with its own lock
`firmware/dependencies.lock.waveshare-amoled-1-32` (committed). That keeps the T-Circle and DevKit
locks untouched; a target rule could not separate two esp32s3 boards. `tools/board.sh` passes
`-DCOMMA_BOARD=<board>`; `firmware/CMakeLists.txt` stores it as a build property, which
`main/CMakeLists.txt` reads in IDF's early requirements pass (before Kconfig). A plain `idf.py` build
with the Waveshare Kconfig choice stops with a message to use `tools/board.sh`. The Waveshare BSP and
LVGL are not used; CST820 is a small driver of ours over `i2c_master`. No `update-dependencies`.

Adapted code: the CO5300 init table and 6-column gap in `boards/waveshare_amoled_1_32/panel.c`
come from the Waveshare BSP (`esp32_s3_touch_amoled_1_32.c`, Apache-2.0); the file carries that
notice. Pins in `board_pins.h` are from the BSP header. The ES8311 setup sequence, gains and PA
handling follow our own voice-box-agent `board_audio_waveshare.c`. Everything else is new.

## 4. Audio: one I2S clock at 24 kHz

ES8311 is full duplex on one I2S port, so mic and speaker share one sample clock. **I2S runs at
24 kHz** (MCLK 6.144 MHz, a rate in the ES8311 coefficient table); playback goes out as received;
the mic is read at 24 kHz and resampled to 16 kHz in the board layer for V2's AFE.

Why: the alternative, 16 kHz I2S, would resample the agent's audio 24→16 kHz and cut it to an 8 kHz
band; the cost is the same one resampler either way. 48 kHz would need two resamplers and twice the
DMA. With `COMMA_VOICE_CLEAN` off the mic rate is 24 kHz and the board skips the resampler.

`voice/resample.c`: polyphase Kaiser-windowed sinc (96 taps at 48 kHz, 48 per output phase,
β 6.5), DC gain exactly 1, ±0.06 dB to 6 kHz, −7.8 dB at 7 kHz, ≥ 73 dB down from 8 kHz (no
folding into the speech band), ~1 ms delay. 15,360 MACs per 20 ms frame. Host test numbers below.

CPU: the T-Circle's call stats were cpu0/cpu1 38/45 % (H2, no discovery); the resampler should add
about 1 % of one core. **Measured on hardware at boot** by `audio: mic resampler: 1 s of audio in N
us (X% of one core)`; compare `cue: voice … cpu0 cpu1 afe_ms` with the T-Circle in the test.

Other choices:

- **Echo gate unchanged** (shared `cue_voice.c`): mic frames are zeros while agent speech plays and
  300 ms after.
- **PA (GPIO46)**: configured low first in `board_audio_init`, before I2S and before
  `es8311_codec_new()` (which soft-resets the codec); the codec driver never drives it
  (`pa_pin = NC`). On: unmute DAC, 2 ms, PA high, 5 ms. Off: PA low, 2 ms, mute DAC. The shared
  playback task turns the speaker off 150 ms after the last audio, so the PA is low whenever nothing
  plays.
- **RX never disabled.** On the S3, disabling the RX channel also stops TX on the same port
  (esp_codec_dev notes this for `SOC_I2S_HW_VERSION_1`). Starting a recording reads out what the DMA
  collected meanwhile, then discards one 10 ms block, as the T-Circle discards its first frame.
- **Codec through `audio_codec_if_t`** (set_fs, enable, mic gain, volume, mute), not
  `esp_codec_dev_open`: that would try to reconfigure our already-running I2S channels, which IDF 6
  refuses (`I2S should be disabled before reconfiguring`). The reference used `esp_codec_dev_open`
  on enabled channels, so its reconfigure calls must have failed and the channels kept its own config.
- **Levels (unverified):** mic PGA 24 dB (the reference's, used for speech recognition), DC block at
  12.8 Hz, no extra digital gain; DAC −10 dB on esp_codec_dev's scale (the reference's volume 95 is
  −2.5 dB, so ~8 dB quieter to start), DC block 120 Hz and the T-Circle's 6 ms fade-in. Targets from
  V2: `mic_rms` ~400 speaking at arm's length (office noise ~380), `echo_rms` median ~10,000 on the
  T-Circle. Adjust `WS_MIC_GAIN_DB` / `WS_SPEAKER_DB` in `board_audio.c` from the test.

## 5. Display

- Framebuffer **434,312 B in PSRAM** (`COMMA_BUFFER_CAPS`), drawn by the shared card/logo code.
- **DMA source: one 14,912 B internal DMA buffer** (466 × 16 rows × 2 B, under the 16 KB
  `SPIRAM_MALLOC_ALWAYSINTERNAL` line), allocated at boot before Wi-Fi. Each frame goes out as 30
  stripes of 16 rows (last one 2), each copied from PSRAM, so the SPI driver never reads PSRAM or
  allocates a bounce buffer.
- A stripe that fails to queue returns the error at once (the buffer is reclaimed, no callback will
  come); a stripe whose done-callback does not arrive in 200 ms (~250× its transfer time) is logged
  `panel: stripe transfer lost` and reclaimed. Either way the frame fails, `display.c` logs it and
  retries in 500 ms. No wait is unbounded in our code.
- **Even alignment:** stripes span the full width (columns 6..471 after the BSP's 6-column gap) and
  start on even rows with even heights; static asserts guard the constants. No partial updates: a
  card change redraws the whole frame (once a second during a call for the timer).
- **Light:** no backlight pin. The BSP init table is used except brightness 0 at init, so the panel
  stays dark until the first frame; then brightness 0xCC (80 %). Sleep after 15 s idle as on the
  T-Circle: brightness 0, wait for the stripe, DISPOFF, SLPIN, 120 ms. Wake: SLPOUT, 120 ms, frame,
  DISPON, brightness.
- **466 cards and logo:** fonts regenerated from Inter at 41 px (small) and 105 px (large), the
  T-Circle's 14/36 px scaled by 466/160; logo 291 px (100 px scaled). Same text, same layout rules;
  each line is wrapped to the chord of the round glass at its row minus the scaled margin, so text
  stays inside the circle (checked by rendering all 22 cards with a circle overlay). Generators:
  `tools/gen_ui_fonts.py 466`, `tools/gen_cue_logo.py 466`; they reproduce the 160 assets
  byte-for-byte (the font generator no longer ends the file with a blank line, the only change to
  `ui/160/ui_font.c`). Flat fills only, no gradients/shadows/animation.

## 6. Input rules

- **Tap** (CST820, polled every 10 ms as on the T-Circle): a touch that lifts within 280 ms calls or
  hangs up. Touch is not the only way: if the CST820 is absent or damaged, BOOT still calls
  (`touch: CST820 probe failed: …; BOOT still calls`). Auto-sleep is switched off with register
  0xFE (CST816-family convention, best effort, unverified on CST820); a failed read counts as no
  finger, and NACKs only log at debug level.
- **BOOT short press** (`COMMA_BOOT_PRESS_CALLS`, shared in `call.c`): a press is told apart from the
  start of a hold **only at release**. If BOOT was down for less than 3 s (`APP_PAIRING_HOLD_US`,
  the same constant `app_poll` uses), the release is the call key, exactly like a tap: it dials,
  hangs up, leaves pairing mode on a provisioned board, and obeys the 500 ms hang-up guard. Nothing
  happens at the press itself except the screen waking (as before). From 3 s on it is `app_poll`'s
  hold: release in 3–10 s → pairing mode; 10 s → factory reset; unchanged. A key held at boot is
  ignored until released, as in `app_poll`.
- Log line for a press: `main: BOOT press (N ms): call key`.

## 7. Memory

Static (linker): DIRAM 180,409 B (T-Circle 179,001), external BSS 33,876 B (same).
Runtime allocations that differ from the T-Circle: +14,912 B internal (DMA stripe); I2S DMA about
5.8 KB internal (two 6 × 240 mono buffers) against the T-Circle's ~11.3 KB (two ports); framebuffer
434 KB PSRAM (T-Circle 51 KB). Net, expect roughly 9 KB less internal heap than the T-Circle, whose
call-time minimum was above 88 KB (H1).

**Not measured: the board has not run.** A diagnostics build exists for the test
(`CONFIG_COMMA_HEAP_DIAGNOSTICS=y`, built in `firmware/build-waveshare-diag/`, compiles without
warnings). With diagnostics on, `call.c` now also logs, every 60 s idle / 10 s in a call:

```text
heap: ui_idle|ui_call free=… largest=… min=… free8=… largest8=… task=ui stack_free=…
heap: psram free=… largest=… min=… stack_free display=… play=… sender=…
```

The uplink task's own stack is in the existing `cue: voice … stack_free=` field.

| Point | Internal free / largest / min | PSRAM free | Stack free (ui / display / play / sender / uplink) |
| --- | --- | --- | --- |
| Boot, before Wi-Fi (`heap: boot_before_wifi`) | pending | pending | pending |
| Connector online, idle | pending | pending | pending |
| In a call, 60 s | pending | pending | pending |
| After the call | pending | pending | pending |

## 8. Host tests

All C suites under ASan/UBSan, after B:

| Suite | Result |
| --- | --- |
| `resample_test` (new) | 48,625 checks, 0 failed |
| `lan_discover_test` default / `--devkit` / `--no-probes` | 2387 + runner 7574 / 2386 + 4502 / 1276 + 24, 0 failed |
| `lan_http_test` default / `--devkit` | 263 / 263 passed |
| `lan_udp_test` default / `--devkit` | 747 / 747 |
| `lan_tunnel_test` default / `--devkit` | 1366 / 1367 (count varies by one with timing, as in H2b) |
| `connector_json_test` | 133 |
| Python `unittest discover -s firmware/tools` (main checkout's `tools/.venv`) | 26 tests, OK |

The same suites passed after A. The resampler test checks unity DC gain and per-phase tap sums,
the int32 accumulator bound, the frequency response (100 Hz–6 kHz within 0.2 dB, 7 kHz between −3 and
−9 dB, 8–11.9 kHz below −55 dB), identical output for seven chunk sizes, invalid lengths, and that
a full-scale square wave matches a 64-bit reference with clamping (saturates, never wraps).
`tools/card_preview.c` builds at 160 and 466 with `-Wall -Wextra -Werror`.

Builds: `tools/board.sh t-circle-s3 build`, `esp32-devkit build`, `waveshare-amoled-1-32 build`, all
without warnings; T-Circle and DevKit regenerated from defaults.

## 9. Hardware test plan (for Claude)

Ports: check `ls /dev/cu.usbmodem*` and `lsof` before flashing. Don't run two monitors on a port.

### 1. T-Circle after commit A

```sh
git checkout bfb9e73   # in the H3 worktree; or test the branch head, which builds the same image
tools/board.sh t-circle-s3 build && tools/board.sh t-circle-s3 flash-monitor <PORT>
```

`app-flash` is enough if the board already has this partition table and its model partition.
Expect the same boot as H2: `main: board t-circle-s3; PSRAM 8388608 bytes`, `audio: T-Circle audio
ready: PDM RX I2S0 at 16000 Hz, MAX98357A TX I2S1 at 24000 Hz, speaker gain 3.75`, `clean: AFE
ready: …`, `touch: CST816D ready: id=0x…`, `main: tap the screen to call, tap again to hang up; hold
BOOT 3 s to pair, 10 s to reset`, logo on screen, `display: screen off` after 15 s.

- Call: tap → `cue: dialing wss://…`, ringback, `cue: sent session.start`, `cue: session.started …`,
  greeting audible; talk; tap → `cue: hangup requested`, `cue: call over: ended (tap=1 …)`, "Call
  ended" card, logo 3 s later. Compare `cue: stats` and `cue: voice` with H2's table (cpu0/cpu1
  ~38/45 %, `mic_dropped` 0, `nrtx_max` 0, `mic_rms` speaking ~400).
- BOOT 3–10 s: "Let go to pair" countdown, release → `prov: provisioning started (pairing mode)`;
  tap leaves it on a provisioned board.
- One discovery (`fake_connector.py --discover-probes` or the app): PASS as in H2.

### 2. Waveshare: backup, flash, functions

**Full flash backup first** (8 MB), with the board in download mode if needed:

```sh
python -m esptool --chip esp32s3 -p <PORT> -b 921600 read-flash 0 0x800000 waveshare-backup.bin
```

Flash the full set (bootloader, partition table, app, model): `tools/board.sh waveshare-amoled-1-32
flash-monitor <PORT>`. If the chip stays silent with a white screen after `--after hard-reset`
(macOS quirk), unplug and replug USB without touching BOOT.

Expected boot lines:

```text
main: board waveshare-amoled-1-32; PSRAM 8388608 bytes
audio: Waveshare audio ready: ES8311 duplex I2S0 at 24000 Hz, mic to 16000 Hz, mic gain 24 dB, speaker -10.0 dB
audio: mic resampler: 1 s of audio in N us (X% of one core)          (expect X around 1)
clean: AFE ready: feed …, fetch … samples at 16000 Hz; …
touch: CST820 ready: id=0x…                                           (or a probe failure: report it)
panel: CO5300 ready: 466x466, framebuffer 434312 B in PSRAM, DMA stripe 14912 B internal
main: tap the screen or press BOOT to call, again to hang up; hold BOOT 3 s to pair, 10 s to reset
panel: first frame queued in N ms
```

Then the logo, centred and round, nothing lit before it. No `panel: stripe transfer lost`,
`display: frame not sent` or `ESP_ERROR_CHECK` abort, during boot or when Wi-Fi starts.

- **BLE setup** from the app: `Pairing code` card with six big digits, matching the phone; Wi-Fi
  cards; `All set`. Connector online: `connector: connected run=… device=…`; on-wire metadata
  `client_source` `waveshare-amoled-1-32`, same methods/limits as the T-Circle.
- **Call by BOOT short press** (< 1 s): `main: BOOT press (N ms): call key`, then `cue: dialing …`,
  ringback audible, "Calling", greeting audible, "Comma 0:0x Listening/Speaking". Talk: the agent
  answers (caller transcript in `cue: transcript role=caller`). End with another short BOOT press
  → `cue: hangup requested`, end tone, "Call ended".
- **Call by tap**, end by tap: the same lines without the BOOT line.
- `cue:` stats over 60 s: `mic_dropped` 0, `send_max_ms`/`queue_ms` like H2, cpu0/cpu1 within a few
  points of the T-Circle, `afe_ms` like the T-Circle. Levels: `mic_rms` speaking at arm's length
  near the T-Circle's ~400 and clearly above room noise; `echo_rms` while the agent speaks in the
  same range as the T-Circle (median ~10,000) or lower. If the agent is too loud/quiet or the mic too
  hot/low, change `WS_SPEAKER_DB` / `WS_MIC_GAIN_DB` and record the values. Listen for pops when
  the PA switches (`audio: speaker PA enabled/disabled via GPIO46`).
- **PA**: with a meter or by ear, no hiss when idle; GPIO46 low except while sound plays.
- **Screen sleep/wake**: `display: screen off` 15 s after the last activity (not during a call);
  tap or BOOT press → `display: screen on`, frame drawn, then the light. A BOOT press on a dark
  screen also dials (as a tap does on the T-Circle).
- **Discovery with probes** (`fake_connector.py --discover-probes --discover-timeout 5000 --loop 3`
  or the app): PASS, Gree answers uncut, like the T-Circle in H2.
- **`http_request`** (text and 1 MiB chunks) to the Mac's HTTP server: PASS.
- **Memory** with the diagnostics build (`firmware/build-waveshare-diag/`, flash with
  `idf.py -B firmware/build-waveshare-diag -p <PORT> app-flash monitor` from `firmware/`): fill
  section 7's table from `heap:` lines at boot, idle, 60 s into a call and after it. Then flash
  the normal build again.

### 3. Waveshare: BOOT holds

- Hold 3–10 s: "Let go to pair" with a countdown from 7; release → `prov: provisioning started
  (pairing mode)`, no `BOOT press` line, no call.
- Hold just under 10 s: countdown reaches 1, release → pairing mode, no reset.
- Hold > 10 s: `main: factory reset`, `config: NVS erased: ESP_OK`, reboot to `config: no stored
  credentials`; BOOT still held at boot → `main: BOOT held at boot, ignored until released`, and
  the release does not dial.

Unverified until then: CST820 register 0xFE, ES8311 gain/volume, CO5300 SLPIN/SLPOUT timing, the
panel's real frame time, and every runtime memory figure.

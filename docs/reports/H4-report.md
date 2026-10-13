# H4 — M5Stack StickS3 (voice, screen, two keys)

2026-10-13. Base `ef89713` (H3 C, `codex/waveshare-amoled`; merged into `main` as `7076237` meanwhile). Worktree
`~/Desktop/AFK/Hardware-h4`, branch `codex/m5-sticks3`, one commit. ESP-IDF **6.0.2**. Not merged,
not pushed. No Connector protocol, BLE contract, app or backend change; `metadata` differs only in
the board ID / client source (`m5stack-sticks3`), with the T-Circle's limits.

**Status: board-tested. Boot, BLE setup with the on-screen passkey, Connector online, six KEY1
calls (one dialed 3 s after a reboot, during the Connector's TLS handshake), the mic rate check,
screen sleep/wake, KEY2 3–10 s pairing, discovery with probes 3/3, `http_request` text and 1 MiB
chunks 3/3, and the heap diagnostics all pass (section 9). Changes from the testing: a mic reader
task with a 320 ms ring (section 3), the speaker at −6 dB, a PMIC wake step, and boot logging of the
reset reason and PMIC state. Open: the KEY2 > 10 s factory reset did not trigger in its one try
(section 5), and the PMIC's voltage/power-source registers read implausibly.**

## 0. Incident during the hardware bring-up: the Waveshare was flashed by mistake

The StickS3 (MAC `14:c1:9f:d4:ed:20`) was backed up on 2026-10-12 22:47 from
`/dev/cu.usbmodem1101`. The first flash, on 10-13 at 07:50, went to the same port without
re-reading the chip's MAC. By then the board on that port was the **Waveshare Comma-219C** (MAC
`70:04:1d:dc:21:9c`). It got `erase-region 0x9000 0x6000` (its NVS: Wi-Fi, Connector token, voice
key) and the full StickS3 image (bootloader, partition table, app, model at 0x700000). It booted
that image (`main: board m5stack-sticks3`, every M5PM1 access NACKed), then went offline when it
was unplugged. Eight minutes earlier (`89c8891`, 07:42) it had been verified online running H3 C
(`ef89713`, now in `main`). H3's factory-firmware backup exists (H3 report), but nothing holds its
provisioned NVS. It needs H3 C flashed again with `flash` (the partition table differs), and BLE
setup from the app. Its pins driven by the
StickS3 image (14/15 = CO5300 D2/D3, 38–41 = I2S, 21, 45) are logic-level; no damage expected but
check the panel. From now on the MAC is read immediately before every flash or erase.

**Recovery, 2026-10-13 evening:** the same Waveshare was reattached with a blank screen, still
running the incorrect StickS3 image. After MAC verification, its correct normal firmware was
restored and the user confirmed the display recovered. Re-provisioning and the HTTP/UDP,
discovery and tunnel checks then passed, as recorded in the
[firmware fix validation report](2026-10-13-firmware-fixes-hardware.md#waveshare-amoled).

## 1. Board

| Item | Value |
| --- | --- |
| Kconfig / board ID / client source | `COMMA_BOARD_M5STACK_STICKS3` / `m5stack-sticks3` / `m5stack-sticks3` |
| Overlay / target | `devices/sdkconfig.m5stack-sticks3` / `tools/board.sh m5stack-sticks3` → `esp32s3` |
| Chip | ESP32-S3-PICO-1 (LGA56) rev v0.2, 8 MB GD flash (DIO 80 MHz), 8 MB AP octal PSRAM (80 MHz) |
| Features | voice, display 135×240, passkey + MITM, discovery, 2 tunnels; all LAN limits = T-Circle |
| Image | 2,055,407 B (`esp_idf_size`), app partition 0x6F0000: 71 % free. DIRAM 182,937 B, external BSS 33,876 B |
| Console | USB-Serial/JTAG (secondary console). Download mode is entered automatically over USB-JTAG: no keys needed |

### Pin map (M5 K150 schematic v0.6, via the reference)

| Function | GPIO |
| --- | --- |
| I2C0 SDA / SCL, 100 kHz: M5PM1 (0x6E), ES8311 (0x18) | 47 / 48 |
| I2S0 MCLK / BCLK / WS / DOUT (to codec) / DIN (from codec) | 18 / 17 / 15 / 14 / 16 |
| ST7789 SPI3 SCLK / MOSI / CS / DC / RST | 40 / 39 / 41 / 45 / 21 |
| Backlight (LEDC PWM 20 kHz, 10 bit, timer 0 ch 0) | 38 |
| KEY1 (front, under the screen) / KEY2 (side), active low, internal pull-ups | 11 / 12 |
| Speaker amp AW8737 enable | M5PM1 GPIO3 |
| LCD + audio rail (L3B), codec on the I2C bus | M5PM1 GPIO2 |

### Partition table (`firmware/partitions.m5stack-sticks3.csv`)

| Name | Offset | Size |
| --- | --- | --- |
| bootloader / partition table | 0x0 / 0x8000 | — |
| nvs | 0x9000 | 0x6000 |
| phy_init | 0xF000 | 0x1000 |
| factory (app) | 0x10000 | 0x6F0000 (6.94 MB) |
| model (ESP-SR) | 0x700000 | 0x100000 (1 MB; srmodels 626,016 B) |

Same nvs/phy/app start as the T-Circle; the app takes the room up to the model, which ends at 8 MB.
The stock M5 layout (backup, section 9) had nvs at 0x9000 too but 0x5000 long; the bring-up erased
0x9000–0xF000 before the first flash.

## 2. M5PM1 (`boards/m5stack_sticks3/pmic.c`)

First thing in `board_init`, before audio and the panel (both are unpowered until it runs):

1. **Wake and connect.** An address probe (a START: SDA falling edge wakes the PMIC), 10 ms, then
   read ID/model/hw/fw (0x00–0x03). If it NACKs: 800 ms, wake, retry; then the same at 400 kHz.
   Once it answers, `I2C_CFG` (0x09) = 0x00 (never sleep, 100 kHz), read back and checked. This step
   is not in the reference; it comes from M5Stack's M5PM1 library (MIT), facts only.
2. The reference's sequence: GPIO2/3 push-pull (0x13 bits clear) → GPIO2/3 outputs (0x10) → amp off
   (0x11 bit 3) → L3B on (0x11 bit 2) → 5 V out off (0x06 bit 3) → LED off (0x06 bit 4) → 20 ms.
   Every bit is written on every boot; nothing assumes a cold PMIC.
3. Readback, once, then the power state: power source (0x04) and wake/reset source (0x05) raw,
   the latter cleared after reading; the watchdog (0x0A) is turned off if another firmware left it
   on (nothing here feeds it). `board_init` also logs `esp_reset_reason()` first.

```text
main: reset reason: USB (11)
pmic: M5PM1 id 0x50 model 0x20 hw 0x05 fw 0x4f; answered on try 1 at 100 kHz, I2C_CFG was 0x00
pmic: M5PM1: L3B on, SPK off (dir 0x0c out 0x04 od 0x13 in 0x15 pwr 0x07), raw battery 108, USB 1130
pmic: M5PM1 power source 0x05; started by 0x00 (); watchdog 0 s
```

The watchdog was 0 on every boot, so it is not behind the restarts in section 5.

The amp is off at boot (`pmic_init`, again in `board_audio_init` before the codec reset) and
whenever nothing plays (the shared playback task turns the speaker off 150 ms after the last audio).
**The voltage and power-source registers are not trusted yet**: the first boot read battery
4080 mV (plausible), later boots 108–110 with USB at 1100–1132 on the same cable, and power source
0x05, which neither M5's header (0 USB/DC, 1 Grove, 2 battery) nor its library (0 none, 1 USB,
2 battery) defines. They are logged raw and nothing depends on them. M5Unified also clears PMIC
register 0x16 bit 3 (GPIO3 as plain GPIO) for the amp; the amp worked here without it, across
power-on resets, so it was left out.

Why the wake step exists: the very first boot on the StickS3's own hardware is the second log above,
and it answered on try 1; the NACKs in section 0 were on the Waveshare, which has no M5PM1. So the
wake path is defensive and has not yet been seen to be needed. It costs 10 ms when the PMIC is awake.

## 3. Audio (`board_audio.c`, `mic_path.c`)

- **One I2S port, full duplex, stereo slot mode in both directions, 24 kHz**, 16-bit Philips, MCLK
  6.144 MHz (256 fs; in the ES8311 coefficient table). DMA 6 × 240 frames (10 ms, 960 B each).
- **Mic**: slot 0 (left) of each stereo frame (`mic_take_slot`), then 24 → 16 kHz
  (`voice/resample.c`, unchanged), then the T-Circle's 12.8 Hz DC block and soft limiter. One I2S
  frame = one mic sample, so the mic rate is the I2S rate by construction. H3's Waveshare used mono
  slot mode with both slots active, which on the S3's RX delivers both slots as samples (~1.9×).
- **Speaker**: as received (24 kHz), the same sample written to both slots (as the reference),
  120 Hz DC block, 6 ms fade-in, soft limiter.
- **Why 24 kHz**: the pipeline plays 24 kHz and wants a 16 kHz mic. At 24 kHz only the mic is
  resampled; at 16 kHz the agent's audio would need a 16 → 24 kHz path and lose everything above
  8 kHz; at 48 kHz both directions need a resampler and twice the DMA. **Cost, measured on the
  board at boot**: `audio: mic resampler: 1 s of audio in 48933 us (4.89% of one core)` (slot pick
  plus resampler; the Waveshare measured 4.81 % for the resampler alone). With `COMMA_VOICE_CLEAN`
  off the mic stays at 24 kHz and nothing is resampled.
- **Rate check, once per call**: 10 s after the mic opens, samples delivered against elapsed time,
  plus the I2S frame rate as the RX DMA interrupt counts it and what was lost:
  `audio: mic rate: N samples in M ms = R/s, want 16000 +-1%: OK (I2S F frames/s; lost: D DMA
  blocks, S ring samples)` (an `E` line with `WRONG` outside 15,840–16,160).
- **Mic reader task and ring** (added after the first board call). The first call, dialed 3 s after
  a reboot while the Connector was still in its TLS handshake, logged `15527/s … WRONG`: 4,720
  samples (~295 ms) short in 10 s. With the DMA counters added, a later call showed `I2S 23997
  frames/s, 0 DMA blocks lost`: the clock is right; the voice task, which read the 60 ms DMA
  directly and also runs the AFE and the sends, had fallen behind (that first second: cpu0 85 %,
  `afe_ms` 479) and the DMA overwrote ~30 blocks. A deeper DMA would also delay the speaker (one
  descriptor setting for both directions in duplex) and eat into the 300 ms echo-gate tail. Instead
  `mic_rx` (priority 7, above play/sender 6 and uplink 5, 3 KB stack) drains the DMA every 10 ms
  always, and while recording takes the slot, resamples (`mic_path_24k_to_16k`) and appends to a
  **320 ms** stream buffer in PSRAM (10,240 B) that `board_audio_read_mono` reads. Starting a
  recording empties the ring and drops 10 ms. Loss is counted at both stages.
- **Codec**: `esp_codec_dev`'s ES8311 interface (registers only: set_fs, enable, mic gain, volume,
  mute), our own I2S channels, `pa_pin = NC`; the amp is ours through the PMIC. RX is never disabled
  (on the S3 that stops TX). Speaker on: unmute, 2 ms, amp on, 5 ms. Off: amp off, 2 ms, mute.
- **Echo gate**: unchanged shared `cue_voice.c` (mic zeros while agent speech plays and 300 ms after).
- **Levels**: mic PGA **24 dB** (the reference runs 30 dB): speaking `mic_rms` median 402–502 over
  three calls, the T-Circle's ~400; quiet room ~100. Kept. DAC started at −15 dB on
  esp_codec_dev's scale (hw_gain 5.0 V PA / 3.3 V DAC; the reference's default volume 70 on the
  default 0→−50 dB, 100→0 dB curve): too quiet by ear, `echo_rms` median 626, peak 1,613. Now
  **−6 dB** (`STICK_SPEAKER_DB`): "no problem" by ear, `echo_rms` median 1,787, peak 3,037, still
  well under the T-Circle's median ~10,000. The user found the voice clearly better than the
  T-Circle's.

## 4. Display (`panel.c`, shared `ui/`)

- ST7789, portrait in the controller's own orientation, window 135×240 at x 52, y 40, inverted
  colours, SPI3 at 40 MHz. Framebuffer **64,800 B in PSRAM**; **DMA source: one 4,320 B internal
  DMA stripe** (135 × 16 rows), 15 stripes per frame, each copied from PSRAM. Lost/failed stripes
  fail the frame (logged, redrawn in 500 ms), as on the Waveshare. First frame queued in **20 ms**.
- Backlight off until the first frame, then 80 %. Sleep after 15 s idle (shared `display.c`):
  backlight 0, wait for the stripe, DISPOFF, SLPIN, 120 ms. Wake: SLPOUT, 120 ms, frame, DISPON, light.
- **Rectangular cards.** `ui_card.h` now has `UI_CARD_W`, `UI_CARD_H`, `UI_CARD_ROUND`. Round boards
  keep `UI_CARD_SIZE` (W = H = SIZE, round); the StickS3 sets `COMMA_DISPLAY_WIDTH/HEIGHT` (135/240,
  Kconfig symbols that exist only for this board). Lines wrap to the round glass's chord, or to
  `W − margin` on a rectangle; spacing scales with the width (8/14/48 at 160 → 6/11/40 at 135).
  Assets live in `ui/<size>/`, size = `135x240` here.
- **Fonts / logo at 135×240**: small Inter 500 at **14 px** (as the T-Circle), big digits Inter 600
  at **32 px** ("482 019" is 115 px of the 124 px between margins; 36 px would be 132). Logo 84 px
  (100 px × 135/160), centred. `gen_ui_fonts.py 135x240`, `gen_cue_logo.py 135x240` (the logo
  packs two pixels per byte in raster order, which equals the old per-row packing for even widths).
  Both generators still reproduce the 160 and 466 assets byte for byte.
- **Other boards' cards unchanged**: `tools/card_preview.c` renders all 22 cards with the old
  (`ef89713`) and new `ui_card.c`: **22/22 byte-identical at 160 and at 466**. The 135×240 set was
  checked by eye (logo, Hello/Pairing mode, pairing code, Wi-Fi cards, countdown, call states): all
  inside the screen. An SSID wider than one line (`ToEverything-IOT`) is broken mid-word, the same
  rule as on the round screens.
- Card text that names the T-Circle's controls ("Tap to call again", "Hold BOOT 3 s…") is shared
  and unchanged; on the StickS3 "tap" means KEY1 and "BOOT" KEY2. Rewording needs a per-board string
  and was left out to keep the other boards byte-identical.

## 5. Input (`board_input.c`)

- **KEY1** (front): a press released within 1 s is `BOARD_INPUT_TAP`, so the shared `call.c` rules
  apply unchanged: call / hang up, the 500 ms hang-up guard, "dial when the last call has closed",
  leave pairing mode on a provisioned board. KEY1 on a dark screen wakes it and dials (as a tap on
  the T-Circle). Held > 1 s: logged, ignored.
- **KEY2** (side) is the board's BOOT for `app_poll`: 3–10 s → pairing mode, > 10 s → factory reset,
  with the shared countdown cards; a short press only wakes the screen (`app_poll` wakes it when the
  key goes down). `COMMA_BOOT_PRESS_CALLS` stays off.
- **Board test.** Short KEY2: screen on, no call. KEY2 held ~8.8 s: "Let go to pair" countdown,
  pairing mode on release, the app re-paired with the passkey. **> 10 s factory reset: did not
  happen in the one try.** The log shows KEY2 down at 181.0 s, released by our GPIO at 187.0 s
  (6.0 s, so pairing mode), then about 13 s after the press a power-on reset (`reset reason: power
  on`, settings kept). Whether the key was let go at 6 s or the PMIC acted (it has its own power
  button with hardware click-reset / double-click-off actions, registers 0x48–0x4A; M5Unified reads
  that button separately from KEY1/KEY2) is not known. The same kind of power-on restart appeared
  twice earlier during key presses. To check: hold KEY2 continuously for 15 s while logging, and
  read the PMIC's button status (0x48) and wake source at the next boot.
- Both debounced 20 ms, polled every 10 ms. A key down at boot: KEY1 is ignored until released
  (`keys: KEY1 released after being held at boot, ignored`); KEY2 by `app_poll` as before.

## 6. Dependencies and licenses

| Package | Version | Why | License |
| --- | --- | --- | --- |
| `espressif/esp_codec_dev` | **1.6.2** (hash `4779f31a…`, same as H3 B) | ES8311 registers | Apache-2.0 |
| `esp_lcd` (ST7789), `esp_driver_ledc`, `esp_driver_i2s/i2c/spi/tsens` | IDF 6.0.2 built-in | panel, backlight, buses | Apache-2.0 |
| `espressif/esp-sr` | 2.5.5 (unchanged, main manifest) | V2 AFE | Espressif |

`esp_codec_dev` is declared in `boards/m5stack_sticks3/sticks3_deps/idf_component.yml`, a component
only the StickS3 build adds (`firmware/CMakeLists.txt`), with its own lock
`firmware/dependencies.lock.m5stack-sticks3` (committed). A plain `idf.py` build with this board
choice stops with a message to use `tools/board.sh`. No LVGL, no M5 BSP/M5Unified.

Adapted code: `pmic.c`, `board_audio.c` and `panel.c` carry the **Meta Platforms Apache-2.0 header**
from `muse-gadget-sdk` (`components/muse/boards/board_m5stack_sticks3.c`, `muse_audio.c`): the PMIC
register map, retries and init order; the stereo duplex I2S, mic slot 0 and the codec
configuration; the ST7789 panel setup and backlight. Pins in `board_pins.h` are the reference's
(from M5's schematic); the overlay's memory/PSRAM choices also follow its
`sdkconfig.muse-m5stack-sticks3`. The PMIC wake handshake and `I2C_CFG` meaning come from
M5Stack's M5PM1 library (MIT, © 2025 M5Stack Technology), no code copied. The transfer scheme,
filters, resampler, mic path, rate check and input are ours (H3).

## 7. Memory

Static: DIRAM 182,937 B (T-Circle 179,001), external BSS 33,876 B (same), before the reader task
(3 KB internal stack, 1.4 KB static buffers; ring 10 KB in PSRAM). At boot, after the AFE:
`clean: AFE ready: … internal RAM used 9384 B, free 228291`. Runtime differences from the T-Circle:
+4,320 B internal (DMA stripe), I2S DMA ~11.5 KB internal (6 × 960 B × 2 directions) against the
T-Circle's two ports, framebuffer 64.8 KB in PSRAM.

A diagnostics build (`CONFIG_COMMA_HEAP_DIAGNOSTICS=y`) is built in
`firmware/build-m5stack-sticks3-diag/` without warnings (built with `idf.py -B … -DCOMMA_BOARD=m5stack-sticks3
-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;../devices/sdkconfig.m5stack-sticks3;<file with the key>"`).
Measured with the diagnostics build (`firmware/build-m5stack-sticks3-diag/`, `CONFIG_COMMA_HEAP_DIAGNOSTICS=y`;
it also logs the `mic_rx` stack watermark with the rate line), one 84 s call:

| Point | Internal free / largest / min | PSRAM free | Stack free |
| --- | --- | --- | --- |
| Boot, before Wi-Fi | 211,607 / 139,264 / 211,607 | — | main 6,184 |
| Connector online, idle (31 s) | 113,691 / 38,912 / 112,511 | 7.71 MB | ui 4,684, display 6,648, play 2,832 |
| In a call (10–80 s) | 75,631 / 31,744 / **73,071** | 7.44 MB (min 7.41 MB) | ui 4,524, display 5,992, play 2,032, sender 2,148, uplink 5,948, mic_rx 2,280 of 3,072, netdog 2,384, connector 3,256 |
| After the call | 113,371 / 31,744 | 7.59 MB | — |

During BLE setup (normal build) the internal minimum was 69,867. The call-time minimum, 73 KB, is in
the T-Circle's range (H1: above 88 KB at the time, before V2); no task stack is near its end.

## 8. Tests and other boards

Host suites (C under ASan/UBSan, on this branch):

| Suite | Result |
| --- | --- |
| `mic_path_test` (new) | **236,896 checks, 0 failed** |
| `resample_test` | 48,625 |
| `lan_discover_test` default / `--devkit` / `--no-probes` | 2387 + runner 7574 / 2386 + 4502 / 1276 + 24 |
| `lan_http_test` default / `--devkit` | 263 / 263 passed |
| `lan_udp_test` default / `--devkit` | 747 / 747 |
| `lan_tunnel_test` default / `--devkit` | 1364 / 1365 (varies by one with timing, as before) |
| `connector_json_test` | 133 |
| Python `unittest discover -s firmware/tools` (main checkout's `tools/.venv`) | 26 tests, OK |

`mic_path_test`: one I2S second gives exactly 16,000 samples and ten seconds 160,000, for read sizes
480, 240, 3, 9, 159 and 477 frames; the output is bit-identical to de-interleaving slot 0 first and
resampling, in every chunking, while the other slot carries loud noise; slot 1 selects the right
slot; unity DC on the mic slot with the other at full scale; `mic_take_slot` alone; bad arguments.

**Builds**: all four boards, no warnings. The three others were built from a clean `ef89713`
checkout and from this branch:

| | Generated `sdkconfig` diff | Image (`esp_idf_size`) | `.bin` |
| --- | --- | --- | --- |
| T-Circle | `+ # CONFIG_COMMA_BOARD_M5STACK_STICKS3 is not set` | 2,032,451 = 2,032,451 | 2,032,576, 73 bytes differ |
| DevKit | none | 1,261,219 = 1,261,219 | 1,261,344, 74 bytes differ |
| Waveshare | `+ # CONFIG_COMMA_BOARD_M5STACK_STICKS3 is not set` | 1,510,566 = 1,510,566 | 1,510,688, 73 bytes differ |

The differing bytes are the app descriptor (version `ef89713-dirty` vs `ef89713`, build time), the
ELF SHA-256 and the image hash. Flash code/data, DIRAM and external BSS are identical.
`dependencies.lock`, `dependencies.lock.esp32` and `dependencies.lock.waveshare-amoled-1-32` are
untouched.

## 9. Hardware test plan, and what has run

Ports: all S3 boards here appear as `/dev/cu.usbmodem1101`. **Read the MAC before every flash**
(`ioreg -p IOUSB -l -w0 | grep "USB Serial Number"`): StickS3 = `14:C1:9F:D4:ED:20` (BLE
`Comma-ED20`).

### 1. Backup, flash, boot — done

- Backup (2026-10-12 22:47, before anything was written):
  `~/Desktop/AFK/Hardware-backups/sticks3-14C19FD4ED20-2026-10-12.bin`, 8 MB,
  `esptool --no-stub read-flash 0 0x800000`, sha256 `edb4f9fd…3f2c`. A second read matched except
  615 bytes inside the stock nvs (the stock app wrote settings on the boot between reads). Stock
  layout: nvs 0x9000/0x5000, otadata, app0/app1 0x330000 each, spiffs 0x670000, coredump 0x7f0000.
  Restore: `esptool --chip esp32s3 -p PORT write-flash 0 <file>`. USB-JTAG entered download mode
  automatically; no key press was needed.
- Flash: `tools/board.sh m5stack-sticks3 flash PORT` (bootloader, partition table, app, model).
- Boot log on the StickS3 (key lines):

```text
boot:  2 factory          factory app      00 00 00010000 006f0000
boot:  3 model            Unknown data     01 82 00700000 00100000
main: board m5stack-sticks3; PSRAM 8354732 bytes
pmic: M5PM1 id 0x50 model 0x20 hw 0x05 fw 0x4f; answered on try 1 at 100 kHz, I2C_CFG was 0x00
pmic: M5PM1: L3B on, SPK off (dir 0x0c out 0x04 od 0x13 in 0x14 pwr 0x07), battery 4080 mV, USB 1100 mV
audio: mic resampler: 1 s of audio in 48933 us (4.89% of one core)
audio: StickS3 audio ready: ES8311 duplex stereo I2S0 at 24000 Hz, mic slot 0 to 16000 Hz, mic gain 24 dB, speaker -15.0 dB
clean: AFE ready: feed 512, fetch 512 samples at 16000 Hz; internal RAM used 9384 B, free 228291
keys: KEY1 GPIO11 up, KEY2 GPIO12 up
panel: ST7789 ready: 135x240, framebuffer 64800 B in PSRAM, DMA stripe 4320 B internal
panel: first frame queued in 20 ms
prov: advertising as Comma-ED20, mfg=FF FF 01 01 00 ED 20
main: press the front key to call, again to hang up; hold the side key 3 s to pair, 10 s to reset
```

No `E` line. (One `W phy_init: failed to load RF calibration data`, expected on a fresh nvs.) The
ES8311 answered (no `ES8311 not found`), so the L3B rail is up. Still to confirm by eye: the
"Hello / Open the Comma app to set up Comma-ED20" card is on the screen, upright and in colour.

Serial capture note: opening `/dev/cu.usbmodem1101` resets this board (`rst:0x15
USB_UART_CHIP_RESET`). Keep one capture open; don't reopen it mid-test.

### 2. BLE setup — passed

`prov: showing passkey` → `paired (LE Secure Connections, passkey)` → Wi-Fi `HUAWEI-F219BS`,
192.168.3.89 → `config: saved "Comma-ED20"` → `connector: connected … device=dev_t4MYkpS_aubL-eqZ_ci9dQ`
(read-only) → `prov: BLE off`. About 37 s from BLE connect to Connector up. Settings survived three
`app-flash`es.

### 3. Call by KEY1 — passed (four calls)

| Call | Build | Dialed | Rate line | Notes |
| --- | --- | --- | --- | --- |
| 1 | ffdadc9 | 3 s after reboot, during the Connector's TLS | `15527/s … WRONG` | ~30 DMA blocks overwritten (above) |
| 2 | + DMA counters | 80 s after boot | `15998/s OK (I2S 23997, 0 lost)` | 51.7 s, `mic_dropped` 0 |
| 3 | + reader task | 230 s after boot | `15998/s OK (I2S 23997; 0 DMA, 0 ring)` | 50.4 s, `mic_dropped` 0 |
| 4 | + −6 dB | 21 s after reboot (Connector up at 6.6 s) | `15999/s OK (I2S 23999; 0 DMA, 0 ring)` | 35.8 s, `mic_dropped` 0 |

Ringback, greeting and the agent's answers were heard; KEY1 hung up (`tap=1`, `caller_hangup`).
`afe_ms` 210–250, `sent_ms` ≈ 1000 per second while speaking, cpu0/cpu1 ~30/45–50 % after the first
second. The amp switches per sentence (`speaker amp enabled/disabled via M5PM1`); no pops reported.
| 5 | reader task, −6 dB | **3.06 s after a reboot**; Connector up at 7.8 s, mid call setup | `15998/s OK (I2S 23998; 0 DMA, 0 ring)` | 35 s, `mic_dropped` 0: the call 1 case, fixed |
| 6 | diagnostics build | 46 s after boot | `15999/s OK (I2S 23999; 0 DMA, 0 ring)` | 84 s, `mic_dropped` 0 |

Found in call 5, shared code, not changed: when a call starts while the Connector is still
connecting, the Connector receives no frame for 45 s (`last_heartbeat=never`), then reconnects by
itself (up again in ~4 s); the voice code logs `voice socket not identified (2 new connections)`.

Plan as written before the test:

KEY1 → `cue: dialing …`, ringback, "Calling", `audio: speaker amp enabled via M5PM1`, greeting
audible; talk, the agent answers (`cue: transcript role=caller`); KEY1 → `cue: hangup requested`,
end tone, "Call ended". **10 s after the mic opens**:
`audio: mic rate: ~160000 samples in ~10000 ms = 16000/s, want 16000 +-1%: OK` (anything else: stop
and look at the I2S slot setup). `cue:` stats over 60 s: `mic_dropped` 0, `sent_ms` per second
≈ 1000, `afe_ms` ≈ T-Circle (~250), cpu0/cpu1 within a few points of the T-Circle's 38/45 % plus
~5 %. Levels: `mic_rms` ~400 speaking at arm's length, `echo_rms` ≤ T-Circle median ~10,000; adjust
`STICK_MIC_GAIN_DB` / `STICK_SPEAKER_DB` and record the values. No hiss when idle (amp off), no pop
at amp switches.

### 4. KEY2 holds — pairing passed; factory reset open (section 5)

3–10 s → "Let go to pair" countdown from 7, release → `prov: provisioning started (pairing mode)`;
short KEY2 → screen on only, no call; > 10 s → `main: factory reset`, reboot to
`config: no stored credentials`; KEY2 still down at boot → ignored until released.

### 5. Sleep, discovery, http_request — passed

`display: screen off` 15 s after the last activity, `display: screen on` on a key.

Discovery and HTTP with a dev build that dials `fake_connector.py` on this Mac
(`-DCOMMA_CONNECT_URL_OVERRIDE=ws://192.168.3.118:8765/v1/connect`, `--allow-ops`), same LAN
(192.168.3.0/24):

- `--discover-probes --discover-timeout 5000 --loop 3`: **3/3 PASS**, 15 hosts each round
  (`truncated=True`: host list at its cap), three Gree units answered 399 B uncut each round.
- `--serve … --serve-port 8001 --text-limit --loop 3`: **3/3 rounds, 0 failures**: text at the
  64 KiB limit; chunks 10 KB, 800 KB JPEG, **1 MiB (128 chunks, ~4.2 s, ~240 KB/s)**, 256 KiB of all
  byte values, and 5 MB cut at the 4 MiB cap (`truncated=True`); every body byte-identical. Internal
  heap afterwards ~115–117 KB. (Port 8000 was held by an unrelated `http.server`.)

No restart of the board during the 12 minutes of LAN tests. The board was then flashed back to
the normal build (`f3fd733` + these changes), still provisioned. Discovery with probes (`fake_connector.py --discover-probes
--discover-timeout 5000 --loop 3` or the app): PASS, Gree answers uncut. `http_request` text and
1 MiB chunks to the Mac's HTTP server: PASS, three times each. Then the diagnostics build for
section 7's table (`idf.py -B firmware/build-m5stack-sticks3-diag -p PORT app-flash monitor` from
`firmware/`), and the normal build again.

Still open: the KEY2 > 10 s factory reset (section 5), the PMIC's voltage and power-source
registers, and the Connector's 45 s gap when it connects during a call start (shared code).

## Factory reset retest — 2026-10-13 (Claude)

KEY2 (right edge, lower half, with the screen up and KEY1 below it) held 15 s on `f557c53`:

```text
W main: factory reset
W config: NVS erased: ESP_OK
rst:0xc (RTC_SW_CPU_RST)          (the firmware's own restart, not a power cut)
```

The board came back unprovisioned. No PMIC action was involved. The earlier failed try released
KEY2 after 6 s (pairing mode), and the "power on" restarts match the M5PM1 power button's own
actions (datasheet: single click resets, double click powers off, long press enters download mode).
That button is a separate key, not KEY1 or KEY2. No firmware change was needed. The board was
reflashed with a clean `f557c53` build; the image used in the tests above was `f3fd733-dirty`.

# Third-party notices

This project is licensed under the [Apache License 2.0](LICENSE). It contains, adapts or links the
third-party material below. Each keeps its own license.

## Code adapted into this repository

| What | Where | Source | License |
| --- | --- | --- | --- |
| LAN discovery: ARP sweep, mDNS service list and record parsing, SSDP search, text sanitising | `firmware/main/lan_discover.c`, `firmware/main/lan_discover_core.c` | Meta muse-gadget-sdk, `esp32/main/net_discovery.c` | Apache-2.0, © Meta Platforms, Inc. and affiliates. The original header is kept; changes are described in each file |
| StickS3 audio, panel and power chip setup; the Waveshare AMOLED 1.43C audio, derived from the StickS3's | `firmware/boards/m5stack_sticks3/board_audio.c`, `panel.c`, `pmic.c`; `firmware/boards/waveshare_amoled_1_43c/board_audio.c` | Meta muse-gadget-sdk, `board_m5stack_sticks3.c`, `muse_audio.c` | Apache-2.0, © Meta Platforms, Inc. and affiliates. The original header is kept |
| Memory settings for boards without PSRAM, StickS3 PSRAM modes | `devices/sdkconfig.esp32-devkit`, `devices/sdkconfig.m5stack-sticks3` | Meta muse-gadget-sdk sdkconfig overlays | Apache-2.0, © Meta Platforms, Inc. and affiliates |
| GC9D01 LCD driver (vendored, version 2.0.0; `esp_lcd_gc9d01.c` modified for ESP-IDF 6, as its header says) | `firmware/components/esp_lcd_gc9d01/` | Espressif esp-iot-solution | Apache-2.0, © Espressif Systems (Shanghai) Co., Ltd. (`license.txt` in that folder) |
| CO5300 init command table and column offset | `firmware/boards/waveshare_amoled_1_32/panel.c`, `firmware/boards/waveshare_amoled_1_43c/panel.c` | Waveshare BSP `esp32_s3_touch_amoled_1_32`; for the 1.43C also the orientation command and offset from Waveshare's ESP32-S3-Touch-AMOLED-1.43C example | Apache-2.0, © Waveshare |

## Hardware facts

Pin maps, I2C addresses and register numbers are facts about the hardware, not copied code:

- Waveshare ESP32-S3-Touch-AMOLED-1.32 pins, from the Waveshare BSP and board documentation
  (`firmware/boards/waveshare_amoled_1_32/board_pins.h`).
- Waveshare ESP32-S3-Touch-AMOLED-1.43C pins, codec I2C addresses and the codec pairing, from
  Waveshare's schematic and its ESP-IDF examples in
  https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.43C
  (`firmware/boards/waveshare_amoled_1_43c/board_pins.h`).
- M5Stack StickS3 (K150) pins, from M5Stack's K150 schematic, and the M5PM1 register map, cross-checked
  with M5Stack's M5PM1 library (MIT, © M5Stack Technology); no code was copied from that library
  (`firmware/boards/m5stack_sticks3/`).
- LilyGO T-Circle-S3 V1.1 pins, from LilyGO's schematic and `pin_config.h` in
  https://github.com/Xinyuan-LilyGO/T-Circle-S3 (`firmware/boards/t_circle_s3/board.c` lists them).

## Font

| What | Where | License |
| --- | --- | --- |
| Inter 4.001 (variable), © 2016 The Inter Project Authors, https://github.com/rsms/inter | `firmware/reference/fonts/Inter-Variable.ttf` | SIL Open Font License 1.1, full text in `firmware/reference/fonts/OFL.txt` |
| Bitmap glyph tables generated from Inter by `firmware/tools/gen_ui_fonts.py` | `firmware/ui/*/ui_font.c` | SIL Open Font License 1.1 (derived from Inter) |

## ESP-IDF and managed components

The firmware is built with [ESP-IDF](https://github.com/espressif/esp-idf) **v6.0.2** (Apache-2.0;
ESP-IDF's own components carry their own licenses, listed in its repository). The component
manager downloads these components at build time; versions are locked in `firmware/dependencies.lock*`.
They are not stored in this repository, but they are linked into the firmware images.

| Component | Version | License | Boards (lock file) |
| --- | --- | --- | --- |
| espressif/cjson (cJSON, © 2009-2017 Dave Gamble and cJSON contributors) | 1.7.19~2 | MIT | all |
| espressif/esp_websocket_client | 1.8.0 | Apache-2.0 | all |
| espressif/esp-sr (speech models included in the model partition) | 2.5.5 | Espressif MIT License (use on Espressif chips) | T-Circle, Waveshare 1.32 and 1.43C, StickS3 |
| espressif/esp-dl | 3.3.13 | MIT | T-Circle, Waveshare 1.32 and 1.43C, StickS3 |
| espressif/dl_fft | 0.8.0 | MIT | T-Circle, Waveshare 1.32 and 1.43C, StickS3 |
| espressif/esp_new_jpeg | 1.0.2 | Espressif MIT License (use on Espressif chips) | T-Circle, Waveshare 1.32 and 1.43C, StickS3 |
| espressif/cmake_utilities | 0.5.3 | Apache-2.0 | T-Circle, Waveshare 1.32 and 1.43C |
| espressif/esp_lcd_co5300 | 2.1.0 | Apache-2.0 | Waveshare 1.32 and 1.43C |
| espressif/esp_codec_dev | 1.6.2 | Apache-2.0 | StickS3, Waveshare 1.43C |

The ESP32 DevKit build links only cJSON and esp_websocket_client from this list.

The Python tools use [bleak](https://github.com/hbldh/bleak) (MIT) and
[websockets](https://github.com/python-websockets/websockets) (BSD-3-Clause), installed from
`tools/requirements.txt`; they are not part of the firmware.

## Trademarks

"Comma" and the Comma logo are trademarks. The logo images in `firmware/ui/*/comma_logo.bin` and
the pictures in `docs/assets/` are included to build and document this firmware. The Apache
License does not grant permission to use the Comma name or logo (Apache-2.0, section 6).

Board names (LilyGO T-Circle-S3, M5Stack StickS3, Waveshare, Espressif ESP32) are trademarks of
their owners and are used only to identify compatible hardware.

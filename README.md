<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/assets/comma-hardware-banner-dark.png">
  <img src="docs/assets/comma-hardware-banner.png" alt="Comma firmware on four ESP32 boards">
</picture>

# Comma Hardware

Firmware that turns a small ESP32 board into a home gateway for [Comma](https://comma.surf). Plugged
in at home, it lets your Comma agent reach lights, air conditioners, cameras and local APIs on your
network while you are away. Boards with a mic and a speaker are also a voice terminal: tap to talk,
tap again to hang up.

The board knows no device protocols. It forwards HTTP, UDP and TCP from the backend to addresses on
its own network, and only after you allow device operations in the app.

## Boards

| Board | ID | Voice | Screen |
| --- | --- | :---: | --- |
| [LilyGO T-Circle-S3](https://lilygo.cc/products/t-circle-s3) | `t-circle-s3` | ✓ | 160 px round |
| [M5Stack StickS3](https://shop.m5stack.com/products/m5stack-sticks3-esp32s3-mini-iot-dev-kit) | `m5stack-sticks3` | ✓ | 135 × 240 |
| [Waveshare ESP32-S3-Touch-AMOLED-1.32](https://www.waveshare.com/esp32-s3-touch-amoled-1.32.htm) | `waveshare-amoled-1-32` | | 466 px round AMOLED |
| [Waveshare ESP32-S3-Touch-AMOLED-1.43C](https://www.waveshare.com/esp32-s3-touch-amoled-1.43c.htm) | `waveshare-amoled-1-43c` | ✓ | 466 px round AMOLED |
| [ESP32 DevKit](https://www.espressif.com/en/products/devkits/esp32-devkitc) (classic ESP32, 4 MB) | `esp32-devkit` | | Status LED |

Every board forwards HTTP, UDP and LAN discovery; all but the DevKit also open TCP tunnels. The
DevKit build is for the classic ESP32 only, not ESP32-S3 boards. Limits and pins are in
[devices/README.md](devices/README.md); test coverage is in [docs/validation.md](docs/validation.md).

## Get started

Install [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/index.html)
and activate it. Use exactly this version: the build patches a NimBLE pairing timeout and relies on
its lwIP internals. Then, from the repository root:

```sh
idf.py --version   # ESP-IDF v6.0.2
tools/board.sh t-circle-s3 build
tools/board.sh t-circle-s3 flash-monitor PORT
```

Use your board's ID from the table. `PORT` is the board's serial port: `/dev/cu.usbmodem*` or
`/dev/cu.usbserial-*` on macOS, `/dev/ttyACM*` or `/dev/ttyUSB*` on Linux, `COMx` on Windows. The
first build downloads components, so it needs internet. Build one board at a time: all boards share
`firmware/managed_components/`. If esptool cannot connect, see each board's download mode in
[devices/README.md](devices/README.md#flashing).

Then add the device in the Comma Android app: the board advertises as `Comma-XXXX` and joins a
**2.4 GHz** Wi-Fi network. Screen boards show a six-digit code; the DevKit pairs without one. A new
board is read-only until you allow device operations in its settings.

Runtime calls and LAN control need the Comma backend; this repository has the firmware and
development tools only.

## Keys

| | T-Circle-S3 | StickS3 | Waveshare 1.32 | Waveshare 1.43C | DevKit |
| --- | --- | --- | --- | --- | --- |
| Call / hang up | Tap screen | KEY1 | — | Tap screen or BOOT, short | — |
| Wake the screen | Tap (also calls) | KEY2, short | Tap or BOOT | Tap or BOOT (also call) | — |
| Pairing mode (hold 3–10 s) | BOOT | KEY2 | BOOT | BOOT | BOOT |
| Factory reset (hold > 10 s) | BOOT | KEY2 | BOOT | BOOT | BOOT |
| Settings on the screen | — | — | — | Hold the idle screen 1 s | — |

The 1.43C has no speaker on board: plug one into its MX1.25 speaker connector for calls.

The 1.43C's on-screen settings show the Wi-Fi network, its signal and whether Comma is online. They
scan for networks and switch to another one with a password typed on the screen, keeping the Comma
setup (no new pairing); a network that does not connect within 45 s is given up and the previous one
rejoined. They also turn Wi-Fi off until it is turned on again or the board restarts, and start
pairing mode, the only time Bluetooth runs. The Volume page sets the speaker level from 1 to 10 in
3 dB steps, with a beep at each step; it is stored and kept across restarts. Up to 8 the codec does
the work without clipping; 9 and 10 add +3 and +6 dB in front of the speaker limiter, louder but with
the loudest peaks compressed. BOOT, short, goes back a page. The same Wi-Fi switch and the volume
run from a computer over USB with `firmware/tools/serial_wifi.py` (see
[Development](docs/development.md)).

## Documentation

- [Security](docs/security.md) — pairing, stored credentials, what the gateway may reach
- [Connector protocol](docs/connector-protocol.md) and [Bluetooth setup](docs/ble-provisioning.md)
- [Development](docs/development.md) — host tests, tools, repository layout, adding a board

## License

[Apache License 2.0](LICENSE). Some files are adapted from Meta, Espressif and Waveshare code under
the same license; the Inter font is under the SIL Open Font License 1.1. See [NOTICE](NOTICE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). "Comma" and the Comma logo are trademarks and are
not licensed.

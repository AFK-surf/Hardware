# Hardware

Comma hardware firmware. One folder per device; each folder is a standalone ESP-IDF project.

| Folder | Board | What it runs |
| --- | --- | --- |
| `t-circle-s3/` | LilyGO T-Circle-S3 (ESP32-S3, 16 MB flash, 8 MB PSRAM) | `comma.voice.v1` voice calls, BLE setup, and a LAN gateway (HTTP, UDP, discovery, TCP tunnels) |

### Planned boards

Every ESP32 board has Wi-Fi and BLE, so each one can serve as a home Connector (LAN gateway). Voice
is enabled only on boards with a microphone and speaker. Camera snapshots (RTSP through
`lan_tcp_dial`) need enough RAM to receive the server's 32 KiB tunnel frames
([protocol §4, `lan_tcp_dial`](docs/connector-protocol.md)), and that is the main difference between boards.

| Board | Chip / memory | Connector (HTTP, UDP, discovery) | Snapshots (TCP tunnel) | Voice | Pairing without a code on screen |
| --- | --- | --- | --- | --- | --- |
| LilyGO T-Circle-S3 | ESP32-S3, 16 MB flash, 8 MB PSRAM | Yes (running) | Yes, 2 tunnels (board-tested) | Yes | Not needed: passkey on the screen |
| ESP32-S3 AUDIO-Board | ESP32-S3 with PSRAM | Yes | Yes, expected same as T-Circle | Yes | Speaker reads the 6-digit passkey aloud |
| M5Stack Stick (model to confirm) | ESP32-S3 or ESP32, depends on model | Yes | S3 model: yes. ESP32 model: as DevKit | If it has a mic and speaker | Not needed: passkey on the screen |
| ESP32 DevKit (WROOM-32) | ESP32-D0WD rev v1.0, 4 MB flash, **no PSRAM** | Yes, with smaller limits (≈16 KiB per text response; chunk mode for larger) | **Unverified**: at most 1 tunnel, possibly none; needs a board test | No | Press BOOT to confirm (dev builds: passkey in the serial log) |

Porting notes:

- Gateway code currently allocates its buffers and some task stacks with `MALLOC_CAP_SPIRAM`. Boards
  without PSRAM need those to fall back to internal RAM and need smaller per-board limits.
- On boards without PSRAM, BLE should run only during setup, so its memory returns to the network.
- If a board cannot hold a tunnel, it must leave `lan_tcp_dial` out of `capabilities.lan_gateway.methods`;
  the backend then does not route snapshots to it.
- The app needs a pairing-method hint in the BLE advertisement (screen / spoken / button) to show the
  right instructions.

## Set up the toolchain

Install **ESP-IDF v6.0.2** with the ESP32-S3 tools, following the
[official installation guide](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/index.html).
The guide covers macOS, Linux, and Windows. Activate that installation in your terminal
(or open its ESP-IDF terminal on Windows), then check:

```sh
idf.py --version
```

It should report `ESP-IDF v6.0.2`. Use this exact version: the project patches the NimBLE pairing
timeout during CMake configuration and relies on this version's lwIP internals. Do not edit the
installed SDK. A newer SDK needs separate validation.

## Clone and build

You need read access to this private repository and GitHub authentication configured for Git.

```sh
git clone https://github.com/AFK-surf/Hardware.git
cd Hardware/t-circle-s3
idf.py build
```

No board, Wi-Fi password, backend credential, or `secrets.h` is needed to compile. The target and
board settings are checked in as `sdkconfig` / `sdkconfig.defaults`; component versions are pinned
in `dependencies.lock`. The first build downloads managed components, so it needs internet access.
Keep the lockfile; do not run `idf.py update-dependencies` as part of ordinary setup.

Display assets, fonts, and the logo binary are already checked in. Their generator scripts are
optional and are not part of the firmware build. Outputs are under `t-circle-s3/build/`.

## Flash and provision a board

Connect your T-Circle with a USB data cable. Replace `PORT` with its actual serial port, for example
`/dev/cu.usbmodem1101` on macOS, `/dev/ttyACM0` on Linux, or `COM5` on Windows. Run from `t-circle-s3/`:

```sh
idf.py -p PORT flash monitor
```

The first flash must include the bootloader, partition table, application, and ESP-SR model
partition; use `flash`, not just `app-flash`. Exit the serial monitor with Ctrl+]. Do not erase
flash when updating a configured board unless you intend to remove its saved credentials.

An unconfigured board advertises over BLE. Use the Comma app to set up **2.4 GHz Wi-Fi** and issue
the board its own Connector token and voice key. See the [BLE setup contract](docs/ble-provisioning.md).
Runtime calls and LAN control require a compatible Comma/Salix backend; this repository contains
the firmware and development tools, not the backend or Android app.

- Tap the screen to call; tap again to hang up.
- Hold BOOT for at least 3 seconds and release before 10 seconds to enter pairing mode.
- Hold BOOT for 10 seconds to factory reset and erase the saved configuration.
- Enable device operations in the app before using the gateway; it starts read-only.

For local development only, copy `main/secrets.h.example` to `main/secrets.h`, fill in your own
credentials, and set `COMMA_DEV_SECRETS` to `1`. That overrides BLE/NVS settings and embeds those
credentials in the firmware. `secrets.h` and build directories are ignored by Git. Ordinary builds
and shared firmware should use BLE provisioning instead.

## Development tools and tests

The following commands are for macOS/Linux shells, from the repository root. Keep tool dependencies
in a separate virtual environment from ESP-IDF:

```sh
python3 -m venv tools/.venv
. tools/.venv/bin/activate
python -m pip install -r tools/requirements.txt
python -m unittest discover -s t-circle-s3/tools -p 'test_*.py'
```

The Python tests use simulated BLE data and local HTTP/WebSocket servers; they do not need a board
or backend credentials. The BLE client is `t-circle-s3/tools/ble_provision.py`, and
`t-circle-s3/tools/fake_connector.py` provides a local test Connector.

The C host tests currently target **macOS** (the HTTP test uses CommonCrypto). Run one firmware build
first to download the managed cJSON component, activate ESP-IDF so `IDF_PATH` is set, and run:

```sh
sh t-circle-s3/tools/lan_discover_test/run.sh
sh t-circle-s3/tools/lan_http_test/run.sh
sh t-circle-s3/tools/lan_udp_test/run.sh
sh t-circle-s3/tools/lan_tunnel_test/run.sh
```

These compile the real LAN implementations with ESP stubs and AddressSanitizer/UndefinedBehaviorSanitizer.
They need a C compiler and permission to open local sockets. They do not replace board tests for
Wi-Fi, BLE, audio, or the lwIP receive-window adjustment.

## Project map and current limits

- `t-circle-s3/main/`: firmware, board drivers, voice, setup, and LAN gateway.
- `t-circle-s3/components/`: checked-in LCD driver.
- `t-circle-s3/tools/` and `tools/`: provisioning clients, protocol simulators, and host tests.
- [Connector protocol](docs/connector-protocol.md): backend integration contract.
- [Project plan](docs/connector-plan.md) and [reports](docs/reports/): design decisions and historical
  board results. Paths, ports, device IDs, and commit IDs in older reports describe the original
  test environment; use your own device and checkout.
- [Code review, 2026-10-09](docs/reports/2026-10-09-code-review.md): newly identified issues awaiting fixes.

Chunked HTTP transfers and TCP tunnels are refused during voice calls. Two simultaneous tunnels
are supported, with a 5-minute idle limit. The [G3 report](docs/reports/G3-report.md) describes a known
stall under bidirectional backpressure. OTA is not implemented. Secure Boot, flash encryption, and
NVS encryption are not enabled in the current development configuration.

# Hardware

Comma hardware firmware: one ESP-IDF project, shared gateway code, and per-board overlays.

| Folder | Purpose |
| --- | --- |
| `firmware/main/` | Shared Connector, LAN methods, Wi-Fi, BLE provisioning and BOOT controls |
| `firmware/boards/t_circle_s3/` | T-Circle display, touch, audio and voice |
| `firmware/boards/esp32_devkit/` | DevKit GPIO0 BOOT and GPIO2 status LED |
| `devices/` | Board configurations and [porting instructions](devices/README.md) |
| `tools/board.sh` | Build and flash one board with its own generated config and build directory |

### Boards

A supported ESP32 board with Wi-Fi and BLE can serve as a home Connector. Voice requires a microphone
and speaker. TCP snapshots additionally need enough RAM for the server's 32 KiB tunnel frames
([protocol §4](docs/connector-protocol.md)). Build support alone does not establish that these workloads fit.

| Board | Chip / memory | Connector | Snapshots (TCP tunnel) | Voice | Pairing |
| --- | --- | --- | --- | --- | --- |
| LilyGO T-Circle-S3 | ESP32-S3, 16 MB flash, 8 MB PSRAM | Running: HTTP, UDP, discovery | 2, board-tested in G3 | Yes | On-screen passkey, MITM |
| ESP32-S3 AUDIO-Board | ESP32-S3 with PSRAM | Planned, not added | Expected; unverified | Planned | Planned spoken passkey |
| M5Stack Stick (model to confirm) | Depends on model | Planned, not added | Unverified | Depends on hardware | Depends on hardware |
| ESP32 DevKit | ESP32-D0WD rev v1.0, 4 MB flash, no PSRAM | Board-tested: HTTP (8 KiB text, 1 MiB in chunks), UDP, discovery (12 KiB result cap), 3/3 each | **Unsupported: `lan_tcp_dial` disabled** | No | Secure Connections Just Works, no MITM |

DevKit uses an 8 KiB text cap, 4 KiB HTTP chunks, 16 KiB incoming JSON frames, a 16 KiB discovery
arena and a 12 KiB discovery JSON cap. On the board with local WSS, the Connector idles at about
130 KB free internal RAM (57 KB largest block) and never dropped below 57 KB during three rounds of
discovery, 8 KiB text and 1 MiB chunk transfers, with no crash or reconnect. Physical BOOT pairing,
factory reset and unplug/replug were also checked.

DevKit does not support `lan_tcp_dial`: the server's 32 KiB raw frames need about 43.7 KB of base64
buffer plus JSON, which does not reliably fit the measured 36–49 KB largest blocks after TLS.
To support tunnels, the backend must honor the device's `max_frame`; the smaller-frame path must
then pass hardware memory tests before tunnels are enabled again.
See [H1's measurements and remaining checks](docs/reports/H1-report.md).

The BLE UUIDs and credential fields are unchanged; even the no-voice board still accepts the existing
`server` and `voice_key` fields for app compatibility. BOOT grants a provisioned board a temporary
pairing window; Just Works encryption cannot stop an active man-in-the-middle.

## Set up the toolchain

Install **ESP-IDF v6.0.2** with the ESP32 and ESP32-S3 tools, following the
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
cd Hardware
tools/board.sh t-circle-s3 build
tools/board.sh esp32-devkit build
```

No board, Wi-Fi password, backend credential, or `secrets.h` is needed to compile. The target and
board settings come from `firmware/sdkconfig.defaults` plus `devices/sdkconfig.<board>`. Each build
generates `firmware/build-<board>/sdkconfig`; no generated sdkconfig is committed. Dependencies are
locked in `firmware/dependencies.lock` (T-Circle) and `firmware/dependencies.lock.esp32` (DevKit).
The first build needs internet access. DevKit excludes ESP-SR and the LCD/audio stack. Build boards
sequentially: the component manager uses one shared `firmware/managed_components/` directory.
Do not run `idf.py update-dependencies` as part of ordinary setup.

Display assets, fonts, and the logo binary are already checked in. Their generator scripts are
optional and are not part of the firmware build. Outputs are under `firmware/build-<board>/`.

## Flash and provision a board

Connect the intended board with a USB data cable. Replace `PORT` with its actual serial port, for example
`/dev/cu.usbmodem1101` on macOS, `/dev/ttyACM0` on Linux, or `COM5` on Windows. Run from the repository root:

```sh
tools/board.sh t-circle-s3 flash-monitor PORT
# DevKit on this Mac (check the port before using):
tools/board.sh esp32-devkit flash-monitor /dev/cu.usbserial-0001
```

The first flash must include the bootloader, partition table and application (plus the ESP-SR model
partition on T-Circle); use `flash`, not just `app-flash`. Exit the serial monitor with Ctrl+]. Do not erase
flash when updating a configured board unless you intend to remove its saved credentials.

An unconfigured board advertises over BLE. Use the Comma app to set up **2.4 GHz Wi-Fi** and issue
the board its own Connector token and voice key. See the [BLE setup contract](docs/ble-provisioning.md).
Runtime calls and LAN control require a compatible Comma/Salix backend; this repository contains
the firmware and development tools, not the backend or Android app.

- T-Circle: tap the screen to call; tap again to hang up.
- DevKit: LED off normally, slow blink while advertising, fast blink while BLE connected, solid 3 s after saved.
- Hold BOOT for at least 3 seconds and release before 10 seconds to enter pairing mode.
- Hold BOOT for 10 seconds to factory reset and erase the saved configuration.
- Enable device operations in the app before using the gateway; it starts read-only.

For local development only, copy `firmware/main/secrets.h.example` to `firmware/main/secrets.h`, fill in your own
credentials, and set `COMMA_DEV_SECRETS` to `1`. That overrides BLE/NVS settings and embeds those
credentials in the firmware. `secrets.h` and build directories are ignored by Git. Ordinary builds
and shared firmware should use BLE provisioning instead.

H2 adds caller-supplied UDP discovery probes on both boards; build/host checks pass and hardware
acceptance is pending. See the [H2 report](docs/reports/H2-report.md) for limits and the test plan.

## Development tools and tests

The following commands are for macOS/Linux shells, from the repository root. Keep tool dependencies
in a separate virtual environment from ESP-IDF:

```sh
python3 -m venv tools/.venv
. tools/.venv/bin/activate
python -m pip install -r tools/requirements.txt
python -m unittest discover -s firmware/tools -p 'test_*.py'
```

The Python tests use simulated BLE data and local HTTP/WebSocket servers; they do not need a board
or backend credentials. The BLE client is `firmware/tools/ble_provision.py`, and
`firmware/tools/fake_connector.py` provides a local test Connector.

The C host tests currently target **macOS** (the HTTP test uses CommonCrypto). Run one firmware build
first to download the managed cJSON component, activate ESP-IDF so `IDF_PATH` is set, and run:

```sh
sh firmware/tools/lan_discover_test/run.sh
sh firmware/tools/lan_http_test/run.sh
sh firmware/tools/lan_udp_test/run.sh
sh firmware/tools/lan_tunnel_test/run.sh
sh firmware/tools/connector_json_test/run.sh
# Smaller DevKit limits (from its overlay):
sh firmware/tools/lan_http_test/run.sh --devkit
sh firmware/tools/lan_udp_test/run.sh --devkit
sh firmware/tools/lan_discover_test/run.sh --devkit
```

These compile the real LAN implementations with ESP stubs and AddressSanitizer/UndefinedBehaviorSanitizer.
They need a C compiler and permission to open local sockets. They do not replace board tests for
Wi-Fi, BLE, audio, or the lwIP receive-window adjustment.

## Project map and current limits

- `firmware/main/`: shared gateway, setup, controls, Kconfig and memory policy.
- `firmware/boards/`: board drivers and feedback; T-Circle voice stays here.
- `devices/`: per-board sdkconfig overlays; [add-a-board guide](devices/AGENTS.md).
- `firmware/cmake/idf_patches.cmake`: shared NimBLE timeout patch and ARP table configuration.
- `firmware/components/`: checked-in LCD driver.
- `firmware/tools/` and `tools/`: provisioning clients, protocol simulators, and host tests.
- [Connector protocol](docs/connector-protocol.md): backend integration contract.
- [Project plan](docs/connector-plan.md) and [reports](docs/reports/): design decisions and historical
  board results. Paths, ports, device IDs, and commit IDs in older reports describe the original
  test environment; use your own device and checkout.
- [Code review, 2026-10-09](docs/reports/2026-10-09-code-review.md): newly identified issues awaiting fixes.

Chunked HTTP transfers and TCP tunnels are refused during voice calls. T-Circle supports two simultaneous tunnels, with a 5-minute idle limit. The [G3 report](docs/reports/G3-report.md) describes a known
stall under bidirectional backpressure. OTA is not implemented. Secure Boot, flash encryption, and
NVS encryption are not enabled in the current development configuration.

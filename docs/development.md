# Development

## Host tests

Python tools and tests use their own virtual environment, separate from ESP-IDF:

```sh
python3 -m venv tools/.venv
. tools/.venv/bin/activate
python -m pip install -r tools/requirements.txt
python -m unittest discover -s firmware/tools -p 'test_*.py'
```

The C host tests compile the real LAN, JSON and audio code against small ESP-IDF stubs with
AddressSanitizer and UndefinedBehaviorSanitizer. They never use the machine's network or DNS. They
currently target **macOS** (the HTTP test uses CommonCrypto). Build one board first (for the managed
cJSON component), activate ESP-IDF, then:

```sh
sh firmware/tools/codec_test/run.sh
sh firmware/tools/lan_discover_test/run.sh
sh firmware/tools/lan_http_test/run.sh
sh firmware/tools/lan_udp_test/run.sh
sh firmware/tools/lan_tunnel_test/run.sh
sh firmware/tools/connector_json_test/run.sh
sh firmware/tools/resample_test/run.sh
sh firmware/tools/mic_path_test/run.sh
sh firmware/tools/device_config_test/run.sh
sh firmware/tools/call_ui_test/run.sh
sh firmware/tools/wifi_switch_test/run.sh
sh firmware/tools/wifi_scan_test/run.sh
sh firmware/tools/speaker_volume_test/run.sh
# The DevKit's smaller limits:
sh firmware/tools/lan_http_test/run.sh --devkit
sh firmware/tools/lan_udp_test/run.sh --devkit
sh firmware/tools/lan_discover_test/run.sh --devkit
```

They do not replace board tests for Wi-Fi, Bluetooth or audio.

## Tools

- `firmware/tools/ble_provision.py`: set up a board over Bluetooth from a computer, as the app does
  (`scan`, `info`, `wifi-scan`, `provision`). Its `--server` and `--connect-url` default to the
  production backend; pass both to use another one. Secrets come from environment variables.
- `firmware/tools/serial_wifi.py`: switch a Waveshare 1.43C to another Wi-Fi network over USB, keeping
  its Comma setup (`join NETWORK`, password on stdin; `status`). On macOS the password can come straight
  from the keychain: `security find-generic-password -D "AirPort network password" -a NETWORK -w |
  python firmware/tools/serial_wifi.py --port /dev/cu.usbmodem101 join NETWORK`. `volume` shows the
  speaker level and `volume 1`-`volume 10` sets it, also during a call. Close any serial monitor first.
- `firmware/tools/fake_connector.py`: a local Connector server for testing a board without the
  backend. Build with `-DCOMMA_CONNECT_URL_OVERRIDE=ws://<your-computer>:8765/v1/connect` in a
  separate build directory (see [devices/README.md](../devices/README.md)).
- `firmware/tools/connector_regression.py --report /tmp/connector-regression.json`: live-board
  HTTP/UDP regression checks using that same override image. Tests interim responses, FIN versus
  reset, queued deadlines, permission revocation and disconnect cancellation. It temporarily changes
  operation permission and restores the original value after completion; wait for `DONE`, then
  restore the normal firmware. On interruption, check the permission in the app before resuming use.
- `tools/lan_mcp_server.py`: a tiny MCP server to stand in for a device on your network.
- For a development board, `firmware/main/secrets.h.example` shows how to compile credentials in
  instead of using Bluetooth setup. `secrets.h` is ignored by Git; never ship such an image.

## Layout

| Path | Contents |
| --- | --- |
| `firmware/main/` | Shared gateway: Connector, LAN methods, Wi-Fi, Bluetooth setup, keys, Kconfig |
| `firmware/voice/` | Shared voice call: voice session, playback, noise suppression and VAD |
| `firmware/ui/` | Shared screen cards, fonts and logo for each screen size, the panel transfer, and the on-screen settings (`settings.c`, `ui_view.c`) |
| `firmware/boards/<board>/` | Pins and drivers for one board |
| `firmware/boards/common/` | Drivers for parts that several boards use (CST816-family touch) |
| `firmware/components/` | Vendored GC9D01 LCD driver (Espressif, Apache-2.0) |
| `devices/` | Per-board configuration overlays and board notes |
| `tools/board.sh` | Build, flash and monitor one board with its own config and build directory |

## Adding a board

1. Read [docs/connector-protocol.md](connector-protocol.md),
   [docs/ble-provisioning.md](ble-provisioning.md) and [devices/README.md](../devices/README.md).
2. Put pins and drivers in `firmware/boards/<board>/`. A board with voice or a screen reuses
   `firmware/voice/` and `firmware/ui/` and implements only `voice/board_audio.h` (mic and speaker
   at the pipeline's rates), `voice/board_input.h` (tap and keys) and `ui/board_display.h` (panel,
   framebuffer, sleep). Reuse what is already there: `ui/panel_transfer.h` sends a framebuffer with
   bounded waits, and `boards/common/` drives parts other boards use. Shared code never includes a
   board's own headers.
3. Add a `COMMA_BOARD` choice and board ID in `firmware/main/Kconfig.projbuild`, an overlay
   `devices/sdkconfig.<board>`, the board/target mapping in `tools/board.sh`, and source selection
   in `firmware/main/CMakeLists.txt`. Choose voice, display, pairing method, GPIOs and limits from
   the actual hardware.
4. Choose a partition table, flash size and mode, chip revision and memory settings. Put movable
   buffers and stacks under `COMMA_BUFFER_CAPS`. On boards without PSRAM, measure free internal heap
   and the largest block with `CONFIG_COMMA_HEAP_DIAGNOSTICS` before enabling a feature.
5. A board without a display must use Just Works pairing (encryption-only GATT, Secure Connections
   on, legacy off, NimBLE `SM_SC_ONLY=0`) and document that it cannot stop an active MITM.
6. Keep the Bluetooth UUIDs, JSON fields and Connector protocol unchanged. Keep the existing boards'
   generated configs and behaviour unchanged: build every board, run the host tests, and do not
   update the dependency lock files as a side effect.
7. Test on the hardware: text at the configured cap, 1 MiB chunked transfers, discovery and tunnels,
   three times each. Enable features from measurements, not from a successful build. The backend
   sends 32 KiB tunnel frames, so tunnels need that much headroom.

[devices/AGENTS.md](../devices/AGENTS.md) has the same checklist in more detail.

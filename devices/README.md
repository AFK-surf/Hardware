# Board configurations

Run from the repository root after activating ESP-IDF 6.0.2:

```sh
tools/board.sh t-circle-s3 build
tools/board.sh esp32-devkit build
tools/board.sh esp32-devkit flash-monitor /dev/cu.usbserial-0001
```

| Overlay | Target | Flash / PSRAM | Features | Status |
| --- | --- | --- | --- | --- |
| `sdkconfig.t-circle-s3` | esp32s3 | 16 MB / 8 MB | Display, voice, passkey, HTTP/UDP/discovery, 2 tunnels | Existing board; H1 config/build/host regression checks |
| `sdkconfig.esp32-devkit` | esp32 | 4 MB / none | GPIO2 LED, Just Works, HTTP/UDP, experimental discovery + 1 tunnel | Rev v1.0 flashed; network and BLE acceptance pending, see H1 report |

The shared defaults load first, then the board overlay. Generated config and objects live in
`firmware/build-<board>/`. ESP-IDF keeps existing config choices when defaults change: delete that
board's generated `sdkconfig` before rebuilding to apply changed defaults, or use menuconfig on that
build directory. Never edit or commit a source-root sdkconfig.

`build`, `flash`, `monitor` and `flash-monitor` are the usual actions. `size`, `size-components` and
`reconfigure` are also supported. Flash and monitor require an explicit port. Additional IDF options
follow the port, or the action for non-port commands:

```sh
tools/board.sh esp32-devkit build -DCOMMA_CONNECT_URL_OVERRIDE=ws://192.168.1.2:8765/v1/connect
```

CMake caches development overrides. To return to the normal provisioned URL and built-in trust bundle:

```sh
tools/board.sh esp32-devkit build -DCOMMA_CONNECT_URL_OVERRIDE= -DCOMMA_CONNECT_TEST_CA=
```

For local WSS measurements, `-DCOMMA_CONNECT_TEST_CA=/absolute/path/test-ca.pem` embeds a test CA only
when the URL override is set; certificate verification stays enabled. Do not distribute test images.
The fake Connector supports `--tls-cert`, `--tls-key`, `--text-limit`, `--discover`, `--tunnel`, and
`--loop 3`. It skips methods the board does not advertise and keeps the server's 32 KiB tunnel frames.

Build targets sequentially: managed components are shared. The DevKit uses its own lockfile and the
manifest's esp32s3 target rule excludes ESP-SR on esp32. T-Circle's package versions and hashes are
unchanged; its manifest hash reflects the conditional declaration. The component manager may remove
and restore ESP-SR when switching boards, but DevKit never downloads or links it.

For new hardware follow [AGENTS.md](AGENTS.md). Do not infer board capacity from a successful build.

HTTP, UDP and discovery host test scripts also accept `--devkit` to exercise the numeric limits
from this overlay. Tunnel tests retain the default two-tunnel profile; the DevKit one-tunnel
acceptance test requires the actual board and the fake Connector.

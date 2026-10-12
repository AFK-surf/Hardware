# Board configurations

Run from the repository root after activating ESP-IDF 6.0.2:

```sh
tools/board.sh t-circle-s3 build
tools/board.sh esp32-devkit build
tools/board.sh esp32-devkit flash-monitor /dev/cu.usbserial-0001
```

| Overlay | Target | Flash / PSRAM | Features | Status |
| --- | --- | --- | --- | --- |
| `sdkconfig.t-circle-s3` | esp32s3 | 16 MB / 8 MB | Display, voice, passkey, HTTP/UDP/discovery, 2 tunnels | Supported; H1 smoke test on the board: boot, Connector online, voice call |
| `sdkconfig.esp32-devkit` | esp32 | 4 MB / none | GPIO2 LED, Just Works, HTTP/UDP, discovery with 12 KiB result cap; no TCP tunnels | Supported; local WSS: discovery, 8 KiB text and 1 MiB chunks passed 3/3; BOOT and replug checked |

DevKit does not support `lan_tcp_dial`. The backend currently sends 32 KiB raw frames regardless of
the device's `max_frame`; their base64/JSON buffers do not reliably fit the 36–49 KB largest blocks
observed after TLS. The backend must honor `max_frame` before DevKit tunnel support can be retested
and enabled. Disabled tunnels are omitted from Connector metadata.

The measured DevKit profile uses static TLS buffers with renegotiation off, 16 KiB TCP receive/send
buffers, an 8 KiB text cap, 4 KiB HTTP chunks and 16 KiB incoming JSON frames. Discovery keeps its
16 KiB string arena but caps result JSON and buffer growth at 12 KiB (plus one NUL byte); excess
hosts set `truncated=true`. Heap diagnostics are off; turn `CONFIG_COMMA_HEAP_DIAGNOSTICS` on in a
test build when measuring a change. See [H1](../docs/reports/H1-report.md) for the hardware
measurements and build/host validation. H2 adds up to four UDP discovery probes with 256-byte
payloads, 1024-byte answers and a shared 4096-byte answer budget (T-Circle: 8 / 512 / 1024 / 16384).
Both builds and host tests pass; probe heap recovery, real device replies and voice coexistence
await the [H2 hardware plan](../docs/reports/H2-report.md).

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
`--loop 3`. H2 adds `--discover-probes [JSON_FILE]` (Gree/miIO by default) and
`--discover-timeout 5000`; probe limits come from metadata. Binary answers are saved as base64 JSON
under `--out`, while logs show only ids, byte counts and cut flags. It skips methods the board does not advertise and keeps the server's 32 KiB tunnel frames.

Build targets sequentially: managed components are shared. The DevKit uses its own lockfile and the
manifest's esp32s3 target rule excludes ESP-SR on esp32. T-Circle's package versions and hashes are
unchanged; its manifest hash reflects the conditional declaration. The component manager may remove
and restore ESP-SR when switching boards, but DevKit never downloads or links it.

For new hardware follow [AGENTS.md](AGENTS.md). Do not infer board capacity from a successful build.

HTTP, UDP and discovery host test scripts also accept `--devkit` to exercise the numeric limits
from this overlay, including the discovery JSON budget. Tunnel tests retain the default two-tunnel
profile for T-Circle; DevKit advertises no tunnel method.

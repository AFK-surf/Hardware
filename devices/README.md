# Board configurations

Each board is one ESP-IDF configuration overlay here, applied on top of `firmware/sdkconfig.defaults`.
Run from the repository root after activating ESP-IDF 6.0.2:

```sh
tools/board.sh t-circle-s3 build
tools/board.sh esp32-devkit build
tools/board.sh waveshare-amoled-1-32 build
tools/board.sh m5stack-sticks3 build
tools/board.sh waveshare-amoled-1-43c build
tools/board.sh esp32-devkit flash-monitor PORT
```

| Overlay | Target | Flash / PSRAM | Features | Tested on hardware |
| --- | --- | --- | --- | --- |
| `sdkconfig.t-circle-s3` | esp32s3 | 16 MB / 8 MB | GC9D01 160 px round screen, CST816D touch, PDM mic, MAX98357A speaker; voice, passkey, HTTP/UDP/discovery, 2 tunnels | Setup, Connector, calls, discovery, HTTP text and chunks, UDP, tunnels; queued deadline/revocation/disconnect regressions |
| `sdkconfig.m5stack-sticks3` | esp32s3 | 8 MB / 8 MB | ST7789 135x240 screen, ES8311 codec on stereo I2S at 24 kHz (mic resampled to 16 kHz), M5PM1 power chip, KEY1 calls, KEY2 = BOOT; T-Circle limits | Passkey setup, Connector, KEY1 calls, screen sleep, KEY2 pairing and factory reset, discovery, HTTP/UDP regressions and tunnels; -3 dB speaker level accepted in listening test |
| `sdkconfig.waveshare-amoled-1-32` | esp32s3 | 8 MB / 8 MB | CO5300 466x466 AMOLED, CST820 touch; gateway without voice (the board has a mic but no speaker); own partition table without a model partition; T-Circle limits | Screen, touch, passkey setup, Connector, BOOT press and holds; HTTP/UDP regressions, discovery and tunnels; display recovered after correcting an earlier wrong-board flash |
| `sdkconfig.waveshare-amoled-1-43c` | esp32s3 | 8 MB / 8 MB | CO5300 466x466 AMOLED (mounted turned 180 degrees), CST820 touch, ES8311 speaker codec and ES7210 mics on stereo I2S at 24 kHz (mic resampled to 16 kHz), NS4150B amplifier for an external speaker; tap or short BOOT press calls; on-screen settings (hold the idle screen 1 s: Wi-Fi switch, Wi-Fi off and on, pairing mode, speaker volume); StickS3 partition layout; T-Circle limits | Boot, screen, touch and audio codec setup from the serial log; see the [validation notes](../docs/validation.md) for what is still open |
| `sdkconfig.esp32-devkit` | esp32 | 4 MB / none | GPIO2 status LED, Just Works pairing, HTTP/UDP, discovery with a 12 KiB result cap; no TCP tunnels | Setup, discovery, 8 KiB text and 1 MiB chunks three times each over local WSS; BOOT holds; replug; current HTTP/UDP regressions over local WS and staging WSS reconnect |

The [validation notes](../docs/validation.md) summarize the current results and outstanding tests.
Physical offline dialing and credential replacement across workspaces remain unverified on hardware;
host regressions cover both policies.

## Flashing

The first flash must write everything (bootloader, partition table, app and, on voice boards, the
speech model partition): use `flash`, not `app-flash`. Exit the monitor with Ctrl+]. Flashing does
not erase saved settings. Download mode, per board:

- **T-Circle-S3 and Waveshare AMOLED** (native USB): esptool enters download mode by itself. If it
  cannot connect, hold BOOT while plugging in USB, then flash. If the Waveshare stays silent with a
  blank screen after flashing on macOS, unplug and replug USB without touching BOOT.
- **Waveshare AMOLED 1.43C** (native USB): esptool enters download mode by itself. The board has
  BOOT and PWR keys but no RESET: if esptool cannot connect, hold BOOT and press PWR, release both,
  then flash.
- **StickS3** (native USB): download mode is automatic, no key needed. Opening the serial port resets
  the board. A long press on the separate power button also enters download mode.
- **ESP32 DevKit** (USB-UART bridge, often a CH340 or CP210x): install the bridge's driver if no port
  appears. If esptool stays at `Connecting...`, hold BOOT until it starts writing.

## Keys

Pairing mode lets the app reconfigure a board that is already set up (new Wi-Fi, new credentials);
it ends after 5 minutes without a connection. Factory reset erases Wi-Fi, credentials and Bluetooth
keys, and the board restarts unconfigured. A key still held when the board starts is ignored until
released. The DevKit's LED is off normally, blinks slowly while advertising, quickly while the app is
connected, and stays on for about 3 s after setup is saved.

On voice boards, tapping while Wi-Fi is unavailable waits up to 20 seconds for it to return.
Tap again to cancel. Pairing/reset holds remain responsive and cancel the pending call.

## ESP32 DevKit limits

The DevKit has no PSRAM, so its overlay trades capacity for memory:

- static TLS buffers with renegotiation off (dynamic buffer release raced the WebSocket writer),
  16 KiB TCP receive/send windows, 16 KiB inbound JSON frames, a 2 KiB WebSocket buffer;
- `http_request`: 8 KiB text (16 KiB failed in tests, 8 KiB passed), 4 KiB chunks, 4 MiB total;
- `lan_discover`: a 16 KiB string arena, at most 6 services and 6 SSDP entries per host, a 12 KiB
  result (excess hosts set `truncated: true`), and up to 4 UDP probes of 256 bytes with 1024-byte
  answers and a 4 KiB answer budget;
- setup Bluetooth ends 3 s after the Connector is up (`CONFIG_COMMA_FREE_BLE_FOR_DISCOVERY`), and
  discovery waits for it, because a discovery result does not fit while Bluetooth holds its memory;
- **no `lan_tcp_dial`**: the backend sends 32 KiB raw tunnel frames regardless of the device's
  `max_frame`, and their base64/JSON buffers do not fit the 36–49 KB largest free blocks left after
  TLS. Tunnels can be reconsidered once the backend honors `max_frame`. Disabled methods are left
  out of the Connector metadata.

With Bluetooth off and the Connector on local WSS, the DevKit idled at about 130 KB free internal
RAM (57 KB largest block) and stayed above that during three rounds of discovery, 8 KiB text and
1 MiB chunk transfers. Discovery with the 6/6 caps while Bluetooth is still up has not been retested.
Heap diagnostics are off; turn on `CONFIG_COMMA_HEAP_DIAGNOSTICS` in a test build to measure a change.

## Build details

The shared defaults load first, then the board overlay. Generated config and objects live in
`firmware/build-<board>/`. ESP-IDF keeps existing config choices when defaults change: delete that
board's generated `sdkconfig` before rebuilding to apply changed defaults, or use menuconfig on that
build directory. Never edit or commit a source-root `sdkconfig`.

Dependencies are locked per board: `firmware/dependencies.lock` (T-Circle),
`dependencies.lock.esp32` (DevKit), `dependencies.lock.waveshare-amoled-1-32` (adds the CO5300
driver), `dependencies.lock.m5stack-sticks3` (adds esp_codec_dev) and
`dependencies.lock.waveshare-amoled-1-43c` (adds both). `tools/board.sh` tells CMake the
board before Kconfig runs, which selects the lock file and components. Build boards one at a time:
`firmware/managed_components/` is shared, and the component manager may remove and restore ESP-SR
when you switch boards. The DevKit never downloads or links ESP-SR. Do not run
`idf.py update-dependencies` as part of ordinary work.

`build`, `flash`, `monitor` and `flash-monitor` are the usual actions; `size`, `size-components` and
`reconfigure` also work. Flash and monitor need an explicit port. Additional IDF options follow the
port, or the action for commands without a port.

## Testing against a local Connector

`firmware/tools/fake_connector.py` is a local stand-in for the backend. Point a build at it:

```sh
tools/board.sh esp32-devkit build -DCOMMA_CONNECT_URL_OVERRIDE=ws://192.168.1.2:8765/v1/connect
```

The board dials that URL with a dummy token; saved credentials are neither changed nor sent. CMake
caches the override; to return to the provisioned URL and the built-in certificate bundle:

```sh
tools/board.sh esp32-devkit build -DCOMMA_CONNECT_URL_OVERRIDE= -DCOMMA_CONNECT_TEST_CA=
```

For local WSS, `-DCOMMA_CONNECT_TEST_CA=/absolute/path/test-ca.pem` embeds a test CA, only together
with the URL override; certificate verification stays on. Do not distribute such images. The fake
Connector supports `--tls-cert`, `--tls-key`, `--text-limit`, `--discover`, `--discover-probes
[JSON_FILE]` (Gree and miIO hellos by default), `--discover-timeout`, `--tunnel` and `--loop 3`. It
reads the board's limits from its metadata and skips methods the board does not advertise. Binary
answers are saved as base64 JSON under `--out`; logs show only ids, byte counts and cut flags.

The HTTP, UDP and discovery host tests accept `--devkit` to use this overlay's limits. The tunnel
test uses the two-tunnel profile; the DevKit advertises no tunnel method.

To add a board, follow [AGENTS.md](AGENTS.md) and the checklist in
[docs/development.md](../docs/development.md#adding-a-board). Do not infer a board's capacity from a
successful build.

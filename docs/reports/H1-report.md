# H1 — Shared firmware and no-PSRAM ESP32 DevKit

Date: 2026-10-11–12. Base: `0fe245a`. Worktree: `Hardware-h1`, branch `codex/multi-board`.

**Implementation and build/host checks are ready for review. Hardware acceptance is incomplete.**
The DevKit was flashed, its GPIO2 LED was confirmed by the user, and BOOT timing/reset logic was
exercised through the USB UART control lines. BLE system pairing could not be completed while the
Mac was locked. The Wi-Fi credentials from the specified gitignored file were also tried through
a temporary NVS image; the ESP32 repeatedly reported reason 201 (`WIFI_REASON_NO_AP_FOUND`). No TLS
session or LAN workload reached the device, so this report does not claim they fit.

No merge, push, backend/app modification, real backend token use or T-Circle flash was performed.
The DevKit is left on the normal build, without the local URL/CA override, unprovisioned after reset.

## Commits and layout

| Step | Commit | Change |
| --- | --- | --- |
| 1 | `4cc7cc4` | Pure `git mv t-circle-s3 firmware`, 100% renames, no content edits |
| 1 | `6ebd5f7` | Shared project, T-Circle board directory, overlay, generated per-board sdkconfig and build helper |
| 2 | `331a011` | RAM fallback, configurable limits, board identity and capability selection |
| 3 | `bcc390b` | ESP32 DevKit overlay/partition/LED and Secure Connections Just Works |
| 4 | `b21100e` | Heap instrumentation, OOM recovery and capability-aware local measurement harness |

- `firmware/main/`: Connector, LAN code, Wi-Fi, device configuration, provisioning, shared BOOT
  behavior, `Kconfig.projbuild`, `firmware_config.h`, `memory_caps.h`, `heap_diag.h` and board API.
- `firmware/boards/t_circle_s3/`: display, touch, fonts/logo, audio, playback, voice and linker fragment.
- `firmware/boards/esp32_devkit/`: GPIO0 BOOT input, GPIO2 LED and serial feedback.
- `devices/sdkconfig.<board>`: board choices and constraints. `tools/board.sh` selects the target and
  shared+board defaults; generated sdkconfig and objects live in `firmware/build-<board>/`.
- `firmware/cmake/idf_patches.cmake`: shared build-local NimBLE 90 s timeout patch and ARP table size 32.
  The installed ESP-IDF was not edited.

The shared default limits preserve T-Circle. `COMMA_BUFFER_CAPS` means SPIRAM|8BIT with PSRAM, or
INTERNAL|8BIT without it. Extmem allocation steering compiles only with SPIRAM. No-voice builds use
a false busy guard and do not depend on `cue_voice_state()`. Metadata uses the configured client
source for both identity fields and includes only enabled methods.

See `devices/AGENTS.md` for the add-a-board checklist. The other two planned boards were not added.
ESP-IDF collects requirements before Kconfig: for the two supported targets, the esp32s3 manifest
predicate and requirement list select the voice/display dependencies, while esp32 excludes them.
A future no-voice S3 board needs that dependency predicate revisited explicitly.

## T-Circle regression evidence

ESP-IDF 6.0.2 was used for both checkouts. All existing keys in the final generated T-Circle
sdkconfig exactly match `0fe245a:t-circle-s3/sdkconfig`, including disabled keys. Only these new
board/feature/limit keys were added:

```ini
CONFIG_COMMA_BOARD_T_CIRCLE_S3=y
CONFIG_COMMA_BOARD_ID="t-circle-s3"
CONFIG_COMMA_CLIENT_SOURCE="t-circle"
CONFIG_COMMA_HAS_VOICE=y
CONFIG_COMMA_HAS_DISPLAY=y
CONFIG_COMMA_PAIRING_PASSKEY=y
CONFIG_COMMA_LAN_DISCOVER=y
CONFIG_COMMA_HEAP_DIAGNOSTICS=n
CONFIG_COMMA_LAN_HTTP_MAX_BYTES=65536
CONFIG_COMMA_LAN_HTTP_CHUNK_BYTES=32768
CONFIG_COMMA_LAN_HTTP_CHUNK_FRAME_BYTES=8192
CONFIG_COMMA_CONNECTOR_RX_MAX=73728
CONFIG_COMMA_LAN_DISCOVER_ARENA_BYTES=98304
CONFIG_COMMA_LAN_UDP_MAX_ANSWER=4096
CONFIG_COMMA_LAN_TUNNELS=2
CONFIG_COMMA_LAN_TUNNEL_WINDOW=131072
CONFIG_COMMA_LAN_TUNNEL_MAX_FRAME=8192
CONFIG_COMMA_CONNECTOR_WS_BUFFER_SIZE=4096
CONFIG_COMMA_LED_GPIO=-1
CONFIG_COMMA_BOOT_GPIO=0
```

The T-Circle lockfile's entire dependency payload (versions and component hashes) is unchanged.
Only `manifest_hash` changed from `0b298641…` to `f08e9b1b…`, reflecting the conditional dependency.
ESP-SR is pinned to the previously locked **2.5.5**. No `update-dependencies` command was used.
The attempted config-dependent manifest condition was unsuitable for IDF's early dependency pass;
the final target predicate avoids dependency resolution churn within a build. DevKit has its own
`dependencies.lock.esp32` (cJSON, WebSocket client and IDF, no ESP-SR).

| Size (bytes) | Main baseline | Step 1 split | Final branch |
| --- | ---: | ---: | ---: |
| Total image | 2,024,723 | 2,024,479 | 2,026,507 |
| Flash code | 1,511,270 | 1,510,930 | 1,512,314 |
| Flash data | 349,732 | 349,828 | 350,436 |
| DIRAM | 178,677 | 178,693 | 178,745 |
| External BSS | 33,876 | 33,876 | 33,876 |

Final image growth is 1,784 bytes (about 0.09%), DIRAM growth 68 bytes. The image is not bit-identical:
board extraction, changed source paths and the added OOM handling affect code/string/link layout.
The successful protocol behavior, T-Circle pairing mode, limit values, voice sources/models, and
external BSS placement remain unchanged. This is build/config/host evidence, not a T-Circle hardware
smoke test; that board was not attached or flashed.

<details>
<summary>Full per-archive size comparison: every nonzero delta (bytes)</summary>

These are linked sizes from `esp_idf_size --archives --format json2`. Small changes to unedited
archives can reflect linker relaxation, merged constants and attribution; they are not dependency
version changes. Unlisted archives have equal size.

| Archive | Main | Branch | Delta |
| --- | ---: | ---: | ---: |
| `libbt.a` | 94,317 | 94,321 | +4 |
| `libbtbb.a` | 3,583 | 3,563 | -20 |
| `libdl_lib.a` | 180,085 | 180,081 | -4 |
| `libesp_audio_front_end.a` | 16,840 | 16,852 | +12 |
| `libesp_audio_processor.a` | 86,215 | 86,231 | +16 |
| `libesp_driver_dma.a` | 5,270 | 5,266 | -4 |
| `libesp_driver_gpio.a` | 7,211 | 7,187 | -24 |
| `libesp_driver_i2c.a` | 11,721 | 11,649 | -72 |
| `libesp_driver_i2s.a` | 11,705 | 11,681 | -24 |
| `libesp_driver_spi.a` | 13,919 | 13,999 | +80 |
| `libesp_event.a` | 3,774 | 3,770 | -4 |
| `libesp_hal_clock.a` | 497 | 493 | -4 |
| `libesp_hal_gpio.a` | 2,318 | 2,314 | -4 |
| `libesp_hal_gpspi.a` | 3,958 | 3,950 | -8 |
| `libesp_hal_i2s.a` | 2,520 | 2,524 | +4 |
| `libesp_hal_mspi.a` | 6,002 | 5,998 | -4 |
| `libesp_hw_support.a` | 35,284 | 35,248 | -36 |
| `libesp_lcd.a` | 3,682 | 3,667 | -15 |
| `libesp_lcd_gc9d01.a` | 2,944 | 2,916 | -28 |
| `libesp_mm.a` | 4,790 | 4,774 | -16 |
| `libesp_partition.a` | 2,087 | 2,079 | -8 |
| `libesp_security.a` | 1,690 | 1,678 | -12 |
| `libesp_stdio.a` | 147,710 | 148,320 | +610 |
| `libesp_system.a` | 15,369 | 15,361 | -8 |
| `libesp_wifi.a` | 5,546 | 5,530 | -16 |
| `libespressif__esp-dl.a` | 153,129 | 152,829 | -300 |
| `libespressif__esp-sr.a` | 906 | 918 | +12 |
| `libespressif__esp_websocket_client.a` | 9,183 | 9,195 | +12 |
| `libfbs_model.a` | 17,593 | 18,113 | +520 |
| `libfreertos.a` | 27,547 | 28,171 | +624 |
| `libheap.a` | 12,161 | 12,133 | -28 |
| `libhttp_parser.a` | 14,878 | 14,874 | -4 |
| `liblog.a` | 1,931 | 1,923 | -8 |
| `liblwip.a` | 110,665 | 110,649 | -16 |
| `libmain.a` | 116,223 | 117,260 | +1,037 |
| `libmbedtls.a` | 25,021 | 25,065 | +44 |
| `libnet80211.a` | 150,727 | 150,731 | +4 |
| `libphy.a` | 36,428 | 36,412 | -16 |
| `libpp.a` | 65,202 | 65,114 | -88 |
| `libtcp_transport.a` | 10,234 | 10,250 | +16 |
| `libtfpsacrypto.a` | 115,557 | 115,157 | -400 |
| `libwakenet.a` | 28,370 | 28,342 | -28 |
| `libwpa_supplicant.a` | 79,531 | 79,527 | -4 |

</details>

## DevKit configuration and build

Verified with esptool: ESP32-D0WD rev v1.0, MAC `98:f4:ab:85:6a:d8`, 4 MB flash, no PSRAM, port
`/dev/cu.usbserial-0001`. DIO/80 MHz flash, 240 MHz CPU, minimum revision 1.0. Partition layout:
NVS `0x9000/0x6000`, PHY `0xf000/0x1000`, one factory app `0x10000/0x3f0000`; no model or OTA partition.

The normal DevKit image is **1,270,968 bytes** (unpadded size), IRAM 92,215 bytes and DRAM 52,171 bytes.
The compile database and archive map contain no T-Circle board, ESP-SR, cue_voice, playback,
voice_clean or GC9D01 source/archive. The DevKit dependency resolution downloaded no ESP-SR.
Both a local WSS test build and the normal build compiled and flashed successfully.

No-PSRAM defaults follow the reasons in muse-gadget-sdk's ideaspark/c6 overlays: internal mbedTLS
allocation, 16 KiB inbound / 4 KiB outbound records, dynamic TLS buffers/config release, BLE-only
controller, internal NimBLE pools, eight static Wi-Fi RX buffers and BA window 16, smaller dynamic
Wi-Fi pools, and lwIP/Wi-Fi IRAM optimizations off. Configuration values were modeled on muse;
**no muse implementation code was copied and there is no SDK dependency**.

Pairing is a board option. T-Circle keeps display-only passkey, MITM and authenticated GATT. DevKit
uses NoInputNoOutput, MITM off, encryption-only GATT and a 16-byte minimum key. Both enable Secure
Connections and disable legacy pairing. Crucially, NimBLE's `SM_SC_ONLY=1` also demands authenticated
Level 4 GATT: DevKit must set it to **0**, with `SM_LEGACY=n` still rejecting non-SC peers. This was
identified directly in IDF's `ble_att_svr.c` and `ble_sm.c` after the first protected read failed.

Just Works cannot prevent active MITM. BOOT still gates a provisioned board's pairing window.
UUIDs, credential fields and status strings are unchanged. The existing advertisement model byte
stays `0x01` for scanner compatibility; `info.model` identifies `esp32-devkit`. The app still receives
all existing credential fields, including voice fields. Its existing `createBond()` path needs no
protocol change; the actual system confirmation/secure write sequence remains unverified.

## Required memory table

All values are bytes, from `MALLOC_CAP_INTERNAL`. A dash means **not measured**, not zero.

| Point | Free | Largest block | Minimum ever | Evidence/status |
| --- | ---: | ---: | ---: | --- |
| Boot, before Wi-Fi | 252,708 | 110,592 | 252,280 | Final normal build, unprovisioned |
| Pairing, BLE up and Wi-Fi associated | — | — | — | Only radio initialization was measured; AP not found |
| Immediately after saved, BLE still up, TLS connecting | — | — | — | BLE pairing/save and Wi-Fi blocked |
| Connector online, BLE off, idle | — | — | — | No Wi-Fi association/TLS session |
| HTTP text at the 16 KiB limit | — | — | — | No Connector session |
| HTTP chunks, 1 MiB | — | — | — | No Connector session |
| Discovery on office LAN | — | — | — | Office AP unavailable |
| One tunnel, 32 KiB server frames, 1 MiB each way | — | — | — | No Connector session |

Additional observations, which must not be substituted for the missing workload measurements:

| Actual state | Free | Largest | Minimum ever |
| --- | ---: | ---: | ---: |
| Fresh setup: BLE and Wi-Fi radios initialized, no Wi-Fi association | 172,852 | 110,592 | 172,380 |
| After main exits in that setup session | 176,468 | 110,592 | 167,492 |
| Stored test config: Connector/LAN/tunnel tasks allocated, waiting for Wi-Fi | 125,468 | 61,440 | 115,512 |
| BOOT re-enters pairing while those tasks wait for Wi-Fi | 89,152 | 49,152 | 89,120 |
| Same pairing session, steady sampled state | 88,468 | 49,152 | 86,428 |
| BLE shutdown at five-minute timeout, before provisioning task exits | 133,620 | 49,152 | 86,428 |

Lowest observed stack free space in these *idle/control* runs: UI 4,744; Connector 4,456; tunnel 5,088;
provisioning 4,876 bytes. These are IDF high-water marks, not transfer-load stack measurements. LAN
worker, WebSocket/TLS and watchdog workload watermarks still need the online run. No reboot or panic
was observed other than the requested reset/flash operations.

### Decisions still pending measurements

- **Setup order unchanged:** Connector starts after credentials are saved while BLE is still up.
  There is no measured TLS failure to justify early BLE shutdown; no `connector_failed` notification
  capability has been removed.
- **Discovery stays experimentally enabled** with the initial 16 KiB arena. No fit/pass claim is made.
- **One tunnel stays an experimental measurement configuration**, with a 32 KiB window, 4 KiB outgoing
  frames and a 48 KiB JSON RX buffer. It is not accepted as a supported capacity until all three
  1 MiB bidirectional runs pass and heap recovery is shown. If those runs fail, set the count to zero
  and verify metadata omits it. No backend/protocol change was made. Honoring `max_frame` server-side
  would reduce incoming base64, JSON-copy and decoded-block pressure, but would require a separate
  backend change and new measurements; it cannot be assumed today.

## Current compiled limits and metadata

This is the configured output, **not an on-wire metadata capture** (no network session was reached).
T-Circle values are the original defaults.

| Setting | T-Circle | DevKit experimental |
| --- | ---: | ---: |
| HTTP text max_bytes | 65,536 | 16,384 |
| HTTP max_timeout_seconds | 30 | 30 |
| response_chunks.max_total_bytes | 4,194,304 | 4,194,304 |
| response_chunks.chunk_bytes | 32,768 | 4,096 |
| Raw gated chunk frame | 8,192 | 4,096 |
| Connector RX JSON | 73,728 | 49,152 |
| Discovery arena | 98,304 | 16,384 |
| UDP answer | 4,096 | 2,048 |
| Tunnel count | 2 | 1 |
| Dial result window | 131,072 | 32,768 |
| Dial result max_frame (device → server) | 8,192 | 4,096 |
| Accepted server raw tunnel frame | 32,768 | 32,768 |
| WebSocket read/write buffer_size | 4,096 | 2,048 |

DevKit `client_source` and `component_versions` key are `esp32-devkit` (version `0.1.0`). Enabled
methods are `http_request`, `udp_request`, `lan_discover`, `lan_tcp_dial`. Scope remains read-only
until allowed. Disabled-method CMake selection and metadata guards are tied to the same Kconfig.
The outgoing gated chunk size is never larger than the advertised chunk cap on either board.

## Tests and actual output

Main baseline and the step-1/step-2 branch passed the existing Python and C suites. Final results:

```text
Python (tools/.venv, including bleak): Ran 21 tests in 63.279s — OK
lan_discover: 1221 checks, 0 failed
lan_http:      263 passed, 0 failed
lan_udp:      747 checks, 0 failed
lan_tunnel:  1199 checks, 0 failed
```

The HTTP, UDP and discovery suites additionally pass with `--devkit`, which reads numeric limits
from the DevKit overlay: respectively 263, 747 and 1221 passing checks. The old tests assumed
T-Circle chunk counts and whole-host JSON truncation; they now verify the configured frame cap and
allow a small arena to truncate strings before the JSON budget drops hosts, retaining payload/hash,
size, truncation and default-profile checks. This is host limit coverage, not ESP32 Wi-Fi/TLS proof.

C suites use AddressSanitizer and UndefinedBehaviorSanitizer. New fault injection covers failed
startup allocation (resources released), failure to allocate a 32 KiB decoded block and failure to
allocate a tunnel command. Both live-tunnel failures emit an error/eof, return the tunnel count to
zero and allow a subsequent echo to succeed. Connector startup now cleans partial allocations and
retries after five seconds; failed RX copies and LAN reply allocations have error paths. These tests
prove those paths under host simulation, not every hardware OOM scenario or three-round heap recovery.

One earlier tunnel-suite run hit its deliberately short two-second idle timeout during the host's
sleep/interruption period; the awake rerun and final run passed. A first Python invocation used the
system interpreter without bleak and failed import; the reported complete run uses the tools venv.
Existing fake-board socket ResourceWarnings are still present in the Python suite.

Fake Connector changes: missing `lan_tcp_dial` / `response_chunks` are skipped with explicit output;
text limit, discovery and tunnels run per round; tunnel server frames stay 32 KiB; optional WSS uses
a certificate/key. The firmware test-CA option requires the URL override and keeps certificate
verification enabled. Test CA/key and Wi-Fi values were never committed.

The actual board-bound fake Connector run did not receive a session:

```text
serving work/files on :8000
TCP echo server on :7007
TCP source server on :7008 (1048576 B per connection)
fake Connector on ...:8765/v1/connect — waiting for the board
```

| Requested board workload | Round 1 | Round 2 | Round 3 |
| --- | --- | --- | --- |
| Text at max_bytes | Not started | Not started | Not started |
| 1 MiB chunk transfer | Not started | Not started | Not started |
| Office discovery | Not started | Not started | Not started |
| 1 MiB tunnel each way, 32 KiB server frames | Not started | Not started | Not started |

There are no PASS output lines for those board workloads. Relevant serial evidence (SSID redacted):

```text
wifi: connecting to [redacted]
wifi: disconnected, reason 201, reconnecting
connector: waiting for Wi-Fi
```

BLE scan found `Comma-6AD8`, MTU 517 was negotiated, and the user confirmed slow advertising blink
and fast connected blink. The initial protected read failed with insufficient authentication under
SC_ONLY=1. After changing to SC_ONLY=0, system pairing remained pending while macOS was locked;
no secure GATT write, `saved`, solid-success LED or online status is claimed.

### BOOT/control checks

UART DTR drove GPIO0 low while RTS left EN released. This tests the real firmware input/control
path but does not verify the physical switch contacts:

```text
BOOT low 4 seconds, released
I (9062) prov: provisioning started (pairing mode)
I (9263) prov: advertising as Comma-6AD8, mfg=FF FF 01 01 01 6A D8
I (309081) prov: pairing mode timed out
I (309088) heap: ble_off free=133620 largest=49152 min=86428 task=prov stack_free=4876
BOOT low 11 seconds
W (329037) main: factory reset
W (329881) config: NVS erased: ESP_OK
I (615) config: no stored credentials
I (979) prov: provisioning started (unprovisioned)
```

Timeout was 300.019 seconds after the pairing-start log. Serial reset with stored NVS loaded the
credentials and kept BLE off while retrying Wi-Fi. This covers NVS retention across reset, **not a
physical unplug/replug**. The temporary NVS generation files containing Wi-Fi credentials were
deleted; the reset erased them from the board. Final flash excludes the local URL and CA overrides; CMake cache values are empty and those test
strings are absent from the application binary. Its fresh setup radio-only heap is 172,808 free /
110,592 largest / 172,336 minimum, then 176,424 free after main exits.

## Remaining acceptance work

1. Unlock the Mac for system BLE pairing. Make the 2.4 GHz AP from the specified secrets file
   reachable, or update that gitignored file with an available network. Do not print credentials.
2. Build the local WSS test image, provision through `firmware/tools/ble_provision.py`, and record
   secure pairing, `saved`, the three-second solid LED and TLS overlap before deciding setup order.
3. Run the fake Connector with `--make-files/--serve`, `--only 1m.bin`, `--text-limit`, `--discover`,
   `--tunnel`, `--loop 3`, a pause for idle sampling, and `--tls-cert/--tls-key`. Record on-wire metadata,
   each verdict, all eight heap points and recovery. Only then choose final discovery/tunnel settings.
4. Verify physical BOOT and unplug/replug; keep T-Circle hardware testing optional and separately
   authorized. Repeat normal DevKit build after any tuning and clear cached local URL/CA overrides.

The implementation and evidence are committed for review, but H1's full hardware acceptance remains
open until these checks are completed.

## H1 review follow-up — 2026-10-12

This follow-up fixes the two error-handling regressions on `codex/multi-board` in one separate
commit. It does not change the protocol, board limits, pairing, or memory-tuning settings.

### Malformed JSON versus allocation failure

`handle_text` now uses `connector_json_parse`. Invalid JSON is logged and ignored, without sending
an OOM response or closing the session. The existing OOM response/session-close path runs only when
an allocation actually failed during that parse.

The allocation-failure callback is installed once in `app_main`, before starting the application
tasks. ESP-IDF 6.0.2 invokes it only after malloc's internal/PSRAM fallback has failed. Two native
thread-local flags scope the result to the parsing task; failures in other tasks, interrupts, or
before the parse do not contaminate it. The callback only records the failure. cJSON keeps its
default global allocator hooks, preserving its realloc path for printing in every task.

The new `firmware/tools/connector_json_test/run.sh` compiles the actual parser wrapper and cJSON
with ASan/UBSan. It checks malformed frames with and without an ID, failure at every allocation in
a nested request, an OOM frame without an ID, cleanup after partial parses, recovery on the next
frame, and unrelated task/interrupt failures. It verifies that malformed frames never invoke the
Connector's OOM callback, which owns error sending and session closure.

### Failed tunnel commands retain their identity

The single all-tunnel failure flag is replaced by a separate queue of `{session, id}` records,
reserved at tunnel startup. Both command allocation failure and command queue failure use it
without allocating memory. The tunnel task drains accepted commands before handling failures, so
an already-queued dial can be found. It finishes only the matching tunnel. An identified but
unknown or stale-session command is ignored; only a record with no usable identity falls back to
ending all tunnels.

The failure queue holds one record per configured tunnel. If it fills, the Connector task waits
for the tunnel task to drain it, without holding the host send lock. Records are never overwritten
or converted into a global shutdown just because a queue is full. Startup failure also frees this
queue.

The tunnel host test now keeps two tunnels open while injecting decoded-buffer OOM, command OOM,
and command-send failure, and while actually filling the command queue. Each case requires the
affected tunnel's error EOF and successful bidirectional transfer plus acknowledgment on the other
tunnel. Additional cases cover stale sessions, unknown IDs, a refused new dial, an unidentified
failure, and back-pressure when multiple failure records fill the separate queue.

### Validation

All C suites use ASan/UBSan. Results from this follow-up:

| Check | Result |
| --- | --- |
| Connector JSON host test | 123 checks, 0 failed |
| TCP tunnel host test | 1,367 checks, 0 failed |
| Discovery host test, default / `--devkit` | 1,221 checks each, 0 failed |
| HTTP host test, default / `--devkit` | 263 passed each, 0 failed |
| UDP host test, default / `--devkit` | 747 checks each, 0 failed |
| Python `unittest discover -s firmware/tools -p 'test_*.py'` | 21 tests in 63.079 s, OK |
| `tools/board.sh t-circle-s3 build` | PASS; app image `0x1eeea0` bytes |
| `tools/board.sh esp32-devkit build` | PASS; app image `0x1366e0` bytes |
| `git diff --check` | PASS |

Python used the existing tools virtual environment with `bleak`. Existing socket `ResourceWarning`
messages in `test_fake_connector.py` and the existing C library deprecation warnings remain;
neither caused a test failure. Both firmware builds used ESP-IDF 6.0.2.

`CONFIG_COMMA_HEAP_DIAGNOSTICS=y` remains enabled for DevKit; T-Circle's existing setting is
unchanged. No board overlay, shared sdkconfig default, Kconfig option, or tunnel limit changed.
Generated configs still have `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=24576` and
`CONFIG_LWIP_TCP_WND_DEFAULT=65535` for both boards, with application tunnel windows of 32,768 bytes
(DevKit) and 131,072 bytes (T-Circle).

This review follow-up performed host tests and builds only; it did not flash either board or add
hardware heap measurements. The hardware acceptance items above remain open, and heap diagnostics
and TCP window tuning still await those measurements. No merge or push was performed.

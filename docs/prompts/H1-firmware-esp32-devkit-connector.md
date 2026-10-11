# H1 · Firmware: one project for every board, and a Connector-only ESP32 DevKit

Read first:
- `README.md`, especially "Planned boards" (the per-board targets and what is still unverified);
- `docs/connector-protocol.md` (§4 `http_request` chunk mode and `lan_tcp_dial`, §6 limits);
- `docs/ble-provisioning.md` (the GATT contract the app already speaks);
- `docs/reports/G1-report.md`, `G3-report.md`, and the B1 report's notes on internal heap during setup;
- the firmware in `t-circle-s3/`: `CMakeLists.txt` (NimBLE patch, ARP table), `sdkconfig.defaults`, `sdkconfig`,
  `partitions.csv`, `main/CMakeLists.txt`, `main/idf_component.yml`, and in `main/`: `main.c`, `connector.c`,
  `provisioning.c`, `net_watchdog.c`, `lan_*.c`, `wifi.c`, `device_config.c`.

IDF 6.0.2: `. ~/.espressif/tools/activate_idf_v6.0.2.sh`.

Work in a worktree: `git -C ~/Desktop/AFK/Hardware worktree add ~/Desktop/AFK/Hardware-h1 -b codex/multi-board`.
Commit on that branch in the steps below, one or more commits per step. Don't merge into main and don't push:
the result gets reviewed first.

## Why

Every ESP32 board should be able to serve as a home Connector (LAN gateway). Voice is only for boards with a
microphone and speaker. We plan four boards (README "Planned boards"), and more may follow, so the firmware becomes
**one project with per-board overlays** instead of one project per board. The first new board is the cheapest and
tightest one, a plain ESP32 DevKit. It tells us, with numbers, whether "Connector on any ESP32" holds.

## Reference: muse-gadget-sdk (read it, don't depend on it)

`~/Desktop/AFK/muse-gadget-sdk/esp32` is an Apache-2.0 ESP-IDF firmware that already runs one codebase on 24 boards,
including classic ESP32 boards without PSRAM. Use it as the model:
- **Layout:** one project, `sdkconfig.defaults` plus a `devices/sdkconfig.<board>` overlay per board, and
  `tools/board.sh BOARD [build|flash|monitor|flash-monitor] [PORT]` that sets the target and
  `SDKCONFIG_DEFAULTS` and builds each board in `build-<board>/`. Read `README.md`, `AGENTS.md`,
  `devices/README.md`, `devices/AGENTS.md` and `tools/board.sh`. Board features are Kconfig options
  (`main/Kconfig.projbuild`, `CONFIG_HOMEHUB_*`), and code checks them with `#if`.
- **No PSRAM:** `devices/sdkconfig.ideaspark` (classic ESP32) and `devices/sdkconfig.c6-nopsram`. Start from their
  memory settings, since each line there has its reason in a comment: mbedTLS internal allocation, outbound TLS
  record 4 KB (inbound stays 16 KB because servers send 16 KB records), BLE-only controller, fewer static Wi-Fi RX
  buffers and smaller block-ack windows, lwIP/Wi-Fi IRAM optimizations off so the image links.
- **Their finding:** on those boards they turned off their home-network tunnel **and** network discovery
  ("do not fit next to the control session"). Their tunnel is a different design (a netif over Noise), so this
  is a strong hint, not our answer: measure ours.
- **Pairing without a screen:** they use an app-level "press the button" confirmation and say plainly that it
  cannot stop an active man-in-the-middle. Our plan below (step 3) reaches the same level with no app change.
- Their ideaspark overlay requires chip rev ≥ 3.0 only because of signed apps; we don't sign, so rev v1.0 is fine.

Copying their sdkconfig settings is fine. If you copy code, keep the Apache-2.0 header and say so in the report.

## The DevKit

- ESP32-D0WD **rev v1.0**, dual core 240 MHz, **4 MB flash, no PSRAM** (read with `esptool flash-id`).
- USB serial `/dev/cu.usbserial-0001`, MAC `98:f4:ab:85:6a:d8`.
- BOOT button on GPIO0 (active low), EN (reset) button. A plain blue LED, usually on GPIO2: check before relying on it.
- No screen, no microphone, no speaker.
- The user plugged it in for this work: flashing and erasing it is fine. **Don't flash the T-Circle**; ask the user if
  you need it for a check.

## Step 1 · Move to one project (T-Circle only, nothing else changes)

- `git mv t-circle-s3 firmware` as a **pure rename commit** (no content changes), so history and review stay readable.
- Add `devices/sdkconfig.t-circle-s3` and `tools/board.sh` in the muse style. Move T-Circle-only settings out of
  `sdkconfig.defaults` into the overlay. Shared settings stay in `sdkconfig.defaults`. Then decide what happens to the
  checked-in `sdkconfig`: with per-board build directories it should become generated (`build-<board>/sdkconfig`).
  **Proof:** the `sdkconfig` that `tools/board.sh t-circle-s3 build` generates must equal today's checked-in one,
  except for keys you list and explain in the report.
- Board-specific code goes in its own place (for example `boards/t_circle_s3/`: display, touch, audio, UI), and
  the shared gateway stays in `main/`. Selection uses Kconfig: a board choice plus feature options, for example
  `COMMA_HAS_VOICE`, `COMMA_HAS_DISPLAY`, the pairing method, `COMMA_LAN_TUNNELS` (0–2), `COMMA_LAN_DISCOVER`, and
  the size limits in step 2. Each board's overlay sets them. `main.c`'s UI loop is board-specific today: split it so
  the shared part (start the Connector once provisioned, BOOT hold → pairing / reset) is shared, and only the
  feedback (screen, LED) is per board.
- The voice stack (`cue_voice`, `playback`, `board_audio`, `voice_clean`, `esp-sr`, the `voice_clean.lf` linker
  fragment) and the display stack (`esp_lcd_gc9d01`, fonts, logo) build only on boards that enable them. `esp-sr` must
  not be downloaded or linked for the DevKit. Use the component manager's conditional dependencies if they work on
  IDF 6.0.2, or another clean way, and keep `dependencies.lock` stable for T-Circle (no `update-dependencies`).
- Factor the NimBLE pairing-timeout patch and the `ARP_TABLE_SIZE` define so every board gets them.
- Update every path that names `t-circle-s3`: `README.md` (build, flash, tests, project map), the host test scripts
  (`tools/lan_*_test/run.sh`), the Python tests, `.gitignore`, and current docs. **Leave old prompts and reports
  as they are**; they describe history.
- **Proof that T-Circle is unchanged:** build it on main and on the branch, compare `idf.py size` (and
  `size-components`), and run all Python and C host tests. If the T-Circle is plugged in and the user agrees, a
  short smoke test (boot, Connector up, one call) is welcome but optional.

## Step 2 · Make the gateway work without PSRAM (T-Circle behavior identical)

- **PSRAM fallback.** About 20 places allocate with `MALLOC_CAP_SPIRAM` or create tasks with
  `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` (`grep -n SPIRAM`). Without PSRAM these all fail. Replace them with
  one definition meaning "PSRAM if the build has it, else internal 8-bit RAM". `connector.c`'s
  `heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)` must compile without `CONFIG_SPIRAM`.
  Task stacks moved to internal RAM must still fit: measure the high-water marks.
- **Per-board limits** as Kconfig options whose defaults are today's values: `LAN_HTTP_MAX_BYTES`,
  `LAN_HTTP_CHUNK_BYTES` (and the chunk frame), `RX_MAX`, the discovery `ARENA_BYTES`, `LAN_UDP_MAX_ANSWER`,
  tunnel count and `window`, and the WebSocket client `buffer_size`. `metadata` and the `lan_tcp_dial` answer must
  report the values actually in force. Check every number they send.
- **Board identity.** `metadata` hardcodes `"client_source":"t-circle"` and `"component_versions":{"t-circle":…}`.
  Make both per board (`"esp32-devkit"` for the new one). T-Circle keeps sending exactly what it sends today.
- **Methods list.** `lan_gateway.methods` lists only what the board enables (`lan_tcp_dial` only with tunnels,
  `lan_discover` only with discovery).
- **No voice:** `connector.c` and `net_watchdog.c` call `cue_voice_state()` for the "busy: voice call active" guards.
  On a board without voice these must compile without the voice stack and never report busy.

## Step 3 · The DevKit board (`devices/sdkconfig.esp32-devkit`)

- Target `esp32`, 4 MB flash, minimum chip revision that accepts rev v1.0, no SPIRAM, BLE only, NimBLE, and the
  memory settings from the muse no-PSRAM overlays (each with its reason as a comment). A partition table without the
  ESP-SR `model` partition (NVS + one app; no OTA yet).
- Voice off, display off. Its board code: BOOT on GPIO0, the LED, and the provisioning feedback (provisioning.c's
  `display_show_card()` calls become LED states plus a serial log line).
- **Pairing: LE Secure Connections, Just Works.** The Android app (`~/Desktop/AFK/comma-android-add-device`, branch
  `codex/add-device-ble`) bonds through the system: `BleLink.kt` calls `createBond()` and Android shows its own
  dialog. It never handles the passkey itself. With IO capability `NoInputNoOutput` (MITM off, bonding on, legacy
  pairing still refused), Android shows a plain "Pair?" confirmation and **the app needs no change**.
  - The GATT characteristics require `READ_AUTHEN` / `WRITE_AUTHEN` (`provisioning.c` `SECURE_READ` /
    `SECURE_WRITE`), which Just Works cannot satisfy. With this pairing method they require encryption only. The
    pairing method is a board option. T-Circle keeps passkey + MITM exactly as today.
  - Physical presence still gates setup. An unprovisioned board advertises. A provisioned one advertises only after
    a BOOT hold of 3–10 s and leaves pairing mode after 5 minutes. Keep those rules and the 10 s factory reset.
  - LED: off when normal, slow blink in pairing mode, fast blink while a phone is connected over BLE, solid ~3 s
    after `saved`. Log the same transitions.
  - Document the board difference in `docs/ble-provisioning.md`, whose security section says passkey/MITM for every
    device, and state the MITM limit as plainly as muse does.

## Step 4 · Memory: measure, then decide

Log `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`, `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` and the
minimum-ever free size at each point. Fill this table in the report:

| Point | free | largest block | min ever |
| --- | --- | --- | --- |
| boot, before Wi-Fi | | | |
| pairing mode (BLE up, Wi-Fi up) | | | |
| right after `saved`, BLE still up, Connector connecting over TLS | | | |
| Connector up, BLE off, idle | | | |
| `http_request` text mode, at the limit | | | |
| chunk mode, 1 MB file | | | |
| `lan_discover` on the office LAN (if enabled) | | | |
| one tunnel with 32 KiB server frames, 1 MB each way (if enabled) | | | |

Decide the following from the numbers. Make each change only if a measurement forces it, and record why:

- **Setup order.** The Connector starts as soon as credentials are `saved`, while BLE is still up, so the app can hear
  `connector_failed`. The app treats `saved` as success and checks "online" with the backend (`AddDeviceFlow.kt`), so
  it doesn't need `connector_up`. If TLS can't start while BLE holds its memory, this board may send `saved`, close
  BLE, then start the Connector. The GATT contract stays the same. The cost is that the app no longer hears
  `connector_failed` on this board: say so in the report and in `docs/ble-provisioning.md`.
- **Discovery.** Keep it with a smaller arena if it fits. Otherwise turn it off on this board (muse did).
- **Tunnels.** The server sends up to 32 KiB per tunnel frame regardless of the device's `max_frame`, and it assumes
  a 512 KiB window if the device reports less than 32 KiB (protocol §4). If one tunnel with a ≥ 32 KiB window survives
  the 1 MB test both ways three times, enable 1. Otherwise set tunnels to 0. Report the numbers and what a backend
  change (honoring `max_frame`) would need. **Don't change the protocol or the backend.**

Whatever the limits end up being, nothing may crash or leak. Out of memory must become an `error` frame or a refused
request, never a reboot. Run each test at least 3 times in a row and confirm the free heap comes back.

## How to test

- Build with the local fake Connector (no backend token needed), using the existing override:
  `-DCOMMA_CONNECT_URL_OVERRIDE=ws://<mac-ip>:8765/v1/connect` through `tools/board.sh`. Then run
  `python3 firmware/tools/fake_connector.py` with `--make-files/--serve` (text + chunk mode) and `--tunnel`. The fake
  Connector fails if `lan_tcp_dial` or `response_chunks` is missing from `metadata`. Teach it to respect what the
  board advertises, and keep `test_fake_connector.py` passing.
- Provision over BLE with `firmware/tools/ble_provision.py` from the Mac. With Just Works, macOS should show a
  confirmation instead of a code prompt. Wi-Fi credentials: read them from `~/Desktop/AFK/Hardware/t-circle-s3/main/secrets.h` (main checkout,
  gitignored) into environment variables. Never print or commit them. The Connector token and voice key can be dummies
  with the right prefixes, because the fake Connector doesn't check them.
- Check: unprovisioned boot advertises; provisioning reaches `saved`; the Connector comes up; unplug and replug
  brings Wi-Fi and the Connector back without BLE; BOOT 3–10 s re-enters pairing mode; BOOT 10 s erases and reboots
  into setup.
- A real backend run is optional and needs a token issued through the app; ask the user before trying it.

## Don't

- Don't change T-Circle behavior, its pairing method, or its limits.
- Don't change the Connector protocol, the BLE GATT contract (UUIDs, JSON fields, status values), the app or the backend.
- Don't run `idf.py update-dependencies` and don't edit the installed IDF.
- Don't commit secrets, `secrets.h`, SDK tokens or build directories.
- Don't add the AUDIO-Board or M5Stick yet. Leave a clear "add a board" path (muse's `devices/AGENTS.md` is the model).

## Report

Write `docs/reports/H1-report.md` covering:
- the layout and Kconfig options, and how to add a board;
- the T-Circle no-change proof (sdkconfig diff, size comparison, host tests);
- the heap table and each decision with its reason;
- the DevKit's final limits as `metadata` reports them;
- the test results (with the fake Connector's output for each test);
- anything flaky, and anything copied from muse.

Update `README.md`: build/flash with `tools/board.sh`, the folder table, and the DevKit row in "Planned boards"
with what was measured. Also add a `devices/README.md` board table in the muse style.

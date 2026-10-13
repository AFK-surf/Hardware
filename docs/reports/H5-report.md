# H5 report: open-source preparation

2026-10-13 · branch `codex/open-source-prep` from `main` 8bb7717 · worktree `~/Desktop/AFK/Hardware-h5`

Two commits: `H5: prepare the firmware for open source` (this report is in it) and
`connector: log where a silent Connector socket stalled`. Nothing flashed, merged or pushed.

## 1. Public tree and export

`tools/export_public.sh DEST` runs `git archive HEAD` with exclude pathspecs, so only committed,
tracked files leave: never `secrets.h`, generated `sdkconfig`, `build-*`, `managed_components`,
`tools/.venv`, test certificates or uncommitted edits (it warns about the last). DEST must be empty.

| Excluded path | Why |
| --- | --- |
| `docs/prompts/` | Agent instructions per step |
| `docs/reports/` | Development reports: networks, device IDs, MACs, logs, local paths |
| `docs/connector-plan.md` | Internal plan (Chinese): backend source paths, the shared board's port and MAC, office/home SSIDs, staging device ID, team process. Superseded by the README and the two protocol docs; nothing in it is needed to build or use the firmware |
| `firmware/reference/cue-logo/` | `CueLogoAnimation.tsx` + CSS copied from the private app repository; its license is the app's. `gen_cue_logo.py` already embeds the geometry, and its docstring no longer points there |
| `tools/mini_connector.py` | A1 protocol spike against a local backend, with probe flags for undocumented server behaviour. `fake_connector.py` covers board testing |
| `tools/check_public.py`, `tools/export_public.sh` | The leak check holds the private deny list (names, SSIDs, hosts, a password hash); both only make sense for the private→public export |

Exported tree: 186 files.

## 2. Leak check

`tools/check_public.py DIR` prints `path:line: [rule] match`, exits 1 on any hit. Text files are
checked line by line; binaries (PNG, TTF, `.bin`) for embedded paths, names, hosts, emails, staging
hosts, private repos and local secrets.

| Rule | Matches |
| --- | --- |
| `wifi-ssid` | `HUAWEI-…`, `ToEverything…` |
| `wifi-password` | The test Wi-Fi password, by SHA-256 of every 25-character window (the password is never in the file) |
| `local-secret` | Every string value ≥ 6 chars in a local `firmware/main/secrets.h`, read at run time from this checkout and the main checkout |
| `private-ip` | `192.168.x.x` and its reverse-DNS form |
| `mac` | `aa:bb:cc:dd:ee:ff` / `-` separated |
| `device-name` | `Comma-XXXX` / `comma-xxxx` with a hex short ID (derived from a real MAC) |
| `backend-id` | `dev_ ws_ grp1_ run_ vc_ conn_ usr_ req_ tun_ agt_ env_` + a mixed-case or upper+digit value (code identifiers like `dev_count` are single-case and pass) |
| `token` | `salix_conn_…`, `salix_vk_…`, GitHub/OpenAI-style keys, AWS key IDs, PEM private keys |
| `personal-name`, `email` | The user's names and account handles, colleagues' names, gmail/cue/comma addresses |
| `hostname` | `*-MacBook*`, `iMac`, `Mac-mini`, `zhen-core`, `*.ts.net` |
| `local-path` | `/Users/…`, `~/Desktop`, `/home/<user>/`, `C:\Users\` |
| `staging-host` | Any host containing `staging`, `cue.surf` |
| `private-repo` | `AFK-surf`, `salix-connect`, `comma-android`, `voice-box-agent`, `salix_agent`, `SalixAgent`, `Cue main/PR/repo/commit/<sha>`, backend paths `systems/apps|connector|station`, backend app names |
| `pr-number` | `PR #n`, `/pull/n` |
| `internal-doc` | `docs/prompts`, `docs/reports`, `*-report.md` |
| `internal-step` | Step names `A1–A5 B1–B3(a–e) C1–C4(a–d) D1 E1–E4 F1–F2 G1–G3 H1–H5(b) V1–V2`, not after `ESP32-` |

Allow-list (each entry says why in the script):

| Rule | Allowed | Why |
| --- | --- | --- |
| `private-ip` | `192.168.1.0/24`, `192.168.2.0/24` (and reverse form) | The project's documentation and host-test example subnets. The real test networks were 192.168.3/5 |
| `mac` | `01:02:03:04:05:06`; any locally administered MAC (`x2/x6/xA/xE:…`) | Documentation example; never burned into hardware |
| `device-name` | `Comma-1A2B` | Documentation example |
| `backend-id` | `<prefix>_Example…` | Documentation placeholders |
| `token` | `salix_conn_`/`salix_vk_` + nothing, `replace_me`, `EXAMPLE`; `salix_conn_dev_override`, `salix_conn_x` | Placeholders; the dummy token of the dev URL override; a malformed-write test value |
| `internal-step` | `firmware/components/esp_lcd_gc9d01/*`, `LICENSE`, `firmware/reference/fonts/*`, `firmware/ui/*/ui_font.c`, `V2` in `THIRD_PARTY_NOTICES.md` | Vendored driver, license text, font binary, generated tables |

An earlier draft allowed step-like names in all `*.c`/`*.h` as register names. A strict run showed
it hid three real references (`C2`, `A4`, `A2` in comments), so that entry was removed and the
comments fixed.

Result on the exported tree: **186 files checked, 0 hits**. A manual sweep for words the rules
don't cover found "office camera/LAN" and the voice model's internal name; both were reworded.

## 3. What changed in public files

- **Test network → example values.** `192.168.5.x` → `192.168.1.x` in docs, comments and all host
  tests (`lan_http_test` keeps its "other private subnet" and `/31` cases on `192.168.2.x`). The
  board MAC `98:a3:16:f5:5d:28` / `Comma-5D28` → `02:00:00:00:1a:2b` / `Comma-1A2B` (also the
  `0x5D, 0x28` bytes in `test_ble_provision.py`). Apple-OUI MAC and `Zhens-MacBook-Pro` in the
  discovery test → `02:00:00:01:02:03`, `Kitchen-Laptop`. SSID examples → `Home-2.4G`.
- **Comments.** Step tags (`(G1)`, `V2:`, `H3's Waveshare`, `A5`, …) replaced by what they meant
  (`voice_clean`, "the Waveshare AMOLED's earlier driver", …) or dropped; links to reports and
  prompts removed (`lan_discover.h`, `lan_discover_core.h` now point to `docs/connector-protocol.md`;
  `lan_udp.h` keeps its inline contract); `salix-connect` → "the desktop Connector", backend
  source files (`lan_tunnel.go`, `protocol.ex`, `stringFromAny`, `DeviceRelay`) removed;
  `voice-box-agent` → "Waveshare's board support package"; "Cue logo" → "Comma logo" in comments;
  `build-v2off` example dir → `build-clean-off`. Only comments and docs changed in firmware sources.
- **Docs.** `README.md` rewritten (section 5 below). `devices/README.md` rewritten without report
  links. `docs/connector-protocol.md`: new intro, example IDs, step tags and Cue commit references
  removed, DevKit paragraph restated as results. `docs/ble-provisioning.md`: production example
  URLs, all four boards (pairing, model IDs, `FREE_BLE_FOR_DISCOVERY`), no internal names.
  `devices/AGENTS.md`: points to `devices/README.md` instead of the H1 report.
- **Default URLs** (the only intended behaviour change, and only in tools/examples):
  `firmware/tools/ble_provision.py` `--server https://salix.comma.surf`, `--connect-url
  wss://salix.comma.surf/v1/connect` (were staging); `secrets.h.example` the same. The firmware
  itself has no compiled-in URL: it uses what the app sends over BLE, or `secrets.h`. Staging stays
  usable with both flags or in `secrets.h`. The device_config.h comment example is production.
- **`.gitignore`** reorganised; the per-experiment `build-v2*/bench/ns/fake` lines were already
  covered by `build-*/`.

### URL sources

| URL | Source |
| --- | --- |
| `https://salix.comma.surf` (API and voice) | The H5 prompt; confirmed in Cue `clients/apps/android/app/build.gradle.kts:145` (prod `BACKEND_BASE_URL`) and `systems/config/config.example.json` (`api_base_url`) |
| `wss://salix.comma.surf/v1/connect` (Connector) | `docs/connector-protocol.md` §1 (`connect_url` = `wss://<salix-host>/v1/connect`); the backend builds it as `connector_server_url() <> "/v1/connect"` (`salix_env/connector_tokens.ex:856`), where `connector_server_url` is the public base URL with `https`→`wss` (`salix/bindings/env_public_url.ex`) |

## 4. Identifiers left as they are

| Identifier | Where | Recommendation |
| --- | --- | --- |
| CMake `project(cue_logo)` → image `cue_logo.bin`, `cue_logo.elf` | `firmware/CMakeLists.txt` | Rename to `comma` before release (image name is visible to users in flash commands); changes build paths and `flash_args`, so do it as its own change |
| `TAG = "cue"` log tag | `firmware/voice/cue_voice.c` | Rename to `voice`; log lines are user-visible. Update reports/tests that grep `cue:` |
| `cue_voice.c/.h`, `cue_voice_*` functions, `cue_voice_state_t`, `CUE_VOICE_*` enum values | `firmware/voice/` | Rename to `voice_session_*` in one mechanical commit; internal only |
| `CUE_VOICE_SERVER`, `CUE_VOICE_GROUP`, `CUE_VOICE_KEY`, `CUE_WORKSPACE_ID`, `CUE_CONNECTOR_URL`, `CUE_CONNECTOR_TOKEN`, `CUE_VOICE_SIMULATE_DROP`, `CUE_VOICE_PCMU` | `secrets.h(.example)`, `device_config.c`, `cue_voice.c` | Rename to `COMMA_*`; breaks existing local `secrets.h` files, so keep old names as fallbacks for one release |
| `cue_logo.h/.bin`, `CUE_LOGO_W/H/BYTES`, `_binary_cue_logo_bin_*`, `gen_cue_logo.py` | `firmware/ui/`, `firmware/tools/` | Rename to `comma_logo`; together with the project name |
| `X-Salix-Connector-Instance-ID`, `salix_conn_`, `salix_vk_` | protocol | Keep: part of the backend contract |

## 5. README

For outside users: what it does, the banner, a four-board table (support, store link, flash/PSRAM,
build command), quick start (IDF 6.0.2, build, flash with per-board download-mode notes, app setup,
key table: call, 3–10 s pairing, > 10 s reset), security notes (passkey on screen boards, Just Works
on the DevKit and its MITM limit, unencrypted NVS, read-only by default, test BLE company ID, no
OTA), host tests and tools, how to add a board (from `devices/AGENTS.md`), license and trademark.
Store links were checked (HTTP 200 and product name on the page) on 2026-10-13.

**Banner:** the second board in `docs/assets/comma-hardware-banner.png` is an ESP32-S3 dev board
(ESP32-S3-N16R8 module, two USB-C), not the classic ESP32 DevKit (ESP32-D0WD, 4 MB, no PSRAM) this
project supports. The README has no caption naming the boards; its alt text is generic and the
board table states that S3 dev boards are not supported by the DevKit build. Replacing the picture
is the user's decision.

**Decisions for the user:** copyright line `The Comma Hardware Authors` in `NOTICE` (no legal entity
was given); the README links `https://comma.surf`; the trademark wording in the README and
`THIRD_PARTY_NOTICES.md`; `docs/connector-plan.md` stays internal (reason in §1).

## 6. Third-party inventory

Full table in `THIRD_PARTY_NOTICES.md`; `NOTICE` lists the attributions Apache-2.0 asks for.

- **muse-gadget-sdk (Meta, Apache-2.0):** `lan_discover.c`, `lan_discover_core.c`, StickS3
  `board_audio.c`, `panel.c`, `pmic.c` keep Meta's header; DevKit and StickS3 memory/PSRAM settings
  credited in their overlays.
- **Espressif:** vendored `esp_lcd_gc9d01` 2.0.0 (Apache-2.0, own `license.txt`).
- **Waveshare BSP (Apache-2.0):** CO5300 init table and column gap in `panel.c` (header kept); pins
  as facts.
- **M5Stack:** K150 schematic pins and M5PM1 register map as facts; the M5PM1 library (MIT) was only
  read, no code copied (as `pmic.c` already says).
- **LilyGO:** T-Circle pins as facts. The sources do not cite where they came from; the notice says
  LilyGO's board documentation — check if that is not right.
- **Inter 4.001** (OFL 1.1): `firmware/reference/fonts/OFL.txt` added (from the Inter repository's
  `v4.0` tag; the `v4.001` tag has no LICENSE.txt at that path, and the license is unchanged). The
  generated `ui_font.c` files carry `SPDX-License-Identifier: OFL-1.1`, and the generator now writes it.
- **ESP-IDF v6.0.2** (Apache-2.0) and managed components, from the four lock files and their
  downloaded `LICENSE`/manifest: cJSON 1.7.19~2 MIT; esp_websocket_client 1.8.0 Apache-2.0; esp-sr
  2.5.5 Espressif MIT (Espressif chips only); esp-dl 3.3.13 MIT; dl_fft 0.8.0 MIT; esp_new_jpeg 1.0.2
  Espressif MIT; cmake_utilities 0.5.3 Apache-2.0; esp_lcd_co5300 2.1.0 Apache-2.0; esp_codec_dev
  1.6.2 Apache-2.0. Python tools: bleak (MIT), websockets (BSD-3-Clause).
- **Comma name and logo:** `ui/*/cue_logo.bin`, `docs/assets/`: trademark note, not licensed by
  Apache-2.0.

SPDX: `SPDX-License-Identifier: Apache-2.0` added to 129 C/H/Python/shell files we wrote (after the
shebang where there is one); Meta-headed files unchanged; vendored GC9D01 unchanged. `LICENSE` is the
canonical Apache-2.0 text from apache.org.

## 7. Connector silence after a call dialed during connect

**Recorded (H4 call 5, StickS3, staging):** the board rebooted and KEY1 was pressed 3 s later. The
voice socket opened at 6.99 s (`voice socket not identified (2 new connections)`), `session.started`
at 7.62 s; the Connector socket opened at 7.72 s (`Connector socket: fd 55`), sent metadata, and got
`connected run=… generation=15` at 7.78 s. Then no frame at all: `stats … last_heartbeat=never` at
31.8 s, the call ended at 43.6 s, `no frame for 45 s, reconnecting` at 52.81 s (= 7.78 + 45.03 s),
`closed` 100 ms later, up again ~4 s after.

**Cause: not found in the firmware.** What was checked:

- The websocket task was alive: `esp_websocket_client_stop()` returned in ~100 ms, which needs the
  task to leave its 1 s `poll_read` loop, so it was not stuck on a lock or a blocking read.
- Nothing arrived unread: lwIP's `select()` reports pending data, and the SSL transport checks
  TLS-buffered bytes before `select()`. The CPU was idle from 43.6 s to 52.8 s and still nothing came.
- `consume_chunk` drops only PING/PONG and frames whose offsets don't line up; each new frame starts
  at offset 0, so a lost chunk cannot stall later frames.
- Socket identification only reads sockets (`getpeername`, pcb list); neither the Connector nor the
  voice code changes the other's socket. The only pcb that the firmware modifies is a tunnel's LAN
  socket (receive window).
- No shared state between the two websocket clients after the handshake (the only global, the
  certificate bundle, is used during the handshake, and both handshakes succeeded).
- Task priorities (Connector websocket 3, voice 5/6) cannot explain silence that outlasts the call.
- Backend (Cue `systems/apps/salix_web/lib/salix_web/connector_socket.ex`): `connected` is pushed at
  the end of init, heartbeats come from a self-sent 15 s timer, and metadata/heartbeat registry work
  runs in tasks, so a blocked socket process would also need a stuck mailbox, which the code does not
  show.

The one difference between this run and the earlier concurrent run that worked: there the Connector
got `connected` (7.45 s) before `session.started` (7.92 s); here `session.started` came before the
Connector socket opened. Two runs are thin evidence. Backend logs for `run_HGNLk4Ctrax88pWD`
(generation 15, 2026-10-13 ~08:4x local time) would show whether heartbeats were sent on that socket.

**Commit 2 is a diagnostic, not a fix.** When the 45 s timeout fires, the Connector logs:

```
W connector: silent link: pongs=<n> fd=<fd> unread=<bytes> tcp state=<s> unacked=<b> nrtx=<n> rcv_wnd=<b> refused=<0|1>
```

It counts WebSocket PONGs since open and reads the Connector pcb in the tcpip thread (the pattern
`lan_tunnel.c` already uses). Behaviour, protocol and timeouts are unchanged. A host test is not
feasible: `connector.c` is not built on the host, and the behaviour lives in the websocket client,
TLS and lwIP. Images grow by 656–736 bytes; configs are unchanged.

A firmware mitigation is possible but changes the liveness rule, so it is not included: reconnect
when no server heartbeat has arrived ~20 s after open (the server sends one every 15 s). It would cut
the outage from ~49 s to ~24 s without explaining it.

### Hardware steps (T-Circle and StickS3, staging, build of commit 2)

1. Flash `tools/board.sh t-circle-s3 flash-monitor PORT` (StickS3: `m5stack-sticks3`). Keep one
   monitor open; on the StickS3, opening the port resets the board.
2. Reproduce: reset the board and tap the screen (T-Circle) or press KEY1 (StickS3) as soon as the
   logo appears, so the call dials while the Connector connects. The run counts only if
   `cue: session.started call_id=…` comes **before** `connector: socket open`. If `connector:
   connected run=…` comes first, that is the working case: reset and try again.
3. Talk briefly, hang up, and wait 60 s after `connector: connected`.
4. Expected on a repro: `connector: socket open`, `connector: Connector socket: fd N` (or `not
   identified`), `connector: sent metadata scope=…`, `connector: connected run=… generation=…`,
   `connector: stats connected=yes last_heartbeat=never …`, then `W connector: no frame for 45 s,
   reconnecting`, `W connector: silent link: …`, `W connector: closed code=… handshake=… after 45s`,
   `connector: reconnecting in … ms`, and a fresh `connector: connected …`.
5. Read the `silent link` line (`state=4` is ESTABLISHED):
   - `unread>0` or `refused=1`: data sat on the device unread → firmware bug; keep the log.
   - `pongs>0`, `unread=0`, `unacked=0`: the server's WebSocket layer answers PINGs but sends no
     frames → backend (send the run ID).
   - `pongs=0`, `unacked=0`: PINGs are ACKed by TCP but never answered → server or proxy.
   - `unacked>0`, `nrtx` climbing: that connection's path is dead → network/AP/proxy.
6. Control run: start the call ≥ 20 s after boot. `connector: stats … last_heartbeat=` stays under
   15 s through the call, and no `silent link` line appears.
7. Record which case it was and the run ID; that decides the fix (firmware, backend, or the
   mitigation above).

## 8. Checks

| Check | Result |
| --- | --- |
| C host tests (ASan/UBSan) | lan_discover 7574 (runner total), lan_http 263, lan_udp 747, lan_tunnel 1366, connector_json 133, resample 48625, mic_path 236896; `--devkit`: lan_http 263, lan_udp 747, lan_discover 4502. All 0 failed |
| Python (`tools/.venv`, bleak 3.0.2, websockets 17.2) | 26 tests OK (two failed until the `0x5D, 0x28` bytes were updated with the new example short ID) |
| Builds, commit 1 | All four boards build. Generated `sdkconfig` identical to `main` (0 diff lines) on all four; `cue_logo.bin` sizes identical: T-Circle 2,032,576, DevKit 1,261,344, Waveshare 1,510,688, StickS3 2,058,336 |
| Builds, commit 2 | All four build with no warnings; configs identical; sizes 2,033,232 / 1,262,080 / 1,511,344 / 2,059,008 (+656 to +736 bytes, the diagnostic only) |
| Export + leak check | 186 files exported, 0 hits |
| Self-contained | In the exported tree (no `.git`): `idf.py --version` = v6.0.2, `tools/board.sh t-circle-s3 build` OK, same size and an identical `sdkconfig` to the repository build |

The baseline was built from `main` 8bb7717 in a separate detached worktree.

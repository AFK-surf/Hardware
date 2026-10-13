# H5 · Prepare the firmware for open source

Work in a new worktree `~/Desktop/AFK/Hardware-h5`, on a new branch `codex/open-source-prep`, from
local `main` (head `67f8981` or later: H1–H4 merged, four boards). ESP-IDF 6.0.2
(`. ~/.espressif/tools/activate_idf_v6.0.2.sh`). Build with `tools/board.sh BOARD build`.

## Decisions already made (by the user)

- **License: Apache-2.0** for the whole project. Files adapted from muse-gadget-sdk keep their
  "Copyright (c) Meta Platforms, Inc. and affiliates" Apache-2.0 header.
- **Internal documents stay internal.** `docs/prompts/` (agent instructions) and `docs/reports/`
  (development reports) are not part of the public project. Do not delete them from this repository:
  the team reviews the result here first. Someone else will later create the public repository from
  an exported tree, in a single initial commit. So this step makes the public tree exportable and
  checkable, without rewriting history.
- **Default server addresses become production.** In the real flow the phone app sends the server
  addresses during BLE setup. Where the firmware, examples or tools still carry a default, use
  production: `https://salix.comma.surf` for the API and voice, and the matching Connector URL. Confirm
  the production Connector path (`/v1/connect`) in `docs/connector-protocol.md`, and say in the report
  where each URL came from. Staging stays usable through explicit flags or `secrets.h`.

## Read first

`README.md`, `devices/README.md`, `devices/AGENTS.md`, `docs/connector-protocol.md`,
`docs/ble-provisioning.md`, `docs/connector-plan.md`, `firmware/main/secrets.h.example`,
`.gitignore`, `tools/`, `firmware/tools/`, and the H1–H4 reports (internal, for facts only).

## Do

1. **Public tree and export.** Add `tools/export_public.sh DEST`. It copies the tracked files that form
   the public project into `DEST`, leaving out `docs/prompts/`, `docs/reports/` and anything else you
   find internal (list each excluded path and why). It never copies untracked or ignored files
   (`secrets.h`, generated `sdkconfig`, `build-*`, `managed_components`, test certificates).
2. **Leak check.** Add `tools/check_public.py DIR`. It fails, naming file and line, on: Wi-Fi SSIDs
   and passwords seen in this project (`HUAWEI-…`, the test password), private IP addresses of the test
   networks (`192.168.x.x`, except clearly marked documentation examples), MAC addresses, device,
   workspace, group, run and call IDs (`dev_…`, `ws_…`, `grp1_…`, `run_…`, `vc_…`), tokens and keys
   (`salix_conn_`/`salix_vk_` followed by real-looking values), personal names and hostnames, local
   paths (`/Users/`, `~/Desktop/`), staging hosts, and private repository names (`Cue`/`cue` as a
   repository, `comma-android`, `voice-box-agent`, `salix_agent`, `SalixAgent`, internal PR numbers).
   Run it on the exported tree and fix every hit in the public files. Keep an explicit allow-list for
   legitimate words, such as the board name, and make each entry say why.
3. **Code and comments.** Remove internal references from public files: comments that cite
   internal reports, prompts or PRs ("see H2 report", "A5"), private repository names, local paths, and
   staging hosts in defaults. Do **not** rename code identifiers, log tags or the CMake project name
   (`cue_logo`, `cue:` tags, `CUE_*` macros): list them in the report with a recommendation, and the
   user decides. No behaviour change except the default URLs.
4. **License files.** `LICENSE` (Apache-2.0 full text), and an SPDX line or short header in each source
   file we wrote (C, Python, shell). Add `NOTICE` and `THIRD_PARTY_NOTICES.md` for:
   - the muse-gadget-sdk adaptations (Meta, Apache-2.0);
   - the Inter font, from which `ui_font.c` is generated (SIL OFL 1.1). Check that
     `firmware/reference/fonts/` ships the font's license, or add it;
   - ESP-IDF and every managed component in the four lock files, with version and license;
   - pin maps taken from the Waveshare BSP and M5's schematics (facts, not copied code; say so);
   - the Comma logo and name: assets in `ui/*/cue_logo.bin` and `docs/assets/`. Note that the code
     license does not grant trademark rights.
5. **Public docs.** Rewrite `README.md` for outside users:
   - what the firmware does: a home gateway for the Comma app, plus voice on boards with a mic and
     a speaker;
   - the banner `docs/assets/comma-hardware-banner.png` at the top;
   - a table of the four boards: what each supports, where to buy, flash size, the build command;
   - quick start: install IDF 6.0.2, build, flash (with each board's download-mode notes), set up
     from the app, the keys (BOOT / KEY1 / KEY2: call, 3–10 s pairing, >10 s reset);
   - security notes: the pairing method per board, and the Just Works limitation on the DevKit;
   - how to add a board (from `devices/AGENTS.md`, rewritten for humans);
   - license.

   Keep `docs/connector-protocol.md` and `docs/ble-provisioning.md` public after the leak check.
   Decide whether `docs/connector-plan.md` is public or internal, and say why. Note in the report
   that the banner's second board is an ESP32-S3 dev board, not the classic ESP32 DevKit this project
   supports. The user decides whether to replace the picture; the README caption must not suggest
   otherwise.
6. **Shared Connector bug (separate commit).** H4 recorded it: when a voice call is dialed while the
   Connector is still connecting, the Connector gets no messages for 45 s, then reconnects (~4 s). Find
   the cause in the shared code (`connector.c`, the voice dial path, TLS/socket or task priorities)
   and fix it for all boards. Add a host test if the logic allows it. Write the hardware steps and
   expected log lines so Claude can verify it on the T-Circle and the StickS3.
7. **Checks.** All host suites (C under ASan/UBSan, Python in `tools/.venv`), all four boards build.
   Generated configs and image sizes are unchanged except for the URL defaults and the bug fix; show
   the diff. Run `export_public.sh` into a temporary directory, then run `check_public.py` on it
   with zero hits. In the exported tree, follow the README quick start for one board (build only) to
   prove it is self-contained.

## Rules

- Do not flash any board; Claude tests on hardware.
- No change to the Connector protocol, BLE contract, app or backend.
- Do not delete `docs/prompts/` or `docs/reports/` from this repository, and do not rewrite history.
- Do not commit Wi-Fi passwords, tokens, test certificates, generated sdkconfig or build outputs.
- Two commits on `codex/open-source-prep`: (1) open-source preparation, (2) the Connector fix. Don't
  merge, don't push.

## Report

Write `docs/reports/H5-report.md` (internal): excluded paths, leak-check rules and the allow-list,
what was changed in public files, URL sources, the identifier list with recommendations, third-party
inventory, the Connector bug's cause and fix, test counts, and the hardware steps for the fix.
Reply with both commit hashes and a short summary.

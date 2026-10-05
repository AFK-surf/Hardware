# B1b · Finish BLE provisioning: test on the board, then merge into main

The B1 session closed before its last step. Pick it up.

Read first:
- `~/Desktop/AFK/Hardware/docs/connector-plan.md` (in Chinese): plan and shared constraints
- In the B1 worktree `~/Desktop/AFK/Hardware-b1` (branch `codex/ble-provisioning`):
  - `docs/ble-provisioning.md`: the GATT contract, already updated so credentials carry a required `connect_url`
  - `docs/reports/B1-report.md`
  - `git log --oneline -4`

## Where things stand

- Branch `codex/ble-provisioning` is rebased onto main's A2 commit (`5c4a59d`). It has three commits:
  1. `22582e8`: BLE provisioning; credentials go in NVS instead of `secrets.h`.
  2. `66a02a6`: the B1 report.
  3. `3da1b2f`: wiring. A2's `connector_credentials()` reads `device_config_get()`; the Connector starts once credentials are saved and reconnects with new ones after re-pairing; status reports `connector_connecting` / `connector_up` / `connector_failed`; `secrets.h` applies only with `COMMA_DEV_SECRETS 1`.
- It builds, but it has never been flashed with a real connector token, and it isn't merged.
- Since then, main gained only docs commits (`docs/prompts/*`), which won't conflict.
- The board (`/dev/cu.usbmodem1101`, MAC `98:a3:16:f5:5d:28`) currently runs A2's firmware from main. Check that the port is there; if not, ask the user to plug it in.

## Credentials for the test (staging)

- Connector: `COMMA_CONNECTOR_TOKEN=salix_conn_REDACTED`, `--connect-url wss://salix-staging.comma.surf/v1/connect`. This is the existing device `dev_M3Z6mxvKhQSYEwyYTber-g` ("T-Circle", scope `local_file_read`). Nothing else may connect with this token: two connections kick each other off.
- Voice key, group and office Wi-Fi: read them from `~/Desktop/AFK/Hardware/t-circle-s3/main/secrets.h` (gitignored) into environment variables. Never print them or commit them.
  - Wi-Fi: SSID `ToEverything-IOT`, 2.4 GHz.
  - Group: `grp1_2089252240962359296_2089252240962359297`.
  - Workspace: `wsp_X7wQDptcSaoxwp1TbjoYaakX`.
- Tool: `t-circle-s3/tools/ble_provision.py provision ...`. Secrets go in through `COMMA_VOICE_KEY`, `COMMA_CONNECTOR_TOKEN` and `COMMA_WIFI_PASSWORD`. On macOS a system dialog asks for the 6-digit code shown on the round screen; the user types it.

## Do

1. Ask the user before flashing; the board is shared. Build and flash from the B1 worktree (IDF 6.0.2: `. ~/.espressif/tools/activate_idf_v6.0.2.sh`). Erase NVS first so the test starts from factory state (`idf.py erase-flash` or erase just the NVS partition).
2. Provision over BLE with the credentials above. Verify:
   - BLE `status` goes `saved` → `connector_connecting` → `connector_up` with `device_id` = `dev_M3Z6mxvKhQSYEwyYTber-g`;
   - the user sees T-Circle online in Comma Settings › Devices;
   - one tap-to-call works while the Connector is up (ask the user to call for ~1 minute; check `cue:` stats);
   - reboot (unplug and replug) → Wi-Fi, Connector and voice all come back with no BLE;
   - re-pair: enter pairing mode (hold BOOT 3–10 s, release), provision again with the same values, and the Connector reconnects without a reboot.
3. Measure internal free heap (idle, Connector up, and during the call). A2 measured a lowest of 23 KB during a call. Report whether BLE code changed that, and confirm BLE is fully off after provisioning.
4. If everything passes:
   - rebase onto current main;
   - fast-forward main to the branch (`git -C ~/Desktop/AFK/Hardware merge --ff-only codex/ble-provisioning`; the main checkout must have no uncommitted changes, so check first and ask if it isn't clean);
   - update `docs/connector-plan.md` (B1 done; `secrets.h` is dev-only now).
   
   Don't push.
5. If something fails, fix it on the branch, re-test, and report. Don't merge a failing build.

## Don't

- Don't commit or print secrets. `secrets.h` stays gitignored.
- Don't start another Connector with this token (no `mini_connector.py`, no Mac `salix-connect`).
- Don't touch the user's Mac Comma Staging connector. It has an unrelated backend issue that's being investigated.

## Report

Append a "B1b: on-board test and merge" section to `docs/reports/B1-report.md`. Include the results of step 2, heap numbers, the merge commit, and anything A3 must know.

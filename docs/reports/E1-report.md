# E1 · Firmware setup state in BLE advertising

2026-10-07 · Hardware `main` · ESP-IDF **v6.0.2** · ESP32-S3

## Result

Implementation, build and offline checks are complete. **Board and Android acceptance tests are
pending**: `/dev/cu.usbmodem1101` was absent at the time of testing. No firmware was flashed and
no factory reset was performed. Flash/reset approval was requested, with no answer yet.

| Check | Result |
| --- | --- |
| Original advertising flags and complete service UUID | Unchanged; 21 bytes |
| Scan response name plus manufacturer-specific data | Implemented; 21 of 31 bytes, including both AD headers |
| Payload contract | `FF FF 01 01 <state> <MAC[4]> <MAC[5]>`; state uses `device_config_provisioned()` |
| Name and short ID | Both use the same Wi-Fi STA MAC read at provisioning start; suffix bytes are not reversed |
| Length check | Compile-time assertion bounds even the longest name the existing buffer can hold to 31 bytes |
| Logging | One line containing all 7 manufacturer payload bytes per successful advertising start |
| Scan client | Prints name, state, short ID, address and RSSI without connecting or pairing |
| ESP-IDF build | **Passed**, v6.0.2, normal project configuration |
| Offline scanner tests | **9 passed** |
| Live macOS scan, 8 seconds | Command succeeded, output `none`; no advertising Comma device observed |
| Unprovisioned board → `0x00` | **Skipped**; no factory-reset consent and no connected board |
| Provisioned board in pairing mode → `0x01` | **Pending**; board absent, flash approval pending |
| B2 Android Add device finds and sets up device | **Pending user test**; not claimed from the build or offline tests |

## Changes

- `t-circle-s3/main/provisioning.c`: capture the two MAC suffix bytes alongside `Comma-XXXX`, add
  the 7-byte manufacturer payload to `advertise()`'s scan response and log it after advertising starts.
  The GATT service, pairing/security behavior and advertising lifecycle are unchanged.
- `docs/ble-provisioning.md`: document offsets, byte order, test-only company ID, length accounting,
  unknown-version handling and scan command. `0xFFFF` must be replaced before production.
- `t-circle-s3/tools/ble_provision.py`: extend the existing `scan` command with active UUID-filtered
  scanning and setup-data decoding. Preserve data across later reports that omit it. Missing or
  truncated data and unsupported versions/models are shown as unknown, not as unprovisioned.
  An unknown state byte is printed explicitly as unknown.
- `t-circle-s3/tools/test_ble_provision.py`: offline regression checks for both states, suffix byte
  order and leading zeroes, invalid/unsupported payloads, delayed scan responses, UUID/company
  filtering, replacement of stale metadata and an empty scan. The scan tests verify that no
  `BleakClient` is created.

The name and manufacturer data can arrive after the first scan callback, as described in the
[Bleak scanner API](https://bleak.readthedocs.io/en/latest/api/scanner.html). IDF 6.0.2's local
`ble_gap_adv_rsp_set_fields()` implementation was also checked: it serializes the fields into a
local buffer synchronously, so the stack-local manufacturer array is valid for this call.

## Verification performed

From the repository root:

```sh
. ~/.espressif/tools/activate_idf_v6.0.2.sh
cd t-circle-s3
idf.py --version
idf.py build
```

The build passed. `build/cue_logo.bin` is **2,009,808 bytes** (`0x1eaad0`); the 4 MiB app partition
has 52% free. Local build log: `t-circle-s3/build/log/idf_py_stdout_output_65902` (gitignored).
Artifact SHA-256: `dba59644823e2f2ca68b945d96d47e97b8fd8b7b3fff06c3f257610ab1a6c89b`.
This pre-commit build identifies itself as `9f47e0c-dirty`.

From the repository root, using the existing ignored virtual environment (Bleak **3.0.2** installed):

```sh
PYTHONDONTWRITEBYTECODE=1 tools/.venv/bin/python -m unittest discover \
  -s t-circle-s3/tools -p 'test_ble_provision.py' -v
tools/.venv/bin/python t-circle-s3/tools/ble_provision.py scan --timeout 8
git diff --check
```

All 9 tests passed; the real scan completed with `none`. This checks that the scanner runs on the
host, not the bytes emitted by the board. `git diff --check` passed. The first sandboxed build was
blocked by IDF's `psutil`/`sysctl` process query, and sandboxed scanning reported unsupported
Bluetooth; both succeeded when rerun with approved host access.

## Remaining hardware acceptance

The task explicitly requires approval before flashing and separate consent before factory reset.
Connect the board and confirm its port, then run `lsof /dev/cu.usbmodem1101` immediately before
flashing. The earlier `lsof` attempt returned “No such file or directory”; that is not evidence
that a connected serial port is free. Do not stop another session's logger without agreement.

Once approved, use an app-only flash to preserve NVS and the existing partition/model images:

```sh
cd t-circle-s3
. ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py -p /dev/cu.usbmodem1101 app-flash
```

1. On a provisioned board, hold BOOT for 3 seconds and release before 10 seconds. Run the scan
   client. For the documented `Comma-5D28` board, expect
   `state=provisioned-pairing(0x01) short_id=5D28` and serial log payload
   `FF FF 01 01 01 5D 28`.
2. **Only with explicit factory-reset consent**, reset the board and scan again. Expect
   `state=unprovisioned(0x00) short_id=5D28` and `FF FF 01 01 00 5D 28`.
   These are expected values, not observations from this run.
3. The user runs Android B2 **Add device**, verifies discovery and completes setup. Record the
   phone result here; neither a scan-only host test nor unchanged GATT code proves this flow passes.

The E1 commit includes the four implementation/doc/test files listed above and this report.
The pre-existing edit to
`docs/prompts/E3-backend-nearby-device-offer.md` was left untouched and is not part of the E1 commit.
No push is requested; the repository has no remote.

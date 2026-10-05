# Hardware

One folder per device. Each folder is a standalone ESP-IDF project: build and flash from inside it.

| Folder | Board | What it runs |
| --- | --- | --- |
| `t-circle-s3/` | LilyGO T-Circle-S3 (ESP32-S3, 16 MB flash, 8 MB PSRAM) | Cue logo + `cue.voice.v1` voice calls |

```sh
. ~/.espressif/tools/activate_idf_v6.0.2.sh
cd t-circle-s3
idf.py build flash monitor -p /dev/cu.usbmodem2101
```

The T-Circle has no compiled-in secrets: the Comma app sets it up over BLE (`docs/ble-provisioning.md`;
test client `t-circle-s3/tools/ble_provision.py`). Hold BOOT 3 s for pairing mode, 10 s to factory reset.
For a dev board, `main/secrets.h` with `COMMA_DEV_SECRETS 1` overrides that (gitignored; copy from
`secrets.h.example`).

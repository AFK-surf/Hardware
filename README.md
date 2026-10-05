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

Secrets live in each device's `main/secrets.h` (gitignored; copy from `secrets.h.example`).

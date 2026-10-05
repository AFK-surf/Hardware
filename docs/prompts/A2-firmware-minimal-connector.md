# A2 · Firmware: minimal Connector on the T-Circle

Read first:
- `~/Desktop/AFK/Hardware/docs/connector-plan.md` (in Chinese): plan and shared constraints
- `~/Desktop/AFK/Hardware/docs/connector-protocol.md`: written by A1, the wire contract to implement
- `~/Desktop/AFK/Hardware/docs/reports/A1-report.md`

## Goal

The T-Circle (`~/Desktop/AFK/Hardware/t-circle-s3`, ESP-IDF 6.0.2) holds a Connector connection to the backend next to its existing voice call. It shows up as a device in Comma's Settings › Devices. This step adds no LAN operations yet.

## Context

- The voice client is `main/cue_voice.c`: `esp_websocket_client`, TLS with the cert bundle, a separate TX lock (`CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y`), and one sender task. Read the header comments and the two latest commits (`git log -2`) to see why each piece is there. They record hard-won fixes for 4410/4400 drops on lossy Wi-Fi.
- Secrets live in `main/secrets.h` (gitignored; template `main/secrets.h.example`). Add `CUE_CONNECTOR_SERVER` and `CUE_CONNECTOR_TOKEN` to the template. The user mints the staging token, or you mint it with their OK. B1 will move this into BLE-provisioned NVS later. Keep the access behind one small function so that swap is easy.
- The board has 8 MB PSRAM. Two TLS sessions fit. Allocate large buffers in PSRAM, as the voice code does.

## Do

1. Add `main/connector.c/.h`, a task started after Wi-Fi gets an IP. It must:
   - dial `/v1/connect` per the spec;
   - answer heartbeats;
   - send `metadata` (capabilities: none yet, or only what A1 says is mandatory);
   - answer every `request` with `error` (unsupported) for now;
   - reconnect with exponential backoff (1 s → 60 s, with jitter);
   - never block or disturb the voice call (separate task and client; nothing shared but Wi-Fi).
2. Log one line per state change and a stats line every 30 s (connected / last heartbeat age / reconnects).
3. Build, then ask the user before flashing. Only one board exists, and the user may be testing calls on it.
4. Verify on staging:
   - the device appears in Settings › Devices;
   - it survives a Wi-Fi drop and a backend reconnect;
   - a voice call made while the Connector is up still works (place one call and check `cue:` stats for drops, 4410 or 4400).

## Don't

- Don't change voice behaviour. If the Connector needs a shared change (for example a Wi-Fi event), keep it minimal and explain why.
- Don't commit `secrets.h`.

## Report

Write `~/Desktop/AFK/Hardware/docs/reports/A2-report.md` with the result, the heap (internal and PSRAM) before and after, and serial log excerpts. Commit with the git identity from the plan. Don't push unless asked.

# A3 · Firmware: `http_request` + access gate (pairs with backend C2)

Read first: `~/Desktop/AFK/Hardware/docs/connector-plan.md`, `docs/connector-protocol.md`, and the A1, A2 and B1 reports (B1 includes the B1b section).

## Goal

The cloud agent can reach a LAN HTTP service through the T-Circle. For example, it reads `http://<a LAN device>/` on the office network and gets the response back. The user's permission toggle is respected.

## Before you start

- B1 is merged (main `eef83d1`): credentials come from BLE provisioning (NVS); `secrets.h` applies only with `COMMA_DEV_SECRETS 1`. The board is provisioned with staging credentials and its Connector is online.
- **Free internal RAM first. This is now critical.** Internal free heap after B1 (`docs/reports/B1-report.md`, section B1b; it lists candidate fixes):
  - idle, BLE off, Connector up: 51.5 KB;
  - **lowest during a call: 14.4 KB**;
  - **BLE on while the Connector connects: lowest 3.1 KB**; pairing mode with the Connector up: 2.8 KB.

  Logo stack and Connector buffers already moved to PSRAM (`08dfe19`). An HTTP client and a worker task won't fit. Before adding anything:
  - move task stacks that don't need internal RAM to PSRAM (`xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`): logo, ui, connector, and the new worker;
  - check where the Wi-Fi/lwIP and mbedTLS buffers live (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, dynamic buffers);
  - consider NimBLE memory settings (host/controller buffers in PSRAM, fewer connections, freeing the BLE controller memory once provisioned if it's never needed again before a reset).

  Targets: lowest internal free ≥ 40 KB during a call with the Connector up and one LAN request in flight, and ≥ 20 KB while BLE is on. Measure before and after, and don't regress the voice fixes (`git log` for 81297a7).

## Do

1. Implement `http_request` in `main/connector.c` with the same params and result shape as the Go reference (`~/Desktop/AFK/Cue/systems/connector/salix-connect/main.go:4283-4362`):
   - methods GET, POST, PUT, PATCH, DELETE; http (and https if `esp_http_client` with the cert bundle handles it);
   - headers and body; `timeout_seconds` (cap at 30 on the device); `max_bytes` (cap at 64 KB on the device);
   - no redirects.

   Use `esp_http_client`. Run requests on a worker task so the socket task keeps answering heartbeats.
2. **Destination allowlist:** RFC1918, link-local and the device's own subnet only. Refuse loopback, the device itself and public IPs. The cloud already reaches the internet; this device exists for the LAN. Return a clear `error`.
3. **Access gate:** implement `device_access` like `device_access.go:143`. Read-only by default: until the user turns on operations in Settings › Devices, refuse every LAN request. Persist the state in NVS.
4. Advertise `http_request` in `metadata`.
5. Testing needs backend C2's agent tool, or a temporary call through `SalixEnv.Connector.Live.request/3` on a local stack. Check the C2 status first.
6. Build. Ask the user before flashing. Verify on staging: with access off the request is refused; with access on, the agent fetches a LAN URL through the T-Circle. Check that a voice call still works while a request runs.

## Report

Write `~/Desktop/AFK/Hardware/docs/reports/A3-report.md` and commit (don't push).

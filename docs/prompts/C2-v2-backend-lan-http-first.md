# C2 v2 · Backend: let the agent reach LAN devices over HTTP through a gateway device

For the Comma backend team (repo AFK-surf/Cue, `systems/`). This replaces the first C2 request: HTTP first; UDP, crypto codecs and a secret store come later.

## Background

The T-Circle (ESP32-S3) runs on staging as a Connector at the user's home or office. Firmware is done and verified: it dials `/v1/connect`, starts read-only, honours `device_access` (Android's "Allow operations" toggle), and implements `http_request` with the same params and result as `salix-connect` (`systems/connector/salix-connect/main.go:4283-4362`). It refuses everything except private, link-local and own-subnet destinations, caps `timeout_seconds` at 30 and `max_bytes` at 64 KB, and advertises:

```json
"capabilities": {"lan_gateway": {"methods": ["http_request"], "max_bytes": 65536, "max_timeout_seconds": 30}}
```

End to end it works today only through a workaround: an agent-created device-placed remote MCP (`salix_mcp/connection.ex:972`) on staging. The agent reached a LAN service through the board, and the LAN server saw requests from the board's IP. The test reports and protocol notes are in `~/Desktop/AFK/Hardware/docs/` (`connector-protocol.md`, `reports/A1-report.md`, `reports/A3-report.md`).

We're moving to LAN devices with open local HTTP APIs and no cloud tokens: WLED light controllers (`GET/POST /json/state`), Tasmota plugs (`/cm?cmnd=…`), Shelly, ESPHome. The agent discovers them and works out the protocol itself, so phase 1 needs no codecs and no secret store.

## Phase 1 (please do now)

1. **Agent tool `device.lan_request`**, HTTP only:
   - Args: `device_id`, `method`, `url`, `headers?`, `body?`, `timeout_seconds?`, `max_bytes?`.
   - Dispatches the existing connector method `http_request` through `EnvDispatch.request/4` (pattern: `salix_agent/tools/dependency_installations.ex`).
   - Server-side checks:
     - (a) the device advertises `capabilities.lan_gateway` and is online;
     - (b) the destination is private or link-local, otherwise refuse and point to `web.http_request`;
     - (c) **the device's `allows_operations` is true**; otherwise return a clear "turn on Allow operations in Settings › Devices" error. Today only the device enforces this (A1 finding). Add the server check as defence in depth.
   - Register it in `tools.ex` (append, cache order), add IFC classification, add it to the `environment-control` plugin, and mark it write-safety.
   - Also make `salix-connect` (Mac/Linux) advertise `lan_gateway`, so a computer Connector can be a gateway too.
   - Tests: validation, dispatch, error mapping (offline, read-only, refused by the device, timeout).
2. **Skill `home-gateway`** (built-in, `resources/salix-system-files/skills/`):
   - find a gateway with `device.list` (capability `lan_gateway`);
   - prefer it for anything on the user's LAN;
   - read state back after every change;
   - if the gateway is offline or read-only, say so and stop.
   
   Also add short device skills for **WLED** and **Tasmota**: identify, read state, set state, read back. Muse's open-source skills (`~/Desktop/AFK/muse-gadget-sdk/skills/`) are a model for tone and safety rules.
3. **Fixes found during device testing:**
   - Deleting a device doesn't revoke its connector token. Revoke it on delete. (#2510 fixed the registration TTL.)
   - Please revoke these by hand on staging: the connector tokens of the old T-Circle devices `dev_M3Z6mxvKhQSYEwyYTber-g`, `dev_DYYh…`, `dev_yAA_sfKRFE_vb1d_f_70WA`, `dev_UJrF…`, `dev_EaauLB…`, `dev_PPsW5YUvPh80-q0Z8I8YlA`, `dev_KokfkxljPn6yuBAjq3ucJA` (all superseded), plus the test voice key used in the firmware's dev `secrets.h` (ask the user for its key id).
   - `dev_C4_DhwzQA9UxutZB5IJ8pA` was online but missing from `GET …/devices` as the Android app shows it. Please check whether it landed in another group or workspace (it was provisioned by a Mac script, not the app).
   - The same Mac appears as several device records (3× "mafuzhens-MacBook-Pro.local" + "My Mac"). Consider reusing or cleaning up records when a computer re-registers.

## Phase 1.5: agree the discovery contract now (firmware will implement it)

Connector method `lan_discover`:
- params: `{sources?: ["arp","mdns","ssdp"], timeout_ms?: 1000..8000}`
- result: `{hosts: [{ip, mac?, hostname?, services: [{type: "_wled._tcp", name, port, txt?}], ssdp?: {server, location, st}}], truncated: bool}`

Strings come from other LAN hosts, so the device strips control characters. Add a tool `device.lan_discover` with the same gating as `lan_request` (lan_gateway + operations allowed). Please confirm or adjust this shape before the firmware builds it.

## Later (phase 2, not now)

`udp_request`, crypto codecs (miIO, Gree, Tuya local) and a per-group secret store for device keys.

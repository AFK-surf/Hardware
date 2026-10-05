# A1 · Connector protocol spike (host script, no firmware)

Read `~/Desktop/AFK/Hardware/docs/connector-plan.md` first (in Chinese). It has the goal, the architecture and the shared constraints.

## Goal

Learn exactly what the Comma backend requires from a Connector, by building the smallest one that works. Build it as a Python script on the Mac, not on the ESP32. The ESP32 firmware (A2) will copy whatever this proves.

## Context

- Backend repo: `~/Desktop/AFK/Cue` (AFK-surf/Cue). Server side:
  - `systems/apps/salix_web/lib/salix_web/connect.ex` (upgrade)
  - `systems/apps/salix_web/lib/salix_web/connector_socket.ex` (`connected` around :197, `handle_message` for `metadata` :218, `heartbeat` :232, `request` :238+; heartbeat 15 s, timeout 45 s, :2208-2214)
  - `systems/apps/salix_env/lib/salix_env/protocol.ex` (envelope, per-method timeouts)
  - `systems/apps/salix_env/lib/salix_env/connector_tokens.ex`
- Reference client (Go): `systems/connector/salix-connect/main.go`
  - dial: :1561-1583, :1685-1710
  - envelope: :454-480
  - read loop: :1640-1677
  - method switch: :4024-4160
  - `http_request`: :4283-4362
  - access gate: :4040-4054, `device_access.go:143`
  - metadata: :3039-3070
- Token minting: `POST /v1/comma/workspaces/:workspace_id/connector-token` (`systems/apps/comma_web/lib/comma_web/router.ex:2046, 4689-4712`). A group token with no `expires_in_seconds` does not expire.

## Do

1. Write `~/Desktop/AFK/Hardware/tools/mini_connector.py` (stdlib + `websockets` only). It must:
   - dial `/v1/connect` with the query params and headers the server requires;
   - handle `connected`, answer `heartbeat`, and send a `metadata` frame that advertises only `http_request`;
   - implement `http_request` (same params and result shape as the Go one) and `device_access`;
   - answer any other method with a proper `error`.
2. Run it against a **local** dev stack (`make dev` in `~/Desktop/AFK/Cue`). Known local-dev pitfalls:
   - Docker Hub dropped `minio/minio`; use `quay.io/minio/...` (same digest).
   - The devcontainer elan dir may need `chown`.
   - Fresh worktrees lack the Electron binary.

   Mint a token for the local workspace, then confirm:
   - the device shows up in `GET /v1/comma/workspaces/:ws/devices` (and in Settings › Devices in the web/Electron app);
   - an agent turn can make the device run `http_request` to a LAN or localhost URL, through whatever path exists today (device-placed MCP, or a temporary test call through `SalixEnv.Connector.Live.request/3` from IEx);
   - reconnect works after you kill the script.
3. Answer these questions with file:line evidence:
   - Which handshake fields are mandatory, and which are optional (`X-Salix-Connector-Instance-ID`, `scope`, `os`/`arch`, `system_info`, `capabilities`)?
   - Does the server enforce anything about access, or is the gate purely client-side?
   - What does a minimal `metadata` look like? What happens with empty `skills`?
   - Max frame sizes, timeouts per method, and what the server does with an unknown method's `error`.
   - **Cloud compute:** when the user's Mac Connector is off, can the Router agent run Python (for example `python-miio` to build encrypted miIO packets) anywhere in the cloud: a managed runtime, a cloud VM environment, or a sandbox? Name the tool and environment, or say it does not exist.
4. Write the protocol spec to `~/Desktop/AFK/Hardware/docs/connector-protocol.md`. Keep it short, with exact JSON examples for each frame. A2 implements the firmware from this spec.

## Don't

- Don't use staging or the user's real workspace. Local only.
- Don't change backend code. If something blocks you, report it.
- Don't touch the ESP32 board.

## Report

Write `~/Desktop/AFK/Hardware/docs/reports/A1-report.md` covering: what works, the answers to step 3, and anything the firmware must do that the plan missed.

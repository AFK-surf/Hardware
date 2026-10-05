# A1 report: Connector protocol spike

2026-10-05 · `tools/mini_connector.py` (stdlib + `websockets`, venv in `tools/.venv`) · spec: `docs/connector-protocol.md`

## What works (all verified on a local stack)

The local stack was the dev containers already running from `~/AFK-workspace/Cue-latest` (main `fa933f164`, API on :4200 with the pre-rename `/v1/workspaces/...` paths, Salix on :4000). The `/v1/connect` code is the same as current `main` `d52347f1e`, apart from an unrelated archive-repair clause. Workspace "Cue Local Dev" (`wsp_MsojvU9zcX_V7zcUp24y66KQ`), login `cue-local@example.com` (code from Mailpit :8025). No backend code was changed, and nothing touched staging or the board.

| Check | Result |
|---|---|
| Mint token (`POST /v1/workspaces/:ws/connector-token`) | 201, `salix_conn_…`, `device_id dev_Pc8wBuEUfP5LT55XBx3qBw`, `expires_at: null`, `connect_url ws://127.0.0.1:4000/v1/connect` |
| Shows in `GET …/devices/:id` | `status: connected`, name taken from `system_info.hostname` ("T-Circle (mini)"), environment `permission_required` until access is allowed |
| Settings toggle (`PUT …/devices/:id/access`) | Server sends `device_access` to the device; device saves state, republishes metadata, replies → `allows_operations: true`, environment loses `requires_permission` |
| Agent → device `http_request` → LAN | Registered a **device-placed remote MCP** (`lan-lamp`, URL `http://127.0.0.1:8765/mcp` on the Mac, i.e. the device's LAN). Salix discovery ran `initialize`, `notifications/initialized` and `tools/list` as three `http_request`s through the device. Then the Router agent session tool route `POST /v1/runtime/agents/:agent/sessions/:sid/tools/mcp.lan-lamp.lamp_status` returned `{"result":"lamp is on (LAN time 16:13:27)"}`. This is the same code path a model tool call takes. |
| Gate | With access turned off, the same tool call reached the device and the device refused it. The agent received `method "http_request" is not permitted for a local_file_read connector`. |
| Reconnect | `kill -9` → device `disconnected` immediately, tool call → `device_offline`. Restart → `connected` in ~1 s, access preserved, tool call works again (MCP re-initializes over the new run). |
| Heartbeat timeout | When the device doesn't reply to heartbeats, the server closes at exactly 45.0 s (code 1011). The script reconnected by itself. |

Not done: a real **model-chosen** tool call. The local LLM is `llm-mock`, so I drove the same tool executor through the session tool route instead of a chat turn. The Settings UI wasn't opened; I used the same REST endpoints it calls.

## Answers to step 3

### Handshake fields

- **Mandatory:** `Authorization` header with the connector token. The bearer is parsed at `salix_web/auth.ex:211`; without a connector token you get 403 `connector credential is required` (`connect.ex:196`). Bare token without `Bearer ` also works (verified). A query-string token → 401 (verified).
- **`name`/`alias`:** optional, default to the token's values (`connect.ex:169`). Connecting with no `name` worked (verified). The "name is required" branch (`connect.ex:52`) is unreachable with a token.
- **`os`/`arch`:** optional, default `""` (`connect.ex:87`).
- **`scope`:** optional to the parser (`connect.ex:243-249`; only `""`/`local_file_read`, else 400, verified) but **mandatory in practice**. Registration stamps it with the connection generation (`salix_env/registry.ex:1264`). The routing view (`salix_env/control.ex:1128-1142`) treats a previously reported scope with an old generation as `:unavailable`. Verified: reconnecting without `scope` and without metadata gave the device **zero environments**. Send it on every dial (salix-connect always does, `main.go:1707`).
- **`X-Salix-Connector-Instance-ID`:** optional (`connect.ex:218`, empty → not passed). Connected fine without it (verified). Use a per-boot UUID anyway, to match salix-connect (`main.go:1570`).
- **`system_info`:** optional (`connector_socket.ex:2583`, stored only when non-empty). Only the keys in `control.ex:20` are shown publicly. `hostname` is the display name (`comma/devices.ex:164`).
- **`capabilities`:** optional. They are flags, not a method list. The projection keeps only `control.ex:16`'s keys, and `scope` drives routing (`connector_socket.ex:2243`). There is no way to "advertise only `http_request`", so the device must reject everything else itself.
- **`group_id`:** forbidden, 400 (`connect.ex:165`, verified).

### Access: server or client?

Both, but for T-Circle's path effectively **client-side**:

- Server-side enforcement exists only for (a) tokens minted with `scope=local_file_read`, which may only run `read_ref` (`connector_socket.ex:667`), and (b) `EnvDispatch` routes (`env.exec` etc.), which refuse a `permission_required` environment (`env_dispatch.ex:590`). That state comes from the device's **self-reported** scope (`control.ex:1145`).
- `Comma.Devices.set_access` just forwards `device_access` to the device and checks the echoed boolean (`comma/devices.ex:82`). Nothing is stored server-side.
- The device-placed MCP path calls `ConnectorLive.request(…, "http_request", …)` directly (`salix_mcp/connection.ex:972`) with **no permission check**. Verified: with access off the request still reached the device.
- So the firmware gate is the only gate on the MCP path, and the device must report its own scope honestly. The future `device.lan_request` tool (C2) should go through `EnvDispatch` so the server also refuses read-only devices.

### Minimal `metadata`; empty `skills`

`{"type":"metadata","capabilities":{"scope":""}}` is the functional minimum. Add `system_info.hostname` for a readable name and `device_access: true` to mirror salix-connect. The full recommended frame is in the spec, §3. `skills` is stored (`connector_socket.ex:2224`) and echoed in `env.list`/the dashboard (`control.ex:972`), but nothing in `salix_agent` reads a device's skills. `[]` (what salix-connect sends, `main.go:3069`) or omitting it changes nothing; verified, the device works with `[]`. The metadata frame itself is optional too: the dial `scope` alone gives a working environment (verified). It is still needed after `device_access` changes.

### Frame sizes, timeouts, unknown-method errors

- Max frame 16 MiB server side (`connect.ex:32`); salix-connect reads 16 MiB (`main.go:62`) and caps HTTP bodies at 10 MiB (`main.go:43`). The MCP path asks for `max_bytes` 10 MiB (`salix_mcp/connection.ex:967`), so the firmware must clamp to its own buffer and set `truncated`.
- Per-method timeouts are in `salix_env/protocol.ex:30-71`: `http_request` 120 s (`:49`), default 30 s (`:76`, applies to `device_access`). `timeout_seconds` raises `http_request` to `n*1000+5000` (`:113`), but the socket's pending timer is `min(120 s, that)` (`connector_socket.ex:712-723`). In effect ≤120 s. Max 256 pending per socket (`:706`).
- Heartbeat: server sends every 15 s (`:2211`) and drops the socket ≥45 s after the last **device heartbeat frame** (`:1922`, `:2214`, `:232`); other frames don't refresh it.
- Unknown method: the device's `error` frame goes through `complete_pending` → `Protocol.outcome` (`connector_socket.ex:399`, `protocol.ex:233`) and comes back to the caller as `{:error, "<string>"}`. No retry, no disconnect. Through MCP the agent sees it verbatim (verified). Late replies are dropped; binary frames, bad JSON and unknown `type`s are ignored (`connector_socket.ex:216`, `:403`). A `result` that isn't an object is wrapped as `{"value": …}` (`protocol.ex:237`).

### Cloud compute for miIO (Mac Connector off)

There is **no always-available Python** for the Router agent:

| Path | Python? | Router can use it? |
|---|---|---|
| **Group Cloud VM** via `env.exec` (`salix_agent/tools/peers.ex:81`) | **Yes.** Cloudflare Sandbox image with Python 3.13 + pip (`cloudflare/salix-vm-gateway/Dockerfile:7,45`) | Only if the workspace VM is enabled. Default `{"enabled": false}` (`comma/workspaces.ex:74`); only an **admin** can turn it on (`PUT /v1/comma/admin/users/:id/workspaces/:ws/vm`, `comma_web/router.ex:1096`, `comma/admin.ex:158`). Not available locally and not checked on staging. Adds VM cold start and cost to every "turn off the light". |
| `compute.exec` / `process.*` (Compute Workload, agent-vmm) | Yes (a full workload) | No. Needs a workload grant in the tool context (`salix_agent/tools/compute.ex:57`), which is for Workers on a Compute Workload. |
| `script.run` / `script.run_file` (spinfoam) | **No.** Integer-only C → eBPF, no libc, 4 KiB stack, 5 s (`salix_agent/tools.ex:40`) | Yes, but writing AES-128 + MD5 in that is impractical. |
| `web.http_request` | n/a | Public hosts only; can't reach the LAN. |

Recommendation (the plan's default decision 2 says to report and decide): don't rely on Python. miIO is just MD5 + AES-128-CBC, which Elixir's `:crypto` has. Put it in the C2/C3 backend tool, for example a `miio` encoding option on `device.lan_request`, or a dedicated `device.miio_send` that builds the packet server-side and has T-Circle send it over UDP. The alternative is mbedTLS on the ESP32, but that puts device tokens on the board.

## What the firmware must do that the plan missed

1. **Use `connect_url` from the token response.** Connector traffic goes to the Salix host, not the Comma API host. B2 must write `connect_url` (not just "server") into the device.
2. **Always send `scope`** (dial query + metadata), or the device silently loses its environment after the first connection.
3. **Re-send metadata after `device_access`.** The server only learns the new state from metadata. Persist the state in NVS (salix-connect persists `device-access.json`); default is read-only.
4. **Reply to every server heartbeat.** Only device `heartbeat` frames keep the socket alive (45 s). The plan only said "断线重连" (reconnect on disconnect).
5. **The device check is the only gate on the MCP path.** A3's `device_access` gate is the security boundary for now, not defence in depth.
6. **Answer every request.** There's no method advertisement, so unknown methods must get an `error` frame or the caller hangs for 30-120 s.
7. **Clamp body sizes yourself.** The server asks for up to 10 MiB. The firmware needs a hard cap with `truncated: true`, and must survive large inbound frames (reject without crashing).
8. **No redirects, default method POST, headers as arrays.** Matching the Go result shape exactly keeps `salix_mcp` happy: it reads `status`, `body`, `headers`.
9. **TLS on staging/prod.** `wss://` needs the ESP-IDF certificate bundle.
10. **Device-placed MCP is chatty.** Each new connection re-runs `initialize` + `notifications/initialized` before a tool call, so one "关灯" costs 2-3 LAN round trips over the socket. C2's direct `device.lan_request` avoids that, and is also where a server-side permission check belongs.
11. **One token = one device record.** Re-flashing with the same token reuses the device and its old metadata (name, scope). Revoking (B3) is the only way to get a clean record.

## Leftovers on the local stack

- Device `dev_Pc8wBuEUfP5LT55XBx3qBw` ("T-Circle (mini)") and its connector token stay in the Cue Local Dev workspace so the script can be re-run. The token is in my session scratchpad only; mint a new one to reuse.
- MCP definition `mcp1_2107021091720396800` / binding `mpb1_2107021120367493120` (`lan-lamp`) are **disabled**, not deleted (Salix has no delete route for either). The temporary tenant API key `a1-spike` was revoked (old key now gets 401).
- The session tool calls ran against the Router's existing session `ses1_2099704158381670401` (it's retired; a fork was refused), so that local session may have extra tool-call records.

## Re-run

```
cd ~/Desktop/AFK/Hardware/tools
python3 -m venv .venv && .venv/bin/pip install websockets   # once
SALIX_CONNECTOR_TOKEN=salix_conn_… NO_PROXY='*' .venv/bin/python mini_connector.py \
  --server ws://127.0.0.1:4000 --name t-circle --alias tcircle
```
Probe flags: `--no-scope-param --no-metadata --no-instance-id --no-os-arch --no-system-info --no-heartbeat-reply --skills '<json>'`.

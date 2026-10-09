# Comma Connector protocol: the subset T-Circle implements

2026-10-05 · verified by `tools/mini_connector.py` against a local Salix (A1). Evidence and reasoning are in `reports/A1-report.md`.
Line numbers refer to Cue `main` at `d52347f1e`, under `systems/`.

## 1. Credential

A connector token (`salix_conn_…`) is minted per device:

```
POST /v1/comma/workspaces/:ws/connector-token      (user session, full workspace session)
{"name": "Comma-5D28"}      (the Android app: the device_name it then writes over BLE)
→ 201 {"token": "salix_conn_…", "device_id": "dev_…", "connector_id": "conn_…",
        "connect_url": "wss://<salix-host>/v1/connect", "server": "wss://<salix-host>",
        "credential_generation": 1, "expires_at": null, …}
```

- Store `token` and `connect_url` in NVS. **Use `connect_url` as given.** It is the Salix host, not the Comma API host (locally `ws://127.0.0.1:4000`, not `:4200`).
- `expires_at: null` means it never expires. Revocation shows up as HTTP 401/403 on the next dial (or as the socket closing).
- The token fixes `device_id`, so every reconnect lands on the same device record.

## 2. Dial

```
GET {connect_url}?name=Comma-5D28&alias=comma-5d28&os=esp-idf&arch=xtensa&scope=<scope>
Authorization: Bearer salix_conn_…
X-Salix-Connector-Instance-ID: <random UUID per boot>
(normal WebSocket upgrade, no permessage-deflate)
```

| Field | Required? | Notes |
|---|---|---|
| `Authorization` | **yes** | Header only. `Bearer ` prefix optional; a token in the query string is rejected (401). |
| `scope` | **yes, in practice** | `""` (operations allowed) or `local_file_read` (read-only). Any other value is rejected with 400. Send it on every dial (see §5). |
| `name` | no | Defaults to the token's name, but a dial's `name` wins. T-Circle sends its saved `device_name` (BLE setup), or `Comma-XXXX` if none, percent-encoded (UTF-8). |
| `alias` | no | Defaults to the token's alias, else `name`. No format check. T-Circle sends the lower-case BLE name, `comma-xxxx`: it survives renames and ends in `-<short id>`, which nearby-device setup offers match on (case-insensitive, along with `name` and `hostname`). |
| `os`, `arch` | no | Shown in Settings. |
| `X-Salix-Connector-Instance-ID` | no | ≤128 bytes. Keep it stable across reconnects within one boot; generate a new one each boot. |
| `group_id` | **never** | 400 `group_id is not accepted with connector credential auth`. |

Handshake failures are JSON `{"error": "..."}` bodies: 401 `unauthorized` (bad, missing or revoked token), 403 (credential revoked or expired), 400 (bad `scope` or `group_id` present), 503 (try again later).

## 3. Frames

One JSON object per WebSocket **text** frame. The server ignores binary frames, invalid JSON and unknown `type`s. Max frame is 16 MiB each way; T-Circle should cap its own buffers far lower (§6).

### Server → device

```json
{"type":"connected","connector_run_id":"run_voWRnWbVJmOE1kJJ","device_id":"dev_Pc8wBuEUfP5LT55XBx3qBw",
 "connector_id":"conn_ZnWGBt3zzh8-ZSiYBjW5lQ","owner_user_id":"usr_HT4lByGvItZN","connection_generation":12}
```
Sent once, right after the upgrade. Logging it is enough; no reply is needed.

```json
{"type":"heartbeat"}
```
Sent every 15 s. **Reply with `{"type":"heartbeat"}` every time.**

```json
{"id":"req_7GSGEDzFTz13NEO9","type":"request","method":"http_request",
 "params":{"method":"POST","url":"http://192.168.5.19:8765/mcp",
           "headers":{"accept":"application/json, text/event-stream","content-type":"application/json"},
           "body":"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}",
           "timeout_seconds":115,"max_bytes":10485760}}
```

```json
{"id":"req_LB4ksvfUcC8ykGEY","type":"request","method":"device_access","params":{"allow_operations":true}}
```

The server may send **any** method name. Capabilities don't declare methods, so every request needs an answer (§4).

### Device → server

**metadata**: send right after the socket opens (no need to wait for `connected`), and again whenever access changes.
```json
{"type":"metadata",
 "capabilities":{"scope":"","device_access":true,"execution_boundary":"lan-gateway",
                 "persistent_processes":false,"computer_use_tool":false,"host_access_required":false,
                 "runtime_probe":false,"meeting_runtime":false,
                 "component_versions":{"t-circle":"0.1.0"}},
 "skills":[],
 "system_info":{"client_source":"t-circle","hostname":"Comma-5D28","os_type":"esp-idf","arch":"xtensa",
                "collected_at":1791187838}}
```
- `capabilities.scope` must match the current access state (`""` or `"local_file_read"`).
- `system_info.hostname` becomes the name shown in Settings › Devices (after a rename there, which wins over it). T-Circle sends the same name as the dial's `name`.
- `skills` is optional. The server stores it but no agent reads it, so `[]` or omitting it has no effect.
- Unknown keys are allowed and stored (the public projection drops them). Do **not** send `agent_runtimes`.

**heartbeat**: `{"type":"heartbeat"}`, as a reply to each server heartbeat.

**response**: echo `id` exactly.
```json
{"id":"req_7GSGEDzFTz13NEO9","type":"response",
 "result":{"status":200,"headers":{"Content-Type":["application/json"]},"body":"{…}",
           "truncated":false,"final_url":"http://192.168.5.19:8765/mcp","method":"POST","body_bytes":118}}
```

**response_chunk** (G1): one piece of a binary `http_request` body, only for a request that asked for `"response_mode":"chunks"`. Echo `id`; `seq` counts from 0 per request. See §4.
```json
{"id":"req_7GSGEDzFTz13NEO9","type":"response_chunk","seq":0,"data_b64":"/9j/4AAQSkZJRgABAQ…"}
```

**error**: echo `id`. The `error` string reaches the agent verbatim.
```json
{"id":"req_eR4BgPf5e2AK4xTo","type":"error","error":"method \"http_request\" is not permitted for a local_file_read connector"}
```

## 4. Methods

### `http_request`
Params: `method` (GET/POST/PUT/PATCH/DELETE; default **POST**), `url` (absolute `http`/`https`, required), `headers` (string → string), `body` (string), `timeout_seconds` (default 120), `max_bytes`.
- If `body` is non-empty and no Content-Type header is set, send `Content-Type: application/json`.
- Do **not** follow redirects. Return the 3xx as is; `final_url` = the request URL.
- Read at most `min(max_bytes, device cap)` bytes. Set `truncated: true` when the body was longer.
- `headers` in the result: header name → **array** of values.
- `body` in the result: UTF-8 text (replace invalid bytes).
- Errors (bad URL, unsupported method, connect failure, timeout): send an `error` frame with a short message.
- **Gate:** while access is read-only, reply `error` without touching the network (§5).

#### Chunk mode: binary bodies (G1)

For bodies that are binary or bigger than the text cap, e.g. a camera snapshot (~800 KB JPEG). Shared with the backend task G2.

**Capability.** The gateway advertises it in `metadata`; the server uses chunk mode only on gateways that do:
```json
"capabilities":{…,"lan_gateway":{…,"response_chunks":{"max_total_bytes":4194304,"chunk_bytes":32768}}}
```

**Request.** `http_request` params gain `"response_mode":"chunks"`. Without it (or `null`), behaviour is exactly the text mode above. Any other value → `error`.

**Device → server:**
1. Zero or more `response_chunk` frames, in order, `seq` from 0. Each carries 1 to `chunk_bytes` raw body bytes, standard base64 with padding. **Frames may be smaller than `chunk_bytes`**, and their sizes may vary: T-Circle sends 8 KB frames (§6). Count the frames; don't compute the count from `chunk_bytes`.
2. Then exactly one final frame:
   ```json
   {"id":"req_…","type":"response","result":{
     "status":200,"headers":{"Content-Type":["image/jpeg"]},
     "body_encoding":"chunks","chunks":100,"body_bytes":812662,
     "sha256":"<lowercase hex of all raw body bytes>","truncated":false,
     "final_url":"http://…","method":"GET"}}
   ```
   - No `body` field. `chunks` = number of chunk frames sent, `body_bytes` = their raw bytes together.
   - `truncated: true` when the body was longer than `min(max_bytes, max_total_bytes)`. The device stops reading there; `sha256` covers what it sent.
   - An empty body: no chunk frames, `chunks: 0`, sha256 of nothing (`e3b0c442…b855`).
3. On failure part-way (LAN server closed early, the device's link to the server stalled for 10 s, access turned off mid-transfer), one `error` frame with the same `id` instead of the final frame. Discard the chunks received so far.

Same gates as text mode: access (read-only → `error`, no network), LAN destinations only, no redirects. **While a voice call is active, T-Circle refuses chunk mode** with `error` `busy: voice call active` without touching the network, and a call that starts mid-transfer ends the transfer with that `error`; text mode is unaffected. `timeout_seconds` is capped at 110 s in chunk mode (30 s in text mode) and covers the whole transfer.

### `lan_tcp_dial` (G3)

A raw TCP tunnel from the gateway to a LAN host, the same protocol as salix-connect (`systems/connector/salix-connect/lan_tunnel.go`) and station (`systems/station/src/connector/lan-tunnel.ts`). The backend uses it for SSH through a gateway, and G3 for RTSP to the camera. Advertised as `"lan_tcp_dial"` in `capabilities.lan_gateway.methods`.

**Request:**
```json
{"id":"tun_…","type":"request","method":"lan_tcp_dial","params":{"host":"192.168.5.33","port":554,"timeout_seconds":15}}
```
- `host` (required): dotted quad or a name (resolved once, IPv4). `port` (required): 1–65535. `timeout_seconds`: dial timeout; missing, ≤ 0 or > 15 means 15. The server waits 20 s for the answer.

**Answer**, once the TCP connection is up:
```json
{"id":"tun_…","type":"response","result":{"status":"connected","remote_address":"192.168.5.33:554","window":131072,"max_frame":8192}}
```
Or an `error` frame: `connect to 192.168.5.33:8800 refused: nothing is listening on that port`, `connect to … timed out after 15 s`, `tunnel capacity exhausted`, `tunnel already exists`, `'host' is required`, `'port' must be between 1 and 65535`, a LAN-destination refusal, the read-only refusal, or `busy: voice call active`.

**Then `stream` frames under the same `id`, both ways:**
```json
{"id":"tun_…","type":"stream","stream":{"channel":"tcp","data":"<base64>"}}
{"id":"tun_…","type":"stream","stream":{"channel":"tcp","eof":true}}
{"id":"tun_…","type":"stream","error":"…","stream":{"channel":"tcp","eof":true}}
{"id":"tun_…","type":"stream","stream":{"channel":"tcp_ack","seq":<n>}}
```
- `data`: standard base64 with padding. The device sends at most `max_frame` (8 KiB) raw per frame. **The server sends up to 32 KiB per frame regardless of `max_frame`** (DeviceRelay's own `@max_frame`); the device accepts that.
- `tcp_ack`: "I have written `seq` bytes in total", sent by whichever side receives data, after it writes it.
- Each sender keeps at most `window` bytes unacknowledged. The server reads `window` from the answer (if ≥ 32 KiB, else it assumes 512 KiB). The device ends the tunnel with `tunnel window exceeded` if the server overruns it, or `invalid tunnel frame` for bad base64 or a frame over 32 KiB.
- `eof` from the device: the LAN host closed (no `error`), or a failure (`error`: `192.168.5.33:554 reset the connection`, `tunnel idle for 60 s`, the read-only refusal, `busy: voice call active`). After an `eof` from the server the device writes what it still holds (≤ 2 s), closes the LAN socket and sends nothing more.

**Gates:** access (read-only → `error`, no network; turned read-only mid-tunnel → `eof` with the refusal), LAN destinations only (same rule as `http_request`), at most 2 tunnels. **Refused during a voice call** (`busy: voice call active`), and a call that starts ends every open tunnel with `eof` + that error. A tunnel with no data either way for 60 s ends. A Connector socket that closes ends its tunnels silently.

### `device_access`
Params `{"allow_operations": bool}`. Anything else → `error` `allow_operations must be a boolean`.
1. Save the new access state to NVS (`true` → `""`, `false` → `local_file_read`).
2. Re-send `metadata` with the new `capabilities.scope`. **Required**: the server's routing view and Settings' `allows_operations` come only from metadata (or the dial `scope`), never from this response.
3. Reply `{"allows_operations": <bool>}`. Comma checks the value matches the request, or the Settings toggle fails.

### Anything else
Reply with `error` `unknown method: <name>`. The server hands that string to the caller and keeps the socket open.

## 5. Access state

- On first boot, or when nothing is stored, the device is **read-only** (`local_file_read`), like salix-connect `--device`.
- Always send the current state as the dial `scope` and as `metadata.capabilities.scope`. Once a device has reported a scope, a later connection that reports none is shown with **no command environment** at all; the server treats the old scope as stale (verified).
- The server does **not** enforce access on the device-placed MCP path. Settings toggles the device's own state, and only the device's check stops `http_request` (verified: with access off, the call reached the device and was refused there). **The firmware gate is the real gate.**

## 6. Liveness, timeouts, limits

| Item | Value |
|---|---|
| Server heartbeat interval | 15 s |
| Server drops the socket when | ≥45 s since the last **device** `heartbeat` frame. Checked on each 15 s tick; closes with code 1011. Other frames don't count. |
| Device read timeout (salix-connect) | 45 s without any frame → treat as dead, reconnect |
| Server wait on `http_request` | 120 s (or `timeout_seconds`+5 s, capped at 120 s) |
| Server wait on `device_access` / other methods | 30 s |
| Late reply (after the server timed out) | Silently dropped |
| Pending requests per socket | 256; T-Circle can handle them one at a time but must answer each |
| Max frame | 16 MiB server side. T-Circle: cap `http_request` bodies at e.g. 32 KiB and report `truncated` (the MCP path asks for `max_bytes` = 10 MiB) |
| T-Circle `http_request` caps | Text mode: 64 KiB body, 30 s. Chunk mode: 4 MiB in all, 110 s for the whole transfer |
| T-Circle chunk frames | 8 KiB raw (~11 KB of JSON), so each frame fits the free send buffer lwIP's `select()` guarantees and never blocks holding the TX lock. 32 KiB when the Connector socket couldn't be identified |
| Chunk mode during a voice call | Refused: `error` `busy: voice call active`, before any network use. A call that starts mid-transfer ends it with the same `error`. Retry after the call (G1 report: a transfer alongside a call froze the board's Wi-Fi) |
| Chunk mode, no progress | A frame that can't be written for 10 s ends the transfer with an `error` frame |
| Server wait on `lan_tcp_dial` | 20 s for the answer (`SalixEnv.Protocol`) |
| T-Circle `lan_tcp_dial` | 2 tunnels; `window` 128 KiB, `max_frame` 8 KiB toward the server; accepts 32 KiB frames from it; dial ≤ 15 s; 60 s idle; LAN socket receive window cut to 16 KiB |

## 7. Reconnect

- On close or error: reconnect with exponential backoff (1 s → 30 s, with jitter). Reset the backoff after a connection that lasted more than 60 s.
- On HTTP 401/403 at the handshake: stop and wait for re-provisioning (the token was revoked). Don't loop.
- Every reconnect gets a new `connector_run_id` and `connection_generation`, but the same `device_id`. Send `scope` and `metadata` again each time.
- Settings shows the device `disconnected` as soon as the socket closes, and `connected` again within ~1 s of the new dial (verified).

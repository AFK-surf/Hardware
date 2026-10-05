# A3 report: `http_request` and the access gate on the T-Circle

2026-10-05 · `t-circle-s3/main/lan_http.c/.h` (new), `connector.c`, `display.c`, `wifi.c/.h` · board on `ToEverything-IOT` (192.168.5.17) · staging `dev_M3Z6mxvKhQSYEwyYTber-g`

## Result

| Check | Result |
|---|---|
| Internal RAM, call + Connector + a LAN request (target ≥ 40 KB) | **Met.** Lowest internal free during a call with LAN requests in flight: **114.5 KB** (B1b, call alone: 14.4 KB). See *RAM*. |
| Internal RAM while BLE is on (target ≥ 20 KB) | **Not measured on the board** (see *Not done*). By arithmetic, ≈ 105 KB: the 102 KB freed is static and BLE's cost hasn't changed. |
| Access off → request refused | **Verified on staging.** Agent-created device-placed MCP: the server sent two `http_request`s at 18:47; the board refused both within 1 ms with `method "http_request" is not permitted for a local_file_read connector`, and the LAN server received nothing. The Comma app showed the device as read-only (`permission_required`, `exec=false`). |
| Access on → agent reads a LAN service through the T-Circle | **Verified on staging.** The user turned on *Allow operations* in the Android app (18:50:25, `device_access` → metadata `scope=""` → reply). The agent's `lan_status` call came back "LAN test device ok, local time 18:51:49"; the LAN server logged every request from 192.168.5.17 (the T-Circle). |
| Voice still works | 4 calls on the A3 firmware (122.7 s, 104.0 s, 38.9 s, 57.8 s), all close 1000 `caller_hangup`, no 4410/4400. See *Calls (#2504)*. |
| Voice call while a request runs | **Verified.** Three `lan_status` calls through the T-Circle during a 104 s call, all 200; no dropped mic frames; lowest internal free 114.5 KB. |
| Host tests | `tools/lan_http_test/run.sh`: 99 checks, ASan + UBSan, all pass. |

## RAM

### Where it went

`idf.py size` showed our own `libmain.a` holding **119.6 KB of internal DRAM**: `display.c`'s two framebuffers were `static DMA_ATTR uint16_t s_fb[2][160*160]`, i.e. **102 400 bytes of `.data` in internal RAM** (`nm`: `s_fb 0x19000`). The B1 report said the logo task "only draws into the PSRAM framebuffers"; it was wrong: only the task's stack was in PSRAM.

Fix: allocate the framebuffers in PSRAM (64-byte aligned) and set `psram_dma_direct` on the panel IO so the SPI DMA reads them straight from PSRAM. Without that flag the driver would copy each 50 KB frame into an internal bounce buffer, which would make things worse. The panel runs at 40 MHz SPI, far below PSRAM bandwidth. The serial log shows no `DMA TX underflow` errors through boot, the calls and the requests. Nobody has looked closely at the screen on this firmware yet (the user didn't report problems).

| | main (`eef83d1`) | A3 |
|---|---|---|
| Static DIRAM | 266.8 KB (78.1 %) | **164.4 KB (48.1 %)** |
| `.data` | 126.4 KB | 24.0 KB |
| Image | 2.72 MB | 2.65 MB |

### Measured (`heap_caps_*(MALLOC_CAP_INTERNAL)`, from the Connector's `stats` line)

| Point | B1b (main) | A3 |
|---|---|---|
| Wi-Fi up, before the first dial | 67.2 KB (largest block 31 KB) | **169.0 KB** (largest 86 KB) |
| Connector up, idle | 51.5 KB (min 49.9) | **153.5 KB** (min 152.3) |
| Connector + LAN requests (MCP discovery and a tool call, no call) | — | min **150.9 KB** |
| Connector + voice call | 17.1 KB (min **14.4**) | 116.9–118.9 KB (min **116.4 KB**, largest 45 KB) |
| Connector + voice call + 3 LAN requests | — | 118.5 KB (min **114.5 KB**, largest 45 KB) |
| After the call | 51 KB | 153.3 KB |

The LAN request path costs about 2.5 KB internal at peak (its stack is in PSRAM; what's left is lwIP socket state and small header strings).

### The other items on the prompt's list

- **Task stacks to PSRAM.** The new LAN worker's 8 KB stack is in PSRAM (`xTaskCreateWithCaps`); it never writes flash. `ui`, `prov` and `connector` stay internal because they write NVS. A flash write disables the cache, and IDF asserts on a PSRAM stack (`esp_task_stack_is_sane_cache_disabled`, `spi_flash/cache_utils.c:126`). The way around that is `CONFIG_SPIRAM_XIP_FROM_PSRAM` (code and rodata run from PSRAM, so flash writes leave the cache on: `SPI_FLASH_CACHE_NO_DISABLE` in `spi_flash_os_func_app.c:32`). It's a global change that touches voice timing. With 100 KB back, it isn't needed now; it's the next lever if RAM gets tight again (about 18 KB more).
- **Wi-Fi/lwIP and mbedTLS.** Already in PSRAM: `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` (boot log "WiFi/LWIP prefer SPIRAM") and `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`. Still internal: 16 static RX buffers (DMA, ~26 KB) and the Wi-Fi IRAM optimisations (`ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT`). Left alone, because the voice fixes (`81297a7`) were tuned with them.
- **NimBLE.** Already `MEM_ALLOC_MODE_EXTERNAL`, one connection, peripheral only. Not changed: pairing mode needs the controller again, so its memory can't be released for good.

## `http_request`

Params and result shape match salix-connect's `methodHTTPRequest` (`main.go:4283-4362`). One request at a time on a PSRAM-stack worker; the Connector task keeps answering heartbeats and does all socket writes (replies come back to it through its event queue).

| Item | Behaviour |
|---|---|
| Methods | GET, POST, PUT, PATCH, DELETE (case-insensitive); default POST; others → `unsupported HTTP method: X` |
| URL | absolute `http`/`https` only; credentials in the URL and IPv6 literals refused with a clear error; `#fragment` dropped |
| Headers | string, number, bool or null values; empty names skipped; values with CR/LF refused. The device always sets `Host`, `Content-Length`, `Connection: close`; `User-Agent: comma-t-circle/0.1` unless given. `Content-Type: application/json` when there's a body and no type. `Content-Length: 0` on empty POST/PUT/PATCH, as Go does |
| `timeout_seconds` | whole-request deadline (connect, send and read), capped at **30 s**; missing/invalid → 30 s |
| `max_bytes` | capped at **64 KB** (server asks for 10 MiB on the MCP path) → `truncated: true` |
| Redirects | not followed; the 3xx comes back as is, `final_url` = request URL |
| Result | `status`, `headers` (canonical name → array), `body` (UTF-8, invalid bytes → U+FFFD, NUL as `\u0000`), `truncated`, `final_url`, `method`, `body_bytes` (raw bytes kept) |
| https | `esp_transport_ssl` with the certificate bundle; the certificate is checked against the URL's host name. A LAN device with a self-signed certificate fails, as it does with salix-connect |
| Queue | 4 waiting; more → `the T-Circle is busy with other LAN requests; try again shortly` |
| Socket closes | a reply only goes out on the socket its request came in on; queued requests from a closed socket are skipped |

**Not `esp_http_client`** (the prompt asked for it). Its read loop compares bytes read against `content_length`, which is -1 when the response has neither Content-Length nor chunked encoding (`esp_http_client.c:1406`), so it returns an empty body for HTTP/1.0-style responses that end by closing the connection. Small LAN devices often answer that way. `lan_http.c` uses IDF's `esp_transport` (TCP/TLS) and `http_parser` instead, which handle chunked, Content-Length and close-delimited bodies.

### Destination allowlist

The host is resolved once (strict dotted quad, or DNS, IPv4 only), checked, and the connection goes to that IP, so a second lookup can't rebind it.

- Allowed: 10/8, 172.16/12, 192.168/16, 169.254/16, and the device's own subnet when its prefix is /16 or longer (a LAN on non-private space, e.g. carrier NAT).
- Refused: loopback, the device's own IP, 0/8, multicast/reserved/broadcast (≥ 224.0.0.0), the own subnet's network and broadcast addresses, everything public. Example error: `8.8.8.8 is a public address. The T-Circle only reaches the local network (private, link-local or its own subnet); use web.http_request for the internet`.

### Access gate

`device_access` was already in A2 (NVS `connector/allow_ops`, read-only by default, metadata re-sent before the reply). A3 adds the gate on `http_request`: checked when the request arrives (refused without touching the network) and again when the worker picks it up, in case access was turned off while it waited.

### Metadata

`capabilities.lan_gateway = {"methods":["http_request"],"max_bytes":65536,"max_timeout_seconds":30}`. Salix stores unknown capability keys. **For the C2 owner:** my unfinished C2 draft (`Cue-lan-request`, uncommitted) advertised `lan_request_v1: true` from salix-connect. Pick one name; the firmware is easy to change.

## Staging test (how)

C2's `device.lan_request` isn't on the Cue remote yet, so the test used the device-placed remote MCP path (the only server path that sends `http_request` to a device, `salix_mcp/connection.ex:972`):

1. `tools/lan_mcp_server.py` on the Pro (192.168.5.13:8765, same /24 as the board): a stdlib MCP server with `lan_status` and `echo`, logging each caller's IP.
2. In a Comma chat, the agent created the definition and binding itself (`mcp.definition_create`, then `mcp.connect` with `placement: "device"` on the T-Circle), then called `lan_status`. These agent tools are hidden from the model's tool list but work for an ordinary user session.

LAN server log (the T-Circle is 192.168.5.17):
```
18:51:35 192.168.5.17 POST /mcp initialize
18:51:35 192.168.5.17 POST /mcp notifications/initialized
18:51:36 192.168.5.17 POST /mcp tools/list
18:51:49 192.168.5.17 POST /mcp tools/call
```

Board serial:
```
18:47:20 connector: request req_5m01o6-llcFjL1dA http_request
         connector: req_5m01o6-llcFjL1dA refused: read-only
18:47:26 connector: request req_mLkMmDaSzBY2K839 http_request
         connector: req_mLkMmDaSzBY2K839 refused: read-only
18:50:25 connector: request req_9IbgxSH4N2CpCJNF device_access
         connector: access: operations allowed
         connector: sent metadata scope=""
18:51:34 lan_http: req_VPJdmwRbZXPZBoBC POST 192.168.5.13:8765 (timeout 30 s, max 65536 B)
         lan_http: req_VPJdmwRbZXPZBoBC -> 200, 166 B, 1109 ms
         lan_http: req_9QPdmNLDRbGRbjdf -> 202, 0 B, 39 ms
         lan_http: req_lCxS2XlEO570sf57 -> 200, 401 B, 34 ms
18:51:50 lan_http: req_mUp32yqG3w6EWUSk -> 200, 180 B, 37 ms
```

Security note, confirmed again: on this path the server checks neither device access nor the destination; the agent chose the URL. The firmware gate and allowlist are the only boundary.

## Calls (#2504, deployed to staging 17:37)

| # | Start | Firmware | call id | Length | End | 4410/4400 |
|---|---|---|---|---|---|---|
| 1 | 17:40:12 | main | `vc_01M45PYX16XX8RA1FEH5Q63X3F` | 61.9 s | `caller_hangup`, 1000 | no |
| 2 | 17:41:25 | main | `vc_01M45Q13TTQQ7GR4KCSYN4CQTG` | ~50 s | hangup logged; no `session.ended`/close line in that log (the planning session's capture, which has gaps) | none logged |
| 3 | 18:01:15 | A3 | `vc_01M45R5A0WGH3KSKY2AA5WWS3E` | 122.7 s | `caller_hangup`, 1000 | no |
| 4 | 19:01:05 | A3, **3 LAN requests during the call** | `vc_01M45VJVW0C9K3PDF0Q8ZPQ2TK` | 104.0 s | `caller_hangup`, 1000 | no |
| 5 | 19:03:03 | A3 | `vc_01M45VPEVW8BGVCA2JM9DFVZMD` | 38.9 s | `caller_hangup`, 1000 | no |
| 6 | 19:03:55 | A3 | `vc_01M45VR22TZ4SFBCF2C6D1QV28` | 57.8 s | `caller_hangup`, 1000 | no |

Calls 1 and 2 are from the planning session's serial log, before the A3 flash at 17:50. Call 2 dropped mic frames for a few seconds while the agent spoke (`mic_dropped` up to 28/s, `send_max_ms` up to 542 ms) but didn't close. The user said all calls felt smooth.

Call 4 is step 6's "voice call while a request runs": during it the user asked the agent to call `lan_status`, and three `tools/call` requests went through the T-Circle (`req_c4TpZYUSJo2PHQ9c` 200 in 3224 ms, `req_wAC5-jsUQT-YewgO` 169 ms, `req_8iuJPmVgxxeX1ddD` 52 ms; outside a call the same request takes 34–37 ms). The call showed no dropped mic frames and no stall. Lowest internal free during it: **114.5 KB** (largest block 45 KB). The 3.2 s first request is most likely the new TCP connection competing with call audio on the radio; it's well within the 30 s cap.

Call 5 was shorter than the 1–2 min asked for (the user hung up at 38.9 s). It had one ~1 s uplink stall at 19:03:26 while the agent spoke (`send_ms` 416, `mic_backlog_ms` 999, `mic_dropped` 61 in that second), with no LAN request running and the Connector idle. Same office-Wi-Fi pattern as A2; it recovered without a close. Call 6 (57.8 s) had no dropped mic frames.

**#2504 summary:** 6 calls after the 17:37 deploy (4 on A3), 39–123 s, no 4410 or 4400, every logged close was 1000 `caller_hangup`. Call 2's ending wasn't captured. No call lasted long enough to say much about longer sessions.

## Not done / for later

- **BLE-on heap not measured.** Pairing mode wasn't exercised on the A3 firmware. Expected ≈ B1b's 2.8–3.1 KB plus the 102 KB freed.
- **https to a LAN device** not tried on the board (no LAN https service in the office); covered only by code path and cert-bundle reuse from the Connector.
- **Wi-Fi drop** still not verified (carried over from A2).
- `tools/lan_mcp_server.py` stays as the LAN stand-in for A4/C2 tests. The staging MCP definition and binding `lan-test` created by the agent are still there; remove or disable them when no longer needed.

## Re-run

```
cd t-circle-s3/tools/lan_http_test && ./run.sh        # host tests, no board
python3 tools/lan_mcp_server.py 8765                  # LAN stand-in on a machine in the board's subnet
```

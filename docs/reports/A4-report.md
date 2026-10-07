# A4 report: `udp_request` on the T-Circle (firmware part)

2026-10-07 · `t-circle-s3/main/lan_udp.c/.h`, `lan_dest.c/.h` (new; the allowlist moved out of `lan_http.c`), `connector.c`, `main/CMakeLists.txt`, `tools/lan_udp_test/` (new), `tools/lan_http_test/test.c` · board on `ToEverything-IOT` (192.168.5.17), staging

The backend part (UDP in `device.lan_request` and the miIO codec, backend stage 2a) isn't on staging yet, so the end-to-end "关灯" test from the A4 prompt is still to do. This covers the firmware.

## Result

| Check | Result |
|---|---|
| `udp_request` in the agreed shape | **Done.** `{host, port, payload_b64, timeout_ms, max_responses?}` → `{responses: [{from, payload_b64}], timed_out}`. |
| One datagram out, answers in until `timeout_ms`, early return at `max_responses` | **Done; host-tested** (returns in under a millisecond once the answer is in; waits out the timeout otherwise). |
| LAN destinations only, same as `http_request` | **Done.** The allowlist moved into `lan_dest.c` and both methods call it, so the rule can't drift. |
| Access gate | **Done.** Same `handle_lan_request` path as A3/A5: refused while read-only (`method "udp_request" is not permitted for a local_file_read connector`), checked again when the worker picks it up. Not exercised on the board this time (access was on). |
| `lan_gateway.methods` | **Done.** `["http_request","udp_request","lan_discover"]`. Metadata frame 539 B (buffer 768). |
| miIO hello to the office lamp (192.168.5.19:54321) | **Verified on the board.** 32 bytes out, a 32-byte answer in 37–141 ms, **11 of 11** tries (self-test build, see below). |
| Host tests | `tools/lan_udp_test/run.sh`: **747 checks, all pass** (ASan + UBSan). `lan_http_test` 99/99 after the move, `lan_discover_test` 1221/1221. |
| Build | Clean, no warnings. Release build flashed. |

## Behaviour

| Item | Behaviour |
|---|---|
| `host` | Required string ≤ 253 bytes; strict dotted quad or a DNS name (IPv4). Resolved once; the datagram goes to that IP. |
| Destination | Private (10/8, 172.16/12, 192.168/16), link-local, or the own subnet when it's /16 or smaller. Refused: loopback, the device itself, 0/8, multicast, **broadcast** (255.255.255.255 and the subnet's), public. Same messages as `http_request`. |
| `port` | Required whole number 1–65535. |
| `payload_b64` | Required. Standard base64 with padding, no whitespace (strict). Empty is allowed (sends an empty datagram). At most **1472 bytes** decoded (one datagram in one Wi-Fi frame, no IP fragmentation); longer → `the payload is longer than 1472 bytes, the most this device sends in one datagram`. |
| `timeout_ms` | Whole number; default **2000**, clamped to **100–10000**. Measured from the send. |
| `max_responses` | Whole number; default **1**, clamped to **0–16**. `0` sends and returns at once with `timed_out: false`. |
| Answers | Only datagrams from the destination's IP count (any source port); others are ignored and logged as such. Each answer up to **4096 bytes** (longer ones are cut by the socket). `from` is `"ip:port"`. |
| `timed_out` | `true` when the timeout ended the wait before `max_responses` answers arrived (also with 0 answers). |
| Errors | Bad params, refused destination, unresolvable host, send failure → an `error` frame with a short message; nothing is sent for the first three. |
| Socket | A fresh UDP socket per request, closed afterwards. Runs on the shared LAN worker (one request at a time). |
| Memory | The reply is built in PSRAM (via the `lan_discover_core` string builder); cJSON would have put up to ~90 KB of base64 in internal RAM. Payload buffer (1.5 KB) on the worker's PSRAM stack. |
| Log | `lan_udp: <id> 192.168.5.19:54321: sent 32 B, 1 answer(s), 45 ms` (plus `timed out` / `others ignored`). |

These match the old salix-connect draft (`Cue-lan-request/…/udp_request.go`: default 2 s, 1 answer, max 16) except: the board refuses broadcast (the draft allows it), takes 1472 bytes instead of 16 KB, and ignores answers from other hosts. **For the backend:** don't rely on broadcast; the miIO codec should send the hello unicast to the lamp's IP.

## On the board

Self-test build (`-DCOMMA_LAN_DISCOVER_SELFTEST_S=20 -DCOMMA_UDP_SELFTEST_HOST=192.168.5.19`): every 20 s a `lan_discover`, then a miIO hello (`0x21 0x31 0x00 0x20` + 28 × `0xFF`) to 192.168.5.19:54321.

```
12:02:59 lan_udp: selftest_udp_1 192.168.5.19:54321: sent 32 B, 1 answer(s), 76 ms
         {"responses":[{"from":"192.168.5.19:54321",
                        "payload_b64":"ITEAIAAAAAB6jc7HAAKF3v////////////////////8="}],"timed_out":false}
```

Decoded: `21 31 00 20` (magic, length 32) · `00000000` · device id `7a8dcec7` · timestamp `000285de` · checksum `FF…` (a paired device doesn't hand out its token). That is what the cloud codec needs to start a session: device id and timestamp. All 11 hellos in the run were answered (37–141 ms), 6 of them during a voice call (A5 report).

The self-test hooks stay in `connector.c` behind the two CMake variables; the release build has neither (checked in `compile_commands.json`).

## Not done

1. **End to end with the backend** (A4 prompt step 3): Mac Connector off, "关灯" from the phone and from a T-Circle call, the light changes and is read back, timing from end of speech to light change. Needs backend stage 2a on staging.
2. Access-off refusal for `udp_request` not seen on the board (same code path as A3's, which was).

## Re-run

```
cd t-circle-s3/tools/lan_udp_test && ./run.sh
```

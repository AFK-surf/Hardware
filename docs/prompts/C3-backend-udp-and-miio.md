# C3 · Backend phase 2a: UDP through LAN gateways + a miIO codec (no secret store yet)

For the Comma backend team (AFK-surf/Cue, `systems/`). Follows C2 v2 (#2619 `device.lan_request`, #2620 skills, #2621 `device.lan_discover`), which is merged and live on staging since 2026-10-07 02:20 CST.

## Background

Verified on staging on 2026-10-07 with the T-Circle (ESP32 gateway on office Wi-Fi):
- `device.lan_discover` through the T-Circle listed 18 hosts on the office LAN.
- `device.lan_request` reached a Xiaomi bedside lamp (Mi bslamp2, 192.168.5.19) over HTTP and got 404. The lamp only speaks **miIO: encrypted UDP on port 54321**. So the agent fell back to a Python miIO script on the user's Mac. That's exactly the dependency this product removes.

Goal of this phase: the agent controls the Mi lamp through the T-Circle with the Mac turned off. The cloud does all the crypto; the gateway only sends and receives bytes. Keep the codec boundary generic: Gree air conditioners (UDP 7000, AES-ECB) and Tuya local come next.

**Out of scope for 2a:** a per-group secret store for device keys. That's phase 2b, with a security review. In 2a, the miIO token is passed as a tool argument, so it appears in the transcript. **This is acceptable for internal testing only.** Mark the tool or its description accordingly; don't advertise it to external users until 2b replaces the argument with a secret reference.

## 1. UDP through a gateway

- New Connector method `udp_request`, already being implemented in the T-Circle firmware with exactly this contract:
  - params: `{host, port, payload_b64, timeout_ms, max_responses?}`
  - result: `{responses: [{from, payload_b64}], timed_out: bool}`
  
  The firmware sends one datagram, collects replies until `timeout_ms` or `max_responses`, and only allows private, link-local and own-subnet destinations. It advertises `"udp_request"` in `capabilities.lan_gateway.methods`. Add a protocol timeout entry.
- `device.lan_request` gains a UDP mode: `udp: {host, port, payload_b64, timeout_ms?, max_responses?}`. Same gating as HTTP: lan_gateway, online, private destination, `allows_operations`, write-safety. Dispatch only if the gateway lists `udp_request`.
- Add `udp_request` to `salix-connect` too (parity with the Mac Connector), behind the same LAN-only rules as its `lan_http_request` path.

## 2. miIO codec, and a tool that uses it

- Implement miIO in Elixir with `:crypto`. Use python-miio as the reference: `miio/protocol.py`, and its tests for known vectors.
  - Hello: a 32-byte packet (magic `0x2131`, length `0x0020`, rest `0xFF`). The reply carries the device id and a timestamp ("stamp").
  - Command: a 32-byte header (magic, length, `0x00000000`, device id, stamp + seconds since the hello) + encrypted payload.
    - key = MD5(token), iv = MD5(key ‖ token), AES-128-CBC with PKCS#7 padding.
    - The checksum in the header is MD5(header-with-token-in-checksum-slot ‖ encrypted payload), as in python-miio.
    - Payload: JSON `{"id": n, "method": "...", "params": [...]}`.
  - Decrypt replies the same way and verify the checksum.
- Put the codec behind a small codec boundary (for example a behaviour or module per protocol) so Gree and Tuya can be added without touching the tool.
- New tool **`device.miio_request`** (or a `codec: "miio"` option on `device.lan_request`; your call):
  - args: `device_id` (the gateway), `host`, `token` (32 hex chars), `method`, `params`;
  - runs hello → command → decode as two `udp_request` round trips through the gateway;
  - returns the decoded JSON result;
  - cache the device id and stamp per (gateway, host) for a short time to save the hello;
  - errors: hello timeout ("device not answering on 54321"), bad token or checksum, device error codes.
  - Same gating and safety as `lan_request`.
- Tests: codec unit tests against python-miio vectors (build, encrypt, decrypt, checksum), plus a tool test with a fake gateway that replays recorded packets.

## 3. Skill

A short built-in skill for **Xiaomi miIO lights (bslamp2 and Yeelight-family Mi lamps)**:
- identify the lamp: `lan_discover` shows it via `_miio` or `_hap` mDNS, or ARP; the user supplies the token;
- read state: `get_prop ["power","bright","rgb","ct","color_mode"]`;
- set state: `set_power ["on"|"off","smooth",500]`, `set_bright [1..100,"smooth",500]`, `set_rgb [int,"smooth",500]`, `set_ct_abx [kelvin,"smooth",500]`;
- read back after every change;
- never log or repeat the token.

## End-to-end check (with the user, after deploy)

The T-Circle firmware will advertise `udp_request` by then. The user turns off their Mac's Connector and says "关掉床头灯" in Comma. The agent controls 192.168.5.19 through the T-Circle and reads the state back. The user gives the lamp token in chat for this test.

## PRs

Draft PRs as usual: (a) UDP (protocol, tool mode, salix-connect parity); (b) miIO codec + tool; (c) skill.

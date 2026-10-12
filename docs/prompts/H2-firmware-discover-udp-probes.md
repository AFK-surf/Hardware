# H2 · Firmware: UDP probes in `lan_discover`

Start only after `codex/multi-board` (H1) is merged into main. Work on a new branch from that main,
IDF 6.0.2. Paths below are the multi-board layout (`firmware/`, `devices/`, `tools/board.sh`).

Read first:
- `docs/connector-protocol.md`, `docs/reports/A5-report.md` (discovery), `A4-report.md` (`udp_request`),
  `H1-report.md` (board profiles, DevKit memory measurements) and `devices/AGENTS.md`;
- the firmware: `firmware/main/lan_discover.c`, `lan_discover_core.c/.h`, `lan_udp.c`, `lan_dest.c`,
  `connector.c` (LAN worker, `metadata`), `memory_caps.h`, `Kconfig.projbuild`.

## Why

Goal: the Comma agent knows what is on the user's network, and what it can control, before the user
names a device. Today the user has to say "check whether there is a Gree air conditioner"; only then
does the agent try.

What we know (2026-10-12):
- `lan_discover` finds hosts through ARP, mDNS and SSDP. Devices that announce themselves (WLED, Shelly,
  ESPHome, HomeKit) are identified.
- Many home devices announce nothing. A Gree air conditioner or a Xiaomi miIO lamp is only `{ip, mac}`
  in the result. They do answer a small UDP datagram on their own port: Gree on 7000, miIO on 54321.
- The backend will run discovery by itself when a gateway comes online, identify devices and keep a
  device list for the agent. For that it needs each host's answer to a few protocol hello datagrams.

**The backend side is done** (Cue `f0a91d861`, `SalixAgent.LanProbes` and `Tools.LanGateway`): it sends
`udp_probes` only to a gateway whose `metadata` has `lan_gateway.discover_probes`, keeps within
`max_probes`, `max_payload_bytes`, `max_answer_bytes` and `max_total_answer_bytes`, sends probes only
when `"arp"` is among the sources, discards an answer marked `cut` or over the limits, and reads
`probes_truncated`. Today it sends two probes, `gree` (`{"t":"scan"}`, 12 bytes) and `miio` (32-byte
hello). Match those field names exactly; read its tests (`lan_gateway_tool_test.exs`) for the shapes.

The board must not learn any protocol. As with `udp_request`, **the cloud builds and parses the bytes;
the board only carries them**. The backend sends the list of probes with each `lan_discover` call, so a
new protocol needs no firmware release.

## Per-board limits

Every limit below is a Kconfig option in `Kconfig.projbuild`, with the T-Circle value as the shared
default and a DevKit value in `devices/sdkconfig.esp32-devkit` (comment the reason, as H1 did). The
values in the contract are written as `{T-Circle / DevKit}`; `metadata` advertises the board's own values.

| Kconfig | T-Circle | DevKit | Meaning |
| --- | ---: | ---: | --- |
| `COMMA_LAN_DISCOVER_PROBES` | 8 | 4 | max probes per call; 0 disables the feature and omits it from `metadata` |
| `COMMA_LAN_DISCOVER_PROBE_PAYLOAD` | 512 | 256 | max decoded payload per probe |
| `COMMA_LAN_DISCOVER_PROBE_ANSWER` | 1024 | 1024 | max stored bytes per answer |
| `COMMA_LAN_DISCOVER_PROBE_BUDGET` | 16384 | 4096 | all answers of one run, decoded |

Keep the answer cap at 1024 on DevKit: the backend discards an answer marked `cut`, and a Gree scan
reply (a JSON envelope around an encrypted base64 `pack`) can come close to 512 bytes. The DevKit
values are a starting point. Its whole discovery result is capped at 12 KiB
(`COMMA_LAN_DISCOVER_MAX_JSON`) and its largest free block is ~36 KB with TLS up; 4 KB of answers is
~5.5 KB of base64. Lower them if the board test needs it; do not raise the result cap.

## Contract

`lan_discover` gains one optional param and one optional result field. Without `udp_probes` the method
behaves exactly as it does now (same result bytes for the same network answers).

**Params** (new part):

```json
{"sources":["arp","mdns","ssdp"],"timeout_ms":5000,
 "udp_probes":[{"id":"gree","port":7000,"payload_b64":"eyJ0Ijoic2NhbiJ9"},
               {"id":"miio","port":54321,"payload_b64":"ITEAIP////////////////////////////////////8="}]}
```

- `udp_probes`: array of 1..{8 / 4} objects. Missing or `null` → no probes. Anything else that breaks
  the rules below → an `error` frame that names the rule, as `parse_params` does now.
- `id`: 1..16 characters of `[a-z0-9_]`, unique in the array. Echoed in the result.
- `port`: 1..65535, unique in the array (answers are matched to a probe by source port).
- `payload_b64`: standard base64, 1..{512 / 256} bytes decoded.
- `udp_probes` needs ARP: when `sources` is given without `"arp"`, answer with an `error`.
- On a board built with `COMMA_LAN_DISCOVER_PROBES=0`, `udp_probes` is an `error` (not silently ignored).

**Behaviour:**

- Every host that the ARP sweep confirms on the board's own subnet gets each probe **once, unicast**, as
  soon as the host is known. Never the board itself, the broadcast address or a multicast address. The
  caller cannot name destinations; that keeps this inside what `lan_discover` already allows.
- All probes go out from **one** UDP socket (ephemeral port) that also receives the answers. Do not open
  a socket per probe: `CONFIG_LWIP_MAX_SOCKETS` is 10 on both boards, a call and the Connector use 2,
  discovery already uses 3 (mDNS, SSDP, Yeelight), and a tunnel or an HTTP request may hold more.
- An answer counts only when its source address is a host that was probed and its source port is the
  port of a probe sent to that host. Everything else on that socket is dropped. Keep the **first** answer
  per (host, probe).
- Send again once, at least 500 ms after the first send, to (host, probe) pairs without an answer, if
  there is time before the listen-only tail of the run. UDP gets lost.
- Pace the sends inside the existing loop so they neither flood Wi-Fi nor starve a voice call. Choose
  the numbers, state them in the report with the worst case (64 hosts × 8 probes × 2 on T-Circle).
- Probes that could not be sent in time, or to hosts past the caps, set `truncated: true`. Add
  `"probes_truncated": true` as well, so the backend can tell it from host truncation.

**Result** (new part): a host that answered at least one probe carries

```json
"probes":[{"id":"gree","payload_b64":"<the answer datagram>"}]
```

- Left out when the host answered none. Order as in the request.
- An answer longer than 1024 bytes is cut to that size and its entry gets `"cut":true`.
- All probe answers of one run share a budget of {16 / 4} KB (decoded). Answers past it are dropped and
  set `truncated` and `probes_truncated`. The board's existing cap on the whole result
  (`COMMA_LAN_DISCOVER_MAX_JSON`: 48 KiB T-Circle, 12 KiB DevKit) still holds, with the H1 rule: a host
  that does not fit is rolled back whole.
- Answers are binary and go out as base64 only. Never log their bytes, and never put them in a string field.

**`metadata`:** add to `capabilities.lan_gateway`, with the board's values:

```json
"discover_probes":{"max_probes":8,"max_payload_bytes":512,"max_answer_bytes":1024,"max_total_answer_bytes":16384}
```

Leave it out when `COMMA_LAN_DISCOVER_PROBES=0` or discovery is disabled. The backend sends
`udp_probes` only to a gateway that advertises this, and keeps within its numbers. (Firmware before H2
ignores the param silently, so the backend cannot tell otherwise.)

The access gate stays as it is: a read-only connector refuses `lan_discover`, with or without probes.

## Do

1. Put everything that needs no network in `lan_discover_core.c`: parsing and validating `udp_probes`,
   the table of (host, probe) pairs sent and answered, matching an answer to a pair, the caps, the budget
   and the `probes` JSON. `lan_discover.c` only owns the socket, the timing and the sends.
2. Allocate new memory with `COMMA_BUFFER_CAPS` like the rest of discovery (PSRAM on T-Circle, internal
   on DevKit). No new allocation larger than the DevKit's measured largest block; prefer one table
   sized from the Kconfig caps, allocated once per run and freed at the end, over many small ones.
3. Extend `firmware/tools/lan_discover_test`, default and `--devkit` (read the new limits from the
   overlay): param validation (each rule above), answer from an unprobed host or a wrong port is dropped,
   a second answer is ignored, `cut`, the answer budget, `truncated`/`probes_truncated`, interaction with
   the 12 KiB result cap, and that a run without `udp_probes` gives the same result as before. Fuzz the
   answer path under ASan/UBSan as A5 did. Extend `firmware/tools/fake_connector.py` so it can send
   `udp_probes` (and reads `discover_probes` from metadata to stay within the limits).
4. Update `docs/connector-protocol.md` with the contract, including the per-board limits in `metadata`.
5. Build both boards with `tools/board.sh`. Show that the T-Circle generated config differs only by the
   new keys, and that DevKit stays within its flash and RAM.
6. On the board — Claude runs these on hardware after your commit; write the steps and expected log
   lines in the report so they can be repeated. Do not flash either board yourself.
   - Home Wi-Fi, both boards: the Gree air conditioner answers `gree` with JSON that contains
     `"t":"pack"`; hosts that are not Gree carry no `probes`.
   - miIO: the office Mi bedside lamp answers `miio` with a 32-byte datagram that starts `21 31 00 20`.
     This needs the office network; mark it pending if not run.
   - DevKit: three rounds of discovery with the maximum probes, with `CONFIG_COMMA_HEAP_DIAGNOSTICS`
     on in a test build only. Record min free, largest block and recovery after each round. No OOM.
   - T-Circle: discovery with 8 probes during a voice call; compare the `cue:` stats with a call without it.
   - Right after a discovery with probes run `http_request` and `udp_request` (both boards) and a
     `lan_tcp_dial` tunnel (T-Circle only), to show no socket is left open.
   - Report the log line per run; extend it with probes sent, answered and resent.

## Rules

- No backend, app or protocol change beyond the contract above. No merge, no push.
- Do not commit Wi-Fi passwords, tokens or test certificates.

## Not in this step

- TCP port probes.
- Listening for device broadcasts (Tuya on UDP 6666/6667). That is its own step, because it needs bound sockets.
- Vendor names from MAC prefixes. The backend does that from `mac`.

## Report

Write `docs/reports/H2-report.md`: the contract as implemented, per-board limits and why, pacing numbers,
memory and socket use on each board, test counts, the hardware test plan, and anything in this prompt
that the code made you change. Commit (don't push).

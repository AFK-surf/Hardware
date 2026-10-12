# H2 — Caller-supplied UDP probes in LAN discovery

2026-10-12. Base `9c73057b26c56318f11ec2c254855523d191a959` (H1 merged), branch
`codex/discover-probes`, worktree `~/Desktop/AFK/Hardware-h2`. ESP-IDF **6.0.2**.

Implementation, both firmware builds and host checks pass. **All H2 physical-board acceptance is
pending for Claude after this commit. Neither board was flashed.** No merge, push, backend/app
change, credentials or test certificates are included.

The Cue contract was read with `git show f0a91d861:<path>` for `lan_probes.ex`,
`tools/lan_gateway.ex` and `test/lan_gateway_tool_test.exs`. Cue's checkout was already newer than
that revision; it was neither checked out nor modified. Field names and capability gating match
that backend, including its refusal to interpret `cut` answers.

## Contract and implementation

See [Connector protocol](../connector-protocol.md#lan_discover-a5-h2) for the complete wire contract.

- Optional `udp_probes`: missing/null preserves the old result; otherwise a nonempty array within
  the board's limit. Probe objects require unique 1..16-character `[a-z0-9_]` ids, unique integer
  ports 1..65535, and nonempty standard padded base64 within the decoded payload cap. ARP must be
  among the sources. Invalid input returns an error identifying the rule before any network use.
- `lan_discover_core.c` owns all params, validation, decoded payloads, stable host/probe pair state,
  matching, caps, answer storage and JSON. `lan_discover.c` owns socket I/O and scheduling. There is
  no Gree/miIO interpretation or hardcoded vendor datagram in the firmware.
- Only ARP-confirmed hosts on the board's subnet are eligible. Self, network/broadcast, multicast,
  reserved/loopback, zero-net and off-subnet addresses are excluded. mDNS/SSDP alone do not authorize
  a send. Results still merge/sort by host IP as before.
- Exactly one additional ephemeral UDP socket handles every probe and answer. Source IP **and port**
  must match a successfully sent pair. Keep the first answer; ignore subsequent answers even if
  the first could not fit the budget. Zero-length answer datagrams are retained as empty base64.
- Answers longer than 1024 bytes are stored at 1024 with `cut:true`. Reading at most cap+1 into the
  existing receive buffer detects oversized datagrams without relying on `MSG_TRUNC` semantics.
  Answers exceeding the remaining total budget are dropped whole. Binary bytes never enter the
  text sanitizer or firmware logs; JSON contains base64 only.
- `probes` is omitted for hosts without stored answers and otherwise follows request order.
  `probes_truncated:true` accompanies `truncated:true` for cut/dropped answers, failed sends,
  unsent first attempts/retries, socket shortage, ARP sweep/host caps and JSON host rollback.
  The optional flag is omitted when false. Two successful sends followed by silence do not mark
  truncation. The final ARP harvest catches hosts discovered too late to probe.
- The 48/12 KiB result budgets remain unchanged. A host that cannot fit is rolled back whole,
  including its answers; all later hosts are omitted. The builder reserves room for both flags.
  No-probe runs retain the old footer reservation and exact output bytes.
- Metadata advertises the configured `discover_probes` limits only with discovery enabled and a
  nonzero probe count. A zero-count build rejects non-null probes; missing/null is still allowed.
  The existing read-only arrival/worker gates are unchanged.

## Limits, memory and sockets

| Setting | T-Circle | ESP32 DevKit |
| --- | ---: | ---: |
| `COMMA_LAN_DISCOVER_PROBES` | 8 | 4 |
| `COMMA_LAN_DISCOVER_PROBE_PAYLOAD` | 512 | 256 |
| `COMMA_LAN_DISCOVER_PROBE_ANSWER` | 1024 | 1024 |
| `COMMA_LAN_DISCOVER_PROBE_BUDGET` | 16384 | 4096 |
| Existing result cap | 49152 | 12288 |
| Probe allocation on target (bytes) | 26824 | 8304 |
| Discovery sockets, all sources + probes | 4 | 4 |

The target allocation sizes come from the linked ELF's DWARF `probe_state_t` size. The 64-bit host
tests report 26832 / 8312 due to `size_t` alignment. Pair state is 12 bytes per host/probe. One
`COMMA_BUFFER_CAPS` allocation holds descriptors, payloads, all 64 × configured-count pairs and
the answer budget; it is freed with the context on every exit. There is no allocation per answer
or send, no extra receive buffer and no temporary full base64 string. No-probe runs allocate none
of this table (only the context gains a pointer).

T-Circle places movable memory in PSRAM; DevKit uses internal RAM. Both new table sizes are below
H1's smallest observed 36864-byte largest block. DevKit's 4 KiB answers expand to about 5.5 KiB
base64, leaving space under the unchanged 12 KiB result cap. Its 1024-byte answer limit avoids
cutting larger Gree scan envelopes at 512 bytes. The chosen values remain the prompt's starting
values: successful linking and host tests do **not** establish live TLS heap headroom or recovery.

The metadata buffer grows from 1024 to 1280 bytes (+256 static bytes), allowing the new fields and
a maximally JSON-escaped device name. Other existing memory policies/limits are unchanged.

Both boards retain `CONFIG_LWIP_MAX_SOCKETS=10`. With all sources, discovery adds the previous three
sockets plus one probe socket. T-Circle with Connector and voice uses six nominal sockets total;
DevKit with Connector uses five. The shared LAN worker still serializes HTTP/UDP/discovery jobs.
The simulated runner checks that every successfully opened socket closes, including probe socket
failure and send failure paths. Physical post-discovery HTTP/UDP/tunnel checks remain pending.

## Pacing and retry policy

- At most **4 nonblocking send attempts per 25 ms**, at priority 2 below voice tasks; no catch-up
  bursts. Initial attempts take priority over retries. Every pair gets at most two attempts,
  including send failures; retry eligibility starts 500 ms after its first attempt.
- No probe sends in the final `min(timeout_ms / 4, 400)` ms. Receive before retry selection so queued
  answers suppress retries. Each socket drain handles at most 16 datagrams per loop, preventing
  a noisy discovery socket from indefinitely holding the worker past the deadline.
- T-Circle worst case: **64 × 8 × 2 = 1024 datagrams**, 256 batches, about **6.4 s** of send slots
  (6.375 s between the first/last slots if immediately available). DevKit: 512 datagrams, about
  3.2 s. ARP arrival delays, task scheduling and the 500 ms retry delay can add time. The runner
  simulation completes both full cases at an 8 s timeout, using a rolling 32-entry ARP cache.
- A 3 or 5 s run cannot promise the T-Circle maximum in the no-answer case; it returns truncation
  instead of speeding up. The backend's usual two small probes need much less traffic.
- Maximum payload rate from probing alone: T-Circle 2048 B per batch / about 80 KiB/s, DevKit
  1024 B / about 40 KiB/s, excluding UDP/IP/Wi-Fi overhead. The complete T-Circle no-answer worst
  case carries 512 KiB of probe payload. Voice impact still requires the physical comparison.

One summary line per run now looks like:

```text
lan_discover: req_…: H hosts (arp A of S swept, F failed; mdns M answers; ssdp D answers; probes P sent, Q answered, R resent), [truncated, ]T ms, B B
```

`sent` is successful datagrams including successful resends; `resent` is that subset; `answered`
counts first matched datagrams, including answers dropped by the storage budget. Counts contain
no payload bytes. Truncation is sampled after serialization, so the log includes JSON rollback.

## Build and config evidence

Fresh baseline builds from the requested main were captured in this worktree before edits. Both
final board sdkconfigs were regenerated from shared defaults + their overlay. Comparing every
`CONFIG_*` assignment and disabled key with the baseline gives **zero changed/removed existing
keys** on both targets; only these four keys are new:

```text
                                  T-Circle   DevKit
CONFIG_COMMA_LAN_DISCOVER_PROBES       8          4
CONFIG_COMMA_LAN_DISCOVER_PROBE_PAYLOAD 512        256
CONFIG_COMMA_LAN_DISCOVER_PROBE_ANSWER  1024       1024
CONFIG_COMMA_LAN_DISCOVER_PROBE_BUDGET  16384      4096
```

| Size, bytes | T-Circle main | T-Circle H2 | DevKit main | DevKit H2 |
| --- | ---: | ---: | ---: | ---: |
| Image reported by `size` | 2027111 | 2031111 | 1255458 | 1259462 |
| Flash code | 1512786 | 1515970 | 900914 | 904102 |
| Flash data | 350568 | 351384 | 237454 | 238270 |
| DIRAM (S3) / DRAM (ESP32) | 178745 | 179001 | 51531 | 51787 |
| Separate IRAM | 16384 | 16384 | 92215 | 92215 |
| External BSS | 33876 | 33876 | — | — |

Final padded binaries: T-Circle 2031232 bytes, DevKit 1259584 bytes. DevKit fits its `0x3f0000`
(4128768-byte) app partition, with 2869184 bytes remaining; static DRAM has 72793 bytes remaining
and IRAM 38857. Runtime heap is separate and must be measured below. Both builds have no compiler
warnings/errors. IDF emits existing upstream Kconfig notices. Lockfiles, managed dependency
versions, partitions, diagnostics-off setting and all other sdkconfig values are unchanged.

Commands used (with the installed IDF 6.0.2 environment):

```sh
tools/board.sh t-circle-s3 build
tools/board.sh t-circle-s3 size
tools/board.sh esp32-devkit build
tools/board.sh esp32-devkit size
```

Builds were sequential because managed components are shared. No dependency update command was run.

## Host checks

| Suite | Result |
| --- | --- |
| Discovery default, ASan/UBSan | 2339 core + 7476 runner checks, 0 failed |
| Discovery `--devkit`, ASan/UBSan | 2338 core + 4404 runner checks, 0 failed |
| Discovery `--no-probes`, ASan/UBSan | 1245 core + 11 runner checks, 0 failed |
| HTTP default / DevKit | 263 / 263, 0 failed |
| UDP default / DevKit | 747 / 747, 0 failed |
| TCP tunnel | 1367, 0 failed |
| Connector JSON allocation failures | 123, 0 failed |
| Python tools | 26 tests, all pass |

New checks cover every probe validation rule and limit boundary, binary round trips, source/port
matching before/after successful sends, duplicate-first semantics, exact/oversized/empty answers,
500 ms retry timing, skipped retries/late hosts, send/socket/allocation failure, host/answer/result
caps, whole-host rollback and missing/null backward compatibility. Each enabled profile runs
**20000 random answer datagrams with exact-sized input allocations**, in addition to the previous
20000 mDNS/SSDP fuzz inputs. The real runner executes against virtual time, ARP and socket calls:
one shared probe socket, send pacing, 8 s worst case, 1 s truncation, receive flooding and cleanup.

`fake_connector.py --discover-probes [JSON_FILE]` sends the built-in Gree/miIO hellos or a supplied
JSON array, constrained by all four advertised fields. It omits probes on older/disabled firmware,
checks answer bounds/order and writes the result under `--out`; logs include only host/probe ids,
byte counts and cut flags. Five new Python tests exercise these behaviors. Existing fake-board
socket `ResourceWarning` messages remain.

```sh
sh firmware/tools/lan_discover_test/run.sh
sh firmware/tools/lan_discover_test/run.sh --devkit
sh firmware/tools/lan_discover_test/run.sh --no-probes
python -m unittest discover -s firmware/tools -p 'test_*.py'
```

The system Python lacked `bleak`; the complete Python run uses the existing tools virtual
environment with the repository's dependencies. This did not change Cue or the main checkout.

## Hardware acceptance plan — all pending

Claude runs these after the commit. Use the existing H1 local-WSS setup and approved provisioning
procedure; leave credentials/certificates/generated configs out of Git. The DevKit was left
unprovisioned by H1. Use actual addresses from each test network, not addresses from older reports.

1. **Setup and access gate, each board.** Build a test image with the local Connector URL and CA
   overrides as in `devices/README.md`, then perform the separately authorized hardware setup.
   Verify `metadata … discover_probes={…}` matches the table above. With access off, plain and
   probing `lan_discover` must return `method "lan_discover" is not permitted for a local_file_read
   connector`, with no discovery run. Enable operations for subsequent tests.
2. **Home Gree, each board.** With the Mac and board on home Wi-Fi, run:
   ```sh
   python3 firmware/tools/fake_connector.py --discover-probes --discover-timeout 5000 \
     --loop 3 --pause 3 --out work/h2-home
   ```
   Expect three `PASS discovery` lines, `gree: <N> B, cut=False` on the Gree host, and three board
   summaries with `answered > 0`. Decode only the saved local JSON for verification: the Gree
   answer must parse as JSON with `t == "pack"`. Hosts without answers have no `probes`; non-Gree
   hosts should have no `gree` answer. A host answering another supplied protocol can have its own
   entry. Do not paste payloads into logs/reports. Record host/probe counts and byte lengths.
3. **Office miIO, each board — pending office network.** Repeat the command with an office output
   directory. The bedside lamp must yield `miio`, exactly 32 decoded bytes starting `21 31 00 20`,
   without `cut`. Record timing/counts, not the datagram. Absence on home Wi-Fi is not a failure.
4. **DevKit maximum load, three rounds.** Enable `CONFIG_COMMA_HEAP_DIAGNOSTICS=y` only in the
   generated test config, keep the 12 KiB result cap, and rebuild. Use the two real hellos plus two
   controlled lab responders on distinct UDP ports; provide a JSON array of four probe objects to
   `--discover-probes work/max4.json --discover-timeout 8000 --loop 3`. For maximum payload stress,
   use four lab responders with 256-byte payloads; for answer-budget stress, each returns 1024
   bytes. Keep these lab datagrams on a controlled network. Log `heap: lan_begin`,
   `heap: discovery_peak`, `heap: lan_result_ready`, `heap: lan_reply_freed` and idle `connector_stats`.
   Record per round: minimum-ever free, minimum sampled free, smallest sampled largest block,
   task stack watermark, and recovered free/largest after the reply is freed. Expect no OOM,
   panic, reboot, reconnect or accumulating heap loss; truncation is valid at documented caps.
5. **T-Circle voice comparison.** Record a baseline call with no discovery, then a comparable call
   with eight supplied probes (`max8.json`, unique ports; 512-byte lab payloads for the worst case)
   at 8 s. Keep the same AP/RSSI and similar speech/audio. Compare `cue:` stats over equivalent
   settled windows: `mic_dropped`, `send_max_ms`, `nrtx_max`, internal heap and audible gaps. Exclude
   startup from both comparisons consistently. Record the board's sent/answered/resent counts.
   Expect no new drop/spike/gap correlated with discovery; do not infer this from the host pacing test.
6. **Immediately after probing, both boards.** On the same Connector session, issue `http_request`
   to the Mac's local HTTP server, then `udp_request` to the just-verified Gree (home) or miIO lamp
   (office), using the same hello bytes and port. Example frames, replacing addresses:
   ```json
   {"id":"h2_http","type":"request","method":"http_request","params":{"method":"GET","url":"http://<mac-ip>:8000/","timeout_seconds":10}}
   {"id":"h2_udp","type":"request","method":"udp_request","params":{"host":"<gree-ip>","port":7000,"payload_b64":"eyJ0Ijoic2NhbiJ9","timeout_ms":2000,"max_responses":1}}
   ```
   Expect a normal HTTP response and `lan_udp: h2_udp … 1 answer(s)` without a socket error.
   On T-Circle after the call ends, immediately follow with `lan_tcp_dial` to the harness's TCP
   echo server on 7007, send/echo a line and close it; expect `connected`, echo and normal close.
   DevKit must still omit/refuse tunnel support. Repeat the full discovery→HTTP→UDP→tunnel sequence
   three times; preserve logs and recovery measurements. The harness's `Session.request`,
   `text_request` and `tunnel_echo` methods can drive these consecutive requests; `--tunnel`
   by itself runs its tunnel suite before discovery, so it is not this ordering check.
7. **Restore normal configuration.** Turn diagnostics off and clear both test URL/CA overrides,
   rebuild and follow the hardware owner's normal-image procedure. Compare generated configs
   with the normal profiles; record remaining failures/pending checks before accepting H2.

Record one line per physical run below when executed. Do not replace pending values with host
simulator or H1 measurements.

| Board / run | sent / answered / resent | min free / largest / recovery | Outcome |
| --- | --- | --- | --- |
| T-Circle home Gree ×3 | pending | pending | pending |
| DevKit home Gree ×3 | pending | pending | pending |
| Both boards office miIO | pending | pending | pending office Wi-Fi |
| DevKit max probes rounds 1 / 2 / 3 | pending | pending / pending / pending | pending |
| T-Circle 8 probes + voice vs baseline | pending | pending | pending |
| Post-discovery HTTP / UDP, both boards; TCP, T-Circle | pending | pending | pending |

## Clarifications from implementation

No requested field, limit, access rule or board feature changed. Standard base64 validation also
checks canonical pad bits. A failed send consumes an attempt and marks truncation; a skipped retry
also marks truncation, while two unanswered successful sends do not. JSON rollback during a
probing run conservatively marks probe truncation too. Limiting legacy socket drains to 16 packets
per loop prevents them from starving probe pacing/deadlines; normal no-probe result bytes remain
unchanged. The metadata buffer increase is the only static RAM growth.

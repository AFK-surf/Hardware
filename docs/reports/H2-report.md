# H2 — Caller-supplied UDP probes in LAN discovery

2026-10-12. Base `9c73057b26c56318f11ec2c254855523d191a959` (H1 merged), branch
`codex/discover-probes`, worktree `~/Desktop/AFK/Hardware-h2`. ESP-IDF **6.0.2**.

Implementation, both firmware builds and host checks pass. Claude's DevKit test of `bc883dd`
received an uncut Gree reply but found BLE-up discovery OOM. **The per-host cap fix below awaits
Claude's BLE-up retest. Neither board was flashed during implementation or this fix.** No merge,
push, backend/app change, credentials or test certificates are included.

The Cue contract was read with `git show f0a91d861:<path>` for `lan_probes.ex`,
`tools/lan_gateway.ex` and `test/lan_gateway_tool_test.exs`. Cue's checkout was already newer than
that revision; it was neither checked out nor modified. Field names and capability gating match
that backend, including its refusal to interpret `cut` answers.

## BLE-up DevKit follow-up

Hardware evidence supplied after `bc883dd`, using local WSS and home Wi-Fi:

- The Gree AC at `192.168.3.18` answered probe `gree`: **399 bytes, not cut**. Keep the DevKit
  answer cap at **1024**; the backend discards cut answers.
- Provisioning with the app, or holding BOOT for 3 s, then calling `lan_discover` with BLE still
  active returned discovery OOM, both with and without probes. This predates H2: H1 discovery
  tests ran after BLE shut down. Backend discovery on gateway connection exposes the setup window.
- At `lan_begin`, BLE up: **93040 bytes free, 51200 largest block**. BLE off: largest block about
  **57 KB**. These are pre-fix observations, not acceptance measurements for the new caps.

`COMMA_LAN_DISCOVER_MAX_SERVICES` and `COMMA_LAN_DISCOVER_MAX_SSDP` now default to **16/16**;
the DevKit overlay selects **6/6**. Both values have a Kconfig range of 1..16. Existing per-host
overflow handling omits excess entries and sets `truncated:true`, with or without probes.
The backend already caps each array at 16, so no backend or wire-shape change is needed.
T-Circle keeps its existing limits and behavior; DevKit's 1024-byte probe answer and 12 KiB result
caps are unchanged.

### Target allocation measurements

Measured with **Xtensa GCC 15.2.0 (esp-15.2.0_20251204), ESP-IDF 6.0.2**, using each board's
actual `compile_commands.json` flags and generated `sdkconfig.h`: compile a scratch translation
unit including `lan_discover_core.c` and `lan_discover.c`, export a constant `uint32_t` array of
`sizeof(host_t)`, `sizeof(host_t) * LD_MAX_HOSTS`, `sizeof(ld_ctx_t)`, `ARENA_BYTES`,
`sizeof(probe_state_t)`, `RECV_BUF` and `sizeof(arp_dump_t)`, then read its ELF section. These are
32-bit target compiler values, not the Mac host-test ABI or a runtime heap measurement.

| Allocation, bytes | T-Circle (unchanged) | DevKit before | DevKit 6/6 |
| --- | ---: | ---: | ---: |
| `sizeof(host_t)` (one entry) | 532 | 532 | **212** |
| Host table, 64 entries | 34048 | 34048 | **13568** |
| Context | 5560 | 5560 | 5560 |
| String arena | 98304 | 16384 | 16384 |
| Probe state, only when requested | 26824 | 8304 | 8304 |
| Receive buffer | 2048 | 2048 | 2048 |
| ARP dump | 396 | 396 | 396 |
| **Fixed total without probes** | **140356** | **58436** | **37956** |
| **Fixed total with probes** | **167180** | **66740** | **46260** |
| Maximum result buffer, including NUL | 49153 | 12289 | 12289 |
| Fixed total with probes + maximum result | 216333 | 79029 | 58549 |

The before column uses the reported 532-byte host and unchanged target-measured allocations.
The reduction is **20480 bytes (20 KiB)**, all from the host table. The largest fixed DevKit
allocation is now the 16384-byte arena. The result buffer coexists with the fixed allocations
during serialization; the context, receive buffer and ARP dump are then freed before wrapping the
result in the response envelope. Totals exclude allocator overhead, temporary old/new buffers
during `realloc`, parsed request/cJSON storage, socket/lwIP buffers and the existing LAN task stack.
They must not be treated as measured peak heap use or proof that BLE-up discovery now succeeds.

### Follow-up validation

- Both normal firmware profiles build successfully with IDF 6.0.2, without compiler warnings.
  T-Circle adds only the two new `16` config keys: no existing generated setting changes.
  Its image, flash code/data and static RAM sizes match `bc883dd` exactly.
- DevKit was built separately in `firmware/build-esp32-devkit-h2-caps/`, from the checked-in
  normal defaults/overlay with diagnostics off. The existing DevKit test `sdkconfig` and cached
  local WSS URL/CA settings were preserved for Claude. Relative to that diagnostics-enabled test
  config, only the two new `6` keys and the normal diagnostics-off setting differ.
- T-Circle image: **2031111 bytes**, padded binary **2031232**; DevKit image: **1259450 bytes**
  (12 fewer than `bc883dd`), padded binary **1259568**. DevKit flash code is 904090 bytes;
  flash data 238270, DRAM 51787 and IRAM 92215 are unchanged. Size/component reports were checked;
  voice components remain confined to T-Circle, with no dependency/lockfile changes.
- Discovery ASan/UBSan: default **2360 core + 7476 runner**, `--devkit` **2359 + 4404**,
  `--no-probes` **1266 + 11**, all zero failures. New checks independently cover exact service
  and SSDP caps, duplicates at the cap, cap+1 truncation and allowances for a second host.
  The oversized-first-host fixture now includes escaped TXT so it still exceeds 12 KiB at 6/6;
  the probe fixture checks that a complete 6-entry host fits and later hosts roll back whole.
- HTTP default/DevKit **263/263**, UDP **747/747**, tunnel **1366**, Connector JSON **123** checks,
  all pass. Python **26 tests** pass (the existing fake-socket ResourceWarnings remain).

**Pending for Claude:** retest discovery with BLE up, both with and without `udp_probes`, after
app provisioning and after BOOT 3 s. Confirm the generated new caps are 6/6; keep probe answers
at 1024. Record discovery success, heap/largest-block recovery and the real Gree answer.
This implementation task did not flash or run hardware tests.

## Contract and implementation

See [Connector protocol](../connector-protocol.md#lan_discover-a5-h2) for the complete wire contract.

- Optional `udp_probes`: missing/null preserves the old result within the current board caps;
  otherwise a nonempty array within
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
  *Superseded by H2b (below):* `probes_truncated` alone now reports cut/dropped answers, unsent
  pairs, omitted retries and socket shortage; `truncated` keeps its pre-H2 meaning, and both are
  set only for ARP sweep/host caps and JSON host rollback.
  The optional flag is omitted when false. Two successful sends followed by silence do not mark
  truncation. The final ARP harvest catches hosts discovered too late to probe.
- The 48/12 KiB result budgets remain unchanged. A host that cannot fit is rolled back whole,
  including its answers; all later hosts are omitted. The builder reserves room for both flags.
  No-probe runs retain the old footer reservation and exact output bytes within the current caps.
- Metadata advertises the configured `discover_probes` limits only with discovery enabled and a
  nonzero probe count. A zero-count build rejects non-null probes; missing/null is still allowed.
  The existing read-only arrival/worker gates are unchanged.

## Limits, memory and sockets

| Setting | T-Circle | ESP32 DevKit |
| --- | ---: | ---: |
| `COMMA_LAN_DISCOVER_MAX_SERVICES` | 16 | 6 |
| `COMMA_LAN_DISCOVER_MAX_SSDP` | 16 | 6 |
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
a maximally JSON-escaped device name. Other than the per-host cap follow-up above, existing
memory policies/limits are unchanged.

Both boards retain `CONFIG_LWIP_MAX_SOCKETS=10`. With all sources, discovery adds the previous three
sockets plus one probe socket. T-Circle with Connector and voice uses six nominal sockets total;
DevKit with Connector uses five. The shared LAN worker still serializes HTTP/UDP/discovery jobs.
The simulated runner checks that every successfully opened socket closes, including probe socket
failure and send failure paths. Physical post-discovery HTTP/UDP/tunnel checks remain pending.

## Pacing and retry policy

- At most **4 nonblocking send attempts per 25 ms**, at priority 2 below voice tasks; no catch-up
  bursts. Initial attempts take priority over retries. Every pair gets at most two successful
  sends; retry eligibility starts 500 ms after its first successful send. (H2b: a failed send no
  longer consumes an attempt; it is retried 50 ms later, after untried pairs.)
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
lan_discover: req_…: H hosts (arp A of S swept, F failed; mdns M answers; ssdp D answers; probes P sent, Q answered, R resent, X send_failed, U unsent)[, truncated][, probes_truncated], T ms, B B
```

(H2b format; `send_failed`, `unsent` and `probes_truncated` were added.)

`sent` is successful datagrams including successful resends; `resent` is that subset; `answered`
counts first matched datagrams, including answers dropped by the storage budget. Counts contain
no payload bytes. Truncation is sampled after serialization, so the log includes JSON rollback.

## Initial H2 build and config evidence (`bc883dd`)

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

## Initial H2 host checks (`bc883dd`)

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

## Remaining hardware acceptance plan

Claude runs these after the commit. Use the existing H1 local-WSS setup and approved provisioning
procedure; leave credentials/certificates/generated configs out of Git. The DevKit Gree response
and BLE-up failure above are recorded evidence; the repeated acceptance runs below remain pending.
Use actual addresses from each test network, not addresses from older reports.

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

## Clarifications from initial implementation (`bc883dd`)

No requested field, limit, access rule or board feature changed. Standard base64 validation also
checks canonical pad bits. (Initial behavior, changed in H2b: a failed send consumed an attempt and
marked both flags.) A skipped retry marks probe truncation, while two unanswered successful sends
do not. JSON rollback during a
probing run conservatively marks probe truncation too. Limiting legacy socket drains to 16 packets
per loop prevents them from starving probe pacing/deadlines; normal no-probe result bytes remain
unchanged. The metadata buffer increase is the only static RAM growth.

## H2b — DevKit discovery during BLE setup, and honest `truncated`

2026-10-12, on `b0c3d9b`, same branch and worktree, IDF 6.0.2. Neither board was flashed. No
protocol change beyond the meaning of the two flags, and no app or backend change.

### 1. The failing allocation

The failure was inside `ld_result_json`. The old log printed `0 B` because `result` was NULL, and
the only allocation in that function is the result builder's `heap_caps_realloc` growth
(1024 → 2048 → 4096 → 8192 → 12289 bytes, `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` on the DevKit).
Nothing was cut by the 12 KiB cap. That realloc found no 8-bit-capable block, and the "36 KB free"
was never usable for it:

- `comma_heap_log` reported `MALLOC_CAP_INTERNAL`. On the classic ESP32 that includes the
  **IRAM heap**, which is 32-bit-only, executable memory (`MALLOC_CAP_INTERNAL|EXEC|32BIT`, IDF
  `heap/port/esp32/memory_layout.c`). Byte buffers cannot come from it.
- In the DevKit map, `_iram_end = 0x40096838`. The IRAM heap is therefore `0x400A0000 − 0x40096838 =
  38,856` bytes, which TLSF's fit-size reports as a **36,864**-byte largest block. That is exactly the
  `largest=36864` seen in every round, with BLE on and off, because nothing ever allocates there.
- So at round 1's `discovery_peak` (`free=45904`), only about **7 KB** of the free memory was
  8-bit-capable DRAM. Discovery itself was still holding about 46 KB at that point: the context with
  its mDNS tables (~5.6 KB), hosts (13,568), arena (16,384), probe table (8,312), receive buffer
  (2,048) and ARP dump. The builder's next growth step (to 8 or 12 KiB) could not fit. With BLE off
  (round 3), DRAM had about 46 KB 8-bit free and the same steps succeeded.

The exact failing step was not captured on hardware. The new hook records it (see below), so the
retest will show it if it still happens.

### 2. What changed

**Diagnostics.** `comma_heap_log` now also prints `free8=` / `largest8=`
(`MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT`) and explains the IRAM caveat. The failed-allocation hook
in `connector_json.c` logs each failure while `comma_log_failed_allocs(true)` is set for the
calling task. `lan_discover_run` turns this on from context creation until the result is built.
It uses the ROM printer, so the hook never allocates. Format:
`W (…) heap: heap_caps_realloc(12289) failed, caps 0x1804: free N, largest M for those caps`.
This works with heap diagnostics off.

**Memory, a real fix independent of BLE (`ld_result_frame`).** The old path built the result by
doubling (needing 8 KiB + 12 KiB at once during the last realloc), then copied it into a separate
reply envelope that doubled again to 16 KiB. For round 3's 10,322-byte result, the peak just for the
reply was about **36.9 KB** (12,289 result + 8,192 old + 16,384 new envelope buffer), with a 16 KiB
contiguous block required. Now:
- the receive buffer and ARP dump are freed before the result is built;
- the result is **measured first** (a counting builder), then written **once** into **one exactly
  sized allocation** that also holds the reply envelope (`{"id":…,"type":"response","result":` +
  result + `}`). Round 3's reply would need one ~10.4 KB block and no transient copy;
- the envelope prefix is built in a ≤448-byte stack buffer (a 64-byte id escapes to at most 386),
  so its builder is freed before the result allocation.
The 12 KiB cap still applies to the result object alone, and output bytes are unchanged.

**BLE timing (`CONFIG_COMMA_FREE_BLE_FOR_DISCOVERY`).** Off by default; T-Circle is unchanged. The
DevKit overlay enables it.
- (a) After `saved`, when provisioning notifies `connector_up` over BLE, it records the time and
  ends BLE **3 s later** (`ble_down`, which terminates the link) instead of waiting up to 30 s for
  the phone to disconnect. The setup order is unchanged: the Connector still starts while BLE is
  up, so `connector_failed` is still reported, and only `connector_up` ends BLE early.
- (b) `lan_discover_run` checks `provisioning_ble_on()` before allocating anything. That flag is
  true from `nimble_port_init` until `ble_down` has finished, which is earlier than
  `provisioning_active()` (that one waits out the 2 s "All set" screen). If BLE is on, discovery
  waits in 100 ms steps, at most `min(10 s, 15 s − 2 s − timeout_ms)`. The backend gives a
  `lan_discover` request 15 s (Cue `salix_env/protocol.ex`, `"lan_discover" => 15_000`), and so does
  `fake_connector.py`. With the default 3 s window it waits up to 10 s; with 5 s, 8 s; with 8 s, 5 s.
  If BLE is still on after that, discovery runs anyway and fails as before.

**Why ending BLE after `connector_up` is safe for the app** (`comma-android`, branch
`codex/add-device-ble` at `bc50372`, `AddDeviceFlow.kt`; `main` has no BLE setup flow yet). After
`saved`, the flow is in `SetupPhase.COMING_ONLINE`. In that phase only `connector_failed` (→ failure)
and `Online` are handled. `Online` comes from `WatchOnline`, which polls the server's device list, not
BLE. `LinkLost` and `connector_up` fall through to `else -> none`, and the code comment there already
says "the device closes it within 30 s". If `saved` was never heard, the 20 s `SAVE` timer also
moves to `COMING_ONLINE`, and `LinkLost` in `SAVING` does the same. Because `connector_up` is only
sent after a successful dial with the saved token, `connector_failed` cannot follow it within the
3 s window, so no refused token goes unreported. The device simply leaves after the app has
everything it needs, and the app finishes with `Done(online = true)` once the server sees the
device.

### 3. Flag semantics and send failures

- `truncated` keeps its pre-H2 meaning: a host (64 cap or ARP sweep of a large subnet), service,
  SSDP entry, TXT key or string was dropped, or a host was rolled back at the result cap.
- `probes_truncated` alone covers: a pair still unsent at the end (including hosts found only in the
  final ARP harvest), an unanswered pair whose retry did not fit, no probe socket, a cut answer, or
  an answer over the total budget. `ld_mark_probes_truncated` no longer sets `truncated`.
- Both are set when a host is dropped at the host cap or rolled back at the result cap.
- A failed `sendto` no longer consumes an attempt. The pair is marked failed and retried no sooner
  than 50 ms later (`LD_PROBE_FAIL_RETRY_MS`), on a later tick, for as long as the send window lasts.
  Untried first sends go before failed first sends, which go before resends, so a pair that keeps
  failing cannot starve the others. The 500 ms resend delay counts from the first *successful*
  send. Each pair still gets at most two successful datagrams. Pacing (4 per 25 ms) and the
  listen-only tail are unchanged.
- `docs/connector-protocol.md` now defines both flags this way.

Round 3's 22 expected first sends with only 20 sent is now visible directly. Either sends failed
(`send_failed > 0`, plus the first errno), or hosts arrived too late to probe (`unsent > 0` with
`send_failed = 0`, typically ARP replies seen only at the final harvest). Only `probes_truncated`
is set for those.

New summary and error lines:

```text
lan_discover: <id>: H hosts (arp A of S swept, F failed; mdns M answers; ssdp D answers; probes P sent, Q answered, R resent, X send_failed, U unsent)[, truncated][, probes_truncated], T ms, B B
lan_discover: <id>: first failed probe sendto: errno E        (only when X > 0; -1 = short send)
```

`B` is now the whole reply frame (about 50 bytes more than the old result-only figure).

### Memory numbers

The probe table grows by 8 bytes (two counters): 8,312 B on DevKit, 26,832 B on T-Circle (target).
The pair state stays at 12 bytes; a 16-bit `next_ms` replaces the old attempt-time meaning. The
reply peak drops as described in section 2. No other buffer or cap changed.

### Tests and builds

| Suite | Result |
| --- | --- |
| `lan_discover_test/run.sh` (default) | 2387 checks + runner 7574 checks, 0 failed (ASan/UBSan) |
| `lan_discover_test/run.sh --devkit` | 2386 + runner 4502, 0 failed |
| `lan_discover_test/run.sh --no-probes` | 1276 + runner 24, 0 failed |
| `connector_json_test` | 133 checks, 0 failed |
| `lan_http_test` default / `--devkit` | 263 / 263 passed |
| `lan_udp_test` default / `--devkit` | 747 / 747 checks |
| `lan_tunnel_test` default / `--devkit` | 1367 / 1368 checks |
| Python `unittest discover -s firmware/tools` | 26 tests OK (interpreter: the main checkout's `tools/.venv`; this worktree has none) |
| `tools/board.sh t-circle-s3 build` / `esp32-devkit build` | both OK |

New host checks:
- failed send: retried after 50 ms, never twice in one tick, resend timed from the successful
  send, failed resend retried, a send failing until the window ends gives `unsent = 1` and only
  `probes_truncated`;
- fairness: untried before failed before resends, and two always-failing hosts do not stop eight
  others from getting both sends;
- `truncated` vs `probes_truncated` for: late host, missing retry, cut/over-budget answers, socket
  shortage (`probes_truncated` only); host cap and JSON rollback (both); two unanswered sends
  (neither);
- result frame: exactly one allocation of exactly `strlen + 1`, bytes equal prefix + result +
  suffix, the cap applies to the result and not the envelope, and an allocation failure gives NULL;
- BLE wait: the deadline-bounded limit, return as soon as BLE is off, give up exactly at the limit,
  and a zero limit never sleeps. In the runner, with the option compiled in: BLE ending at 2.95 s
  gives a 3.0 s wait and then a normal scan; BLE never ending with `timeout_ms` 5000 gives an 8 s
  wait and then a scan. Failed-allocation logging is off again after every run.
- the failed-allocation hook logs only for the task that enabled it, never from an interrupt or
  another task, and queries free/largest for the failed caps.

Generated config: the T-Circle `sdkconfig` differs from before only by
`# CONFIG_COMMA_FREE_BLE_FOR_DISCOVERY is not set`. The DevKit build directory gains
`CONFIG_COMMA_FREE_BLE_FOR_DISCOVERY=y` (it needed `board.sh esp32-devkit reconfigure` to pick up the
new key). That local build directory still carries `CONFIG_COMMA_HEAP_DIAGNOSTICS=y` from the
previous hardware test; the overlay keeps it `n`.

### Hardware retest steps (for Claude)

Build the DevKit test image as for the `b0c3d9b` test: local WSS URL/CA overrides and
`CONFIG_COMMA_HEAP_DIAGNOSTICS=y` in the generated config only. Check that the generated config
has `CONFIG_COMMA_FREE_BLE_FOR_DISCOVERY=y`, then flash.

**A. Setup from the app, then discovery right away (BLE up).** Factory reset (hold BOOT 10 s) and
set up from the app. Have the fake Connector call `lan_discover` with `gree` + `miio` and
`timeout_ms` 5000 as soon as the board connects (as before, e.g.
`fake_connector.py --discover-probes --discover-timeout 5000 --loop 3 --pause 10`). Expect, in
this order:

```text
prov: status {"state":"connector_up",...}
prov: connector_up sent, BLE ends in 3 s
heap: lan_begin free=… largest=… free8=… largest8=…        (still BLE-up numbers)
lan_discover: <id>: waiting up to 8000 ms for BLE to end
prov: ending BLE after connector_up to free memory for discovery
prov: disconnected, reason 0x216                            (local terminate)
heap: ble_off …
prov: BLE off
lan_discover: <id>: BLE off after ~2000-3000 ms
heap: discovery_peak … free8≈ BLE-off level
lan_discover: <id>: 11-12 hosts (…; probes N sent, 3 answered, R resent, X send_failed, U unsent)…, ~5000 ms, ~10400 B
```

There should be no `out of memory for the discovery result` and no `heap: … failed` line. The app
should finish setup normally (device online), with no failure screen and no "disconnected" error.
If a `heap: … failed` line does appear, record its size, caps and largest: that identifies the
allocation.

**B. Three rounds with BLE off** (the next two rounds of the same loop, or a fresh run 30 s after
boot): three PASS, the Gree answer uncut (`t == "pack"`), sizes about 10.4 KB, and
`discovery_peak` `largest8` comfortably above the frame size. Record `free8`/`largest8` at
`lan_begin`, `discovery_peak` and `lan_reply_freed`.

**C. New counts and flags.** In every summary, `sent = first sends + resent`. Expect `truncated` to
be absent unless a real cap was hit: 64 hosts, or a host with 6 services or 6 SSDP entries
(DevKit caps). Check the saved JSON before calling it a bug. `probes_truncated` may appear: if so,
`unsent > 0` (hosts too late, or sends still failing at the end) or a pair was answered neither
time with its retry outside the window. When `send_failed > 0`, the
`first failed probe sendto: errno E` line follows; record `E` (12 = ENOMEM, 11 = EAGAIN). In the
saved result, `"truncated":false` with `"probes_truncated":true` is now a valid combination, and
the backend still reads both.

**D. T-Circle** behavior is unchanged except for the flag meanings and the log fields; one home
Gree round is enough to confirm.

## Hardware results — 2026-10-12 (Claude, `d58b744`)

Home Wi-Fi, three Gree units on the LAN. Test builds dial a local fake Connector (WSS, test CA) with
`CONFIG_COMMA_HEAP_DIAGNOSTICS=y`; both boards were then flashed back to normal builds (default URL,
diagnostics off).

**DevKit**

| Run | BLE | Result |
| --- | --- | --- |
| 3 rounds after boot | off | PASS: 11–12 hosts, 3 Gree answers (399 B, not cut), 8.9–10.5 KB; `send_failed` 0–4 (errno 12), `unsent` 0, `probes_truncated` false |
| setup from the app, discovery right after `connector_up` | on | PASS: `waiting up to 8000 ms for BLE to end` → BLE ended by the 3 s timer → `BLE off after 2900 ms`; 12 hosts, 3 Gree, 9.9 KB. `free8` 55 KB at `lan_begin`, 100 KB after BLE off, 51 KB at `discovery_peak` |
| 2 more rounds after that | off | PASS |
| normal build, setup from the app, real backend (staging) | on, then app disconnects 2.3 s after `connector_up` | app reached Done; `udp_request` to a Gree unit answered |
| normal build, "scan the home network" in the app | off | discovery ran, no OOM, but `probes 0 sent`: the backend did not send `udp_probes` (backend projection bug, Cue #3515, not firmware) |

`truncated` is still set on DevKit, honestly: the router has 6 SSDP entries and three hosts reach 6
services (DevKit 6/6 caps). On the T-Circle it comes from AirPlay TXT records over 16 keys (as in A5).

**T-Circle** (`max_probes` 8, budget 16 KB)

- 3 rounds with `gree` + `miio`: PASS, 12 hosts, 3 Gree answers, 12.0–12.7 KB, `probes_truncated` false.
- Right after discovery, 2 rounds of: `lan_tcp_dial` echo 1 MB each way, 1 MB pull, refused dial,
  `http_request` 1 MB in chunks, text mode: all PASS, second round equal to the first (no socket left
  open). `udp_request` was not covered by the fake Connector; covered on DevKit through staging.
- Voice call, without and with discovery (8 probes: `gree`, `miio`, six 512-byte probes to unused
  ports) every ~8 s, 205 probe datagrams per run, 7 runs during the call. User heard no stutter.

| `cue:` stats | no discovery (91 s) | 8-probe discovery (60 s) |
| --- | --- | --- |
| `send_max_ms` avg / max | 74 / 500 | 62 / 161 |
| `queue_ms` avg / max | 501 / 1909 | 208 / 380 |
| `mic_backlog_ms` avg / max | 70 / 460 | 62 / 380 |
| `mic_dropped` total | 15 (one burst) | 0 |
| `nrtx_max` | 0 | 0 |
| cpu0 / cpu1 avg | 38 / 45 % | 47 / 52 % |

Discovery runs during the call took 5017–5046 ms (about 5013 ms without a call), with 0
`send_failed` and 0 `unsent`.

**Pending:** an end-to-end discovery with probes through staging once Cue #3515 is deployed; miIO on
the office network.

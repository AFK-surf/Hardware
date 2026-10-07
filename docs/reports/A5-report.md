# A5 report: `lan_discover` (LAN discovery) on the T-Circle

2026-10-06 · `t-circle-s3/main/lan_discover.c/.h`, `lan_discover_core.c/.h` (new), `connector.c`, `main/CMakeLists.txt`, `CMakeLists.txt`, `sdkconfig(.defaults)`, `tools/lan_discover_test/` (new) · **not yet on the board** (see *Not done*)

## Result

| Check | Result |
|---|---|
| Port of Muse's `net_discovery.c` (ARP sweep + harvest, mDNS, SSDP M-SEARCH, bounded results, ASCII sanitising) | **Done.** Apache-2.0 header kept on both files that carry ported code, with a note on what changed. |
| `lan_discover` with the agreed contract | **Done.** Params, result shape, caps and `truncated` exactly as in the prompt (details below). Checked by host tests; no other keys in the result. |
| Sanitising | **Done.** Every LAN string: printable ASCII only; tab, CR and LF become spaces; other bytes dropped; ≤ 255 bytes. Fuzzed (20 000 random datagrams, every prefix of a valid one) under ASan/UBSan. |
| `lan_gateway.methods` | **Done.** `["http_request","lan_discover"]`. |
| Access gate (read-only refuses) | **Done, same path as A3.** Checked when the request arrives and again when the worker picks it up. Refusal: `method "lan_discover" is not permitted for a local_file_read connector`. Not exercised on the board. |
| Yeelight discovery | **Done.** `239.255.255.250:1982`, `ST: wifi_bulb`; answers appear in `ssdp` with `st: "wifi_bulb"` (Yeelight's answer has no ST header). Host-tested with a canned Yeelight answer; no bulb to try it on. |
| Real-world answers parse | **Verified on the Mac**, not the board. The firmware's own mDNS questions, sent from the Pro, got real answers from macOS; replayed through the firmware's parser they give the expected host (hostname, AirPlay/RAOP/companion-link with ports and TXT). See *Checked against real answers*. |
| Host tests | `tools/lan_discover_test/run.sh`: **1221 checks, all pass** (ASan + UBSan). `lan_http_test` still 99/99. |
| Build | Clean, no warnings. Static DIRAM 164.4 → **165.3 KB** (+0.9 KB: ARP table 10 → 32 entries and the code); image 1.39 MB. |
| **On office Wi-Fi: Mac and router listed, within caps** | **Verified 2026-10-07** (see *On the board*). Self-test build: 13–15 hosts, ~8 KB, router with 11 SSDP entries, the Mac with its services, the Mi bedside lamp (`_hap._tcp:80`). |
| **Through Comma (staging, backend #2619–#2621)** | **Verified.** "用 T-Circle 扫描局域网" from the phone → `lan_discover` reached the board, 16 hosts in 8007 ms, 6161 B; the agent listed them. Read-only gate seen first: self-tests refused until Allow operations was turned on. |
| **Voice call during discovery (`cue:` stats)** | **Not done.** |

## Contract as implemented

```
params  {sources?: ["arp","mdns","ssdp"], timeout_ms?: 1000..8000}
result  {hosts: [{ip, mac?, hostname?, sources: [...], services: [{type, name, port, txt?}],
                  ssdp?: [{server, location, st}]}], truncated: bool}
```

- **Params.** Missing or `null` `sources` → all three. An empty array, a non-array or an unknown name → `error` (`'sources' may only contain "arp", "mdns" and "ssdp"`). `timeout_ms` missing → 3000; a number outside 1000..8000 is clamped; a non-number → `error`. `params` itself may be missing.
- **`hosts`** sorted by address. The device itself, 0.0.0.0, loopback and multicast are never listed.
- **`sources`** always in the order `arp`, `mdns`, `ssdp`, only those that saw the host.
- **`mac`** only from ARP. **`hostname`** from the SRV target of one of the host's services (`mafuzhens-MacBook-Pro.local`), otherwise from an mDNS A record or a reverse (`in-addr.arpa`) answer for its address.
- **`services`** is always present (possibly `[]`). `type` without `.local` (`_wled._tcp`); `name` is the instance label (it may contain dots); `port` is 0 when no SRV record arrived. `txt` is an object: `key=value` → `"key":"value"`, a bare `key` → `"key":""`, the first of a repeated key wins (RFC 6763 §6.4); left out when the record is empty.
- **`ssdp`** is an array, left out when empty; entries de-duplicated on `(st, location)`.
- **Caps:** 64 hosts, 16 services per host, 16 `ssdp` entries per host, 16 TXT keys, 255 bytes per string. Anything cut sets `truncated: true`, including a subnet larger than /24 (only 254 addresses around the device's own are swept) and running out of sockets.
- **One more cap, not in the contract:** the result JSON stays under **48 KB**. Whole hosts past that are left out (`truncated: true`). 64 hosts × 16 services with full TXT could otherwise reach megabytes. Host tests fill 64 hosts with 16 maximal SSDP entries each; the result stays under 48 KB.

**For the backend:** expect `truncated: true` on most real networks. Any Mac or Apple TV advertises AirPlay with more than 16 TXT keys (the Pro has 17), and that alone sets it. It is still the right signal per the contract, but don't read it as "hosts were missed". If you'd rather have a flag for that, tell me and I'll split it.

## How it works

All three probes run **at once** for `timeout_ms`, unlike Muse, which runs them one after another (over 10 s for ARP, mDNS and SSDP alone with its defaults). The call returns after ≈ `timeout_ms`. The server waits up to 30 s for non-HTTP methods, so 8 s is safe.

| Time (fraction of `timeout_ms`) | What happens |
|---|---|
| 0 | mDNS: one PTR question per packet for 17 service types (Muse's list plus `_miio._udp`, `_wled`, `_esphomelib`, `_shelly`, `_ipp`, `_printer`, `_companion-link`, `_spotify-connect`) and the DNS-SD meta query `_services._dns-sd._udp.local`. SSDP `ssdp:all` (MX = timeout − 1 s, 1–5). Yeelight search. |
| 0 – 55 % | ARP sweep of the own subnet in even batches every 50 ms (a /24 at 3000 ms: 9 addresses per batch). The ARP table is read before each batch. |
| any time | A service type learnt from the meta query that wasn't asked yet gets its own question. |
| ⅓ | SSDP and Yeelight searches repeated once (UDP gets lost). |
| 55 % + 150 ms | Reverse mDNS questions (`x.x.x.x.in-addr.arpa`, 12 per packet) for every host still without a name. Computers answer these; small devices usually don't. |
| last 25 % (≤ 400 ms) | No new questions; listening only. |
| end | Last ARP read; sockets closed; result built. |

Details:

- **ARP.** This build has no lwIP core locking, so `etharp_request` and `etharp_get_entry` run on the tcpip thread through `tcpip_api_call` (as `cue_voice.c` does for its TCP probe). Muse uses `LOCK_TCPIP_CORE`, which would be wrong here.
- **ARP table 10 → 32 entries** (`ARP_TABLE_SIZE=32` on the lwip component in the top `CMakeLists.txt`; IDF 6.0.2 has no Kconfig for it). With 10, a batch of replies pushes out the previous batch before it's read, and pushes out the router's own entry. A reply to our own request may still evict the oldest entry, which can be the router's when more than ~30 hosts answer. The next packet to the router then waits for one ARP round trip (a few ms; lwIP queues it).
- **mDNS socket** binds 5353, joins 224.0.0.251, multicast TTL 255, and asks with the QU bit (unicast answers to 5353). If 5353 can't be bound it falls back to an ephemeral port; responders then answer "legacy unicast" to that port.
- **Matching.** Records are matched to an instance by exact name, not substring as in Muse (where an A record could land on the wrong instance). A service goes on the host whose A record (on our subnet) matches its SRV target; otherwise on the host that answered. A sleep proxy answering for a sleeping printer therefore puts the printer's services on the printer's address. Goodbye records (TTL 0) are ignored.
- **Hostile input.** Name decompression follows at most 16 pointers and only backwards; every length is bounds-checked; a datagram larger than 2 KB is cut by `recvfrom` and parsed as far as it goes.
- **Memory.** Everything lives in PSRAM: host table (~58 KB), a 96 KB string arena (running out sets `truncated`), the receive buffer and the result. With `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, plain `malloc`/cJSON would have put hundreds of small blocks in internal RAM, so the result is written with a PSRAM string builder and the core uses `heap_caps_malloc(…SPIRAM)` throughout. Peak stack in the LAN worker (8 KB, PSRAM) is under 3 KB by inspection. 3 extra UDP sockets during a run (limit 10; voice and Connector use 2).
- **`CONFIG_LWIP_UDP_RECVMBOX_SIZE` 6 → 16.** A router answering `ssdp:all` sends a dozen datagrams at once, and the worker runs at priority 2 below the call tasks, so 6 would drop some.
- **Worker.** `lan_discover` shares the A3 LAN worker and queue with `http_request` (one at a time, 4 waiting, then "busy"). A discovery therefore delays a queued `http_request` by up to `timeout_ms`.
- **Log line** per run, e.g. `lan_discover: req_…: 12 hosts (arp 10 of 253 swept, 0 failed; mdns 9 answers; ssdp 14 answers), truncated, 3004 ms, 9120 B`.

## Checked against real answers

The Pro's network right now (a /20, router 192.168.64.254) has nothing that answers SSDP, and only the Pro itself answers mDNS. `tools/lan_discover_test/capture.py` sent the firmware's questions from the Pro and saved the answers. `replay.sh` ran them through the firmware's parser with a pretend board address of 192.168.73.17:

```
192.168.73.72 mafuzhens-MacBook-Pro.local ['mdns']
  _airplay._tcp        port 7000   16 txt keys (of 17: truncated)
  _raop._tcp           port 7000   12 txt keys
  _companion-link._tcp port 53856   8 txt keys
truncated: true
```

The AirPlay name comes out as `" mafuzhens MacBook Pro"`: the label really starts with a space (checked in the raw bytes), and the `’` (U+2019) is dropped by the sanitiser. The meta query's answer listed `_airdrop`, `_remotepairing` and others, which the board would then ask about.

A first attempt ran the firmware's `lan_discover.c` itself on the Mac against BSD sockets. macOS gave that freshly built binary no LAN traffic and not even the ARP table (Local Network privacy, presumably), while Python had both, so that harness was dropped for capture + replay.

## On the board (2026-10-07)

Office Wi-Fi (`ToEverything-IOT`), board 192.168.5.17, staging.

- Self-test build (`COMMA_LAN_DISCOVER_SELFTEST_S=20`): every run refused (`selftest_N refused: read-only`) until the user turned on Allow operations at 11:43:06 (`device_access` → `access: operations allowed` → metadata `scope=""`). The next run: `15 hosts (arp 13 of 253 swept, 0 failed; mdns 41 answers; ssdp 22 answers), 3007 ms, 7935 B`.
- Hosts found: router 192.168.5.1 (ARP + 11 SSDP STs: rootdevice, InternetGatewayDevice, WANDevice, WANIPConnection, …), the Pro 192.168.5.13 (`mafuzhens-MacBook-Pro.local`, AirPlay/RAOP/companion-link), other Macs/iPhones/a Chromebook with hostnames, the Mi bedside lamp 192.168.5.19 (`MiBedsideLamp2-28A1.local`, `_hap._tcp:80`), and a few ARP-only hosts. `truncated: true` (AirPlay TXT > 16 keys).
- Internal heap unchanged by a run: idle 152.2 KB, lowest 150.0 KB after an 8 s discovery.
- Release build flashed afterwards. From the phone (11:47): `request req_ewa_tDHcHKDn5M82 lan_discover` → `16 hosts (arp 13; mdns 59 answers; ssdp 22 answers), 8007 ms, 6161 B` (the agent asked for 8 s). Then the agent tried the lamp over HTTP through the board: `GET 192.168.5.19:80 -> 404, 82 ms`. The lamp speaks miIO (UDP), so that path needs A4 (`udp_request`) and a cloud miIO codec.

## Not done / next

1. **Voice call during discovery.** Not run yet. With the self-test build:
   ```
   cd t-circle-s3 && . ~/.espressif/tools/activate_idf_v6.0.2.sh
   idf.py -B build-selftest -DCOMMA_LAN_DISCOVER_SELFTEST_S=20 build   # already built
   lsof /dev/cu.usbmodem1101                                            # nobody else on the port
   idf.py -B build-selftest -p /dev/cu.usbmodem1101 flash monitor
   ```
   The self-test build runs `lan_discover` (defaults: all sources, 3000 ms) every 20 s through the real worker and gate, and logs the reply in 400-byte chunks (`connector: selftest_N [i/n] …`) plus the summary line. Check:
   - the Mac (192.168.5.13, with services and hostname) and the router (with `ssdp` entries) are listed;
   - every cap holds;
   - a voice call during the runs: `cue:` stats show no `mic_dropped` / `send_max_ms` spikes that line up with discovery; lowest internal heap stays ≥ 40 KB.

   Then flash the normal build (`idf.py -p … flash`). The self-test is off unless that CMake variable is set, and `build-selftest/` is gitignored.
2. **Through the backend with access off.** The refusal was seen on the self-test path only, not from a chat.
3. **Not tried at all:** a Yeelight bulb, a WLED/Tasmota device (the user is buying one), a subnet larger than /24, more than ~30 hosts answering ARP.
4. **`capture.py` / `replay.sh`** stay as the way to turn a field report ("it didn't find my X") into a host test case.

## Re-run

```
cd t-circle-s3/tools/lan_discover_test
./run.sh                                         # host tests, no board
python3 capture.py /tmp/cap 3                    # real answers on this network (needs LAN access)
./replay.sh <unused ip> <netmask> /tmp/cap/*.bin # through the firmware's parser
```

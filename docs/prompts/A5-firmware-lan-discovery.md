# A5 · Firmware: LAN discovery

Read first: `~/Desktop/AFK/Hardware/docs/connector-plan.md`, `docs/connector-protocol.md`, and the A1–A4 reports.

## Goal

The agent can ask the T-Circle "what is on this network?" and get back a list of hosts and services. This lets it find a Hue bridge, a Mi light or a printer without the user typing IPs.

## Do

1. Port the approach of Meta's Muse Home Link: `~/Desktop/AFK/muse-gadget-sdk/esp32/main/net_discovery.c` (Apache-2.0; keep its license header on any copied code). It covers:
   - ARP sweep and harvest;
   - mDNS queries for common services (`_hap`, `_googlecast`, `_airplay`, `_raop`, `_sonos`, `_hue`, `_miio`, …);
   - SSDP M-SEARCH;
   - bounded result size;
   - ASCII sanitising of names. Other LAN hosts control these strings, so strip control characters and newlines before they reach the agent.
2. Expose it as the Connector method `lan_discover`, using **exactly this contract**, which was agreed with the backend team on 2026-10-06; they are building `device.lan_discover` against it:
   - params: `{sources?: ["arp","mdns","ssdp"], timeout_ms?: 1000..8000}`; default all sources, `timeout_ms` 3000.
   - result: `{hosts: [{ip, mac?, hostname?, sources: ["arp","mdns",...], services: [{type: "_wled._tcp", name, port, txt?}], ssdp?: [{server, location, st}]}], truncated: bool}`; `ssdp` is an **array**, because one host can answer for several STs.
   - Hard caps: 64 hosts, 16 services per host, strings ≤ 255 bytes, `txt` ≤ 16 keys. Set `truncated: true` when anything is cut.
   - Sanitise every string that comes from the LAN (printable ASCII; newlines become spaces).
   - Add `"lan_discover"` to `capabilities.lan_gateway.methods` in `metadata`. Apply the same access gate as A3 (read-only refuses).
   - Also probe Yeelight's discovery (multicast `239.255.255.250:1982`, `M-SEARCH * HTTP/1.1` with `ST: wifi_bulb`) and report answers in `ssdp` with that ST, so Yeelight bulbs show up if the user ever adds one.
3. Backend: the backend team is building `device.lan_discover`. Until it is on staging, test with a temporary path, the same way A3 did (A3 report), or locally.
4. Verify on office Wi-Fi (`ToEverything-IOT`): the list includes the Mac (`192.168.5.13`, which advertises mDNS services) and the router, and stays within the caps. No smart-home device is needed yet; the user is buying a WLED or Tasmota device, which will show up later. Run a voice call at the same time to confirm discovery doesn't starve audio (check `cue:` stats).

## Report

Write `~/Desktop/AFK/Hardware/docs/reports/A5-report.md`. Commit (don't push).

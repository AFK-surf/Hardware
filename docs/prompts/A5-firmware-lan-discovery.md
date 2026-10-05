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
2. Expose it as a `lan_discover {sources?, timeout_ms?}` method. Advertise it in `metadata`. Apply the same access gate as A3.
3. Backend: a small addition to C2's tool (or a sibling tool) so the agent can call it. Coordinate with C2.
4. Verify on office Wi-Fi: the list includes the Mi light and the Mac. Run a voice call at the same time to confirm discovery doesn't starve audio (check `cue:` stats).

## Report

Write `~/Desktop/AFK/Hardware/docs/reports/A5-report.md`. Commit (don't push).

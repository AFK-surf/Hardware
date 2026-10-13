#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture real discovery answers on this network for replay.sh.

Sends the firmware's mDNS questions (main/lan_discover.c MDNS_TYPES, plus the DNS-SD meta query), its
SSDP ssdp:all search and Yeelight's search, then saves every answer as <kind>_<n>_<source ip>.bin.
One difference from the board: questions go out from an ephemeral port, so mDNS answers come back as
"legacy unicast" (the board binds 5353 and asks for unicast answers there). The records are the same.

Usage: python3 capture.py OUT_DIR [seconds]
Then:  ./replay.sh <an unused ip on the subnet> <netmask> OUT_DIR/*.bin
"""

import os
import select
import socket
import struct
import sys
import time

MDNS_TYPES = """_services._dns-sd._udp.local _http._tcp.local _hap._tcp.local _googlecast._tcp.local
_airplay._tcp.local _raop._tcp.local _sonos._tcp.local _hue._tcp.local _miio._udp.local _wled._tcp.local
_esphomelib._tcp.local _shelly._tcp.local _matter._tcp.local _ipp._tcp.local _printer._tcp.local
_smb._tcp.local _companion-link._tcp.local _spotify-connect._tcp.local""".split()


def question(qid, name):
    labels = b"".join(bytes([len(p)]) + p.encode() for p in name.split("."))
    return struct.pack(">HHHHHH", qid, 0, 1, 0, 0, 0) + labels + b"\0" + struct.pack(">HH", 12, 1)


def main():
    out = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
    os.makedirs(out, exist_ok=True)
    mdns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ssdp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    yeelight = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for i, name in enumerate(MDNS_TYPES):
        mdns.sendto(question(i + 1, name), ("224.0.0.251", 5353))
        time.sleep(0.005)
    ssdp.sendto(b'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\n'
                b"MX: 2\r\nST: ssdp:all\r\n\r\n", ("239.255.255.250", 1900))
    yeelight.sendto(b'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1982\r\nMAN: "ssdp:discover"\r\n'
                    b"ST: wifi_bulb\r\n\r\n", ("239.255.255.250", 1982))
    kinds = {mdns: "mdns", ssdp: "ssdp", yeelight: "yl"}
    counts = {k: 0 for k in kinds.values()}
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select(list(kinds), [], [], 0.1)
        for sock in ready:
            data, (ip, _) = sock.recvfrom(9000)
            kind = kinds[sock]
            counts[kind] += 1
            with open(os.path.join(out, f"{kind}_{counts[kind]:03d}_{ip}.bin"), "wb") as f:
                f.write(data)
    print(counts)


if __name__ == "__main__":
    main()

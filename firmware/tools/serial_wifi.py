#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Switch a board's Wi-Fi from this computer over its USB serial port, keeping its Comma setup.

For firmware with CONFIG_COMMA_SERIAL_WIFI (main/serial_console.h). The board tries the network and
stores it only once it has an IP; otherwise it goes back to the stored network. The password is
read from stdin, so it never appears in a command line, is never echoed, and is not logged.

  security find-generic-password -D "AirPort network password" -a NETWORK -w | \\
      python firmware/tools/serial_wifi.py --port /dev/cu.usbmodem101 join NETWORK
  python firmware/tools/serial_wifi.py --port /dev/cu.usbmodem101 join OPEN_NETWORK --open
  python firmware/tools/serial_wifi.py --port /dev/cu.usbmodem101 status
  python firmware/tools/serial_wifi.py --port /dev/cu.usbmodem101 volume      # show the speaker level
  python firmware/tools/serial_wifi.py --port /dev/cu.usbmodem101 volume 9    # set it (1-10), with a beep

Opening the port does not reset the board. Needs pyserial. Close any other serial monitor first.
"""

import argparse
import sys
import time

import serial

SHOWN = (" console: ", " audio: speaker volume", " wifi: ", " wifi_switch: ", " connector: connected", " connector: closed")


def open_port(name):
    port = serial.Serial()
    port.port = name
    port.baudrate = 115200
    port.timeout = 0.5
    # DTR and RTS stay as opened (both set): clearing DTR while RTS is set resets an ESP32-S3 on
    # USB Serial/JTAG.
    port.open()
    return port


def run(port, line, done, timeout, log):
    port.reset_input_buffer()
    port.write(line)
    port.flush()
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        buf += port.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            text = raw.decode("utf-8", "replace").rstrip("\r")
            if log:
                log.write(text + "\n")
            if any(tag in text for tag in SHOWN):
                print(text, flush=True)
            if done(text):
                return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--log", help="append every line the board prints to this file")
    sub = parser.add_subparsers(dest="command", required=True)
    join = sub.add_parser("join", help="switch to a network; the password comes from stdin")
    join.add_argument("ssid")
    join.add_argument("--open", action="store_true", help="a network without a password")
    sub.add_parser("status", help="network, signal, Comma state and credential fingerprint")
    volume = sub.add_parser("volume", help="show or set the speaker volume (1-10; above 8 boosts)")
    volume.add_argument("level", nargs="?", type=int, choices=range(1, 11), metavar="LEVEL")
    args = parser.parse_args()

    log = open(args.log, "a") if args.log else None
    port = open_port(args.port)
    try:
        if args.command == "status":
            ok = run(port, b"comma wifi status\n", lambda t: "wifi-status " in t, 10, log)
            return 0 if ok else 1
        if args.command == "volume":
            line = b"comma volume" + (b" %d" % args.level if args.level else b"")
            ok = run(port, line + b"\n", lambda t: "volume level=" in t or "volume refused" in t, 10, log)
            return 0 if ok else 1
        ssid = args.ssid.encode()
        password = b"" if args.open else sys.stdin.buffer.readline().rstrip(b"\r\n")
        if not args.open and not 8 <= len(password) <= 64:
            print("serial_wifi: expected an 8-64 byte password on stdin", file=sys.stderr)
            return 2
        line = b"comma wifi join " + ssid.hex().encode()
        if password:
            line += b" " + password.hex().encode()
        password = None
        finished = ("wifi-join joined", "wifi-join not joined", "wifi-join refused", "ended; its result")
        ok = run(port, line + b"\n", lambda t: any(f in t for f in finished), args.timeout, log)
        line = None
        if ok:
            # The status line that follows the result.
            run(port, b"", lambda t: "wifi-status " in t, 5, log)
        return 0 if ok else 1
    finally:
        port.close()
        if log:
            log.close()


if __name__ == "__main__":
    sys.exit(main())

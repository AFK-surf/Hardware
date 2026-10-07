#!/usr/bin/env python3
"""Provision a T-Circle over BLE, the way the Comma app does. Contract: docs/ble-provisioning.md.

    pip install bleak
    tools/ble_provision.py scan
    tools/ble_provision.py info
    tools/ble_provision.py wifi-scan
    tools/ble_provision.py provision --ssid ToEverything-IOT --password ... \
        --server https://salix-staging.cue.surf --workspace ws_... --group grp1_... --name "Kitchen"
    tools/ble_provision.py bad-writes      # malformed writes must fail with the documented errors

Secrets come from the environment so they stay out of shell history:
    COMMA_VOICE_KEY       salix_vk_...
    COMMA_CONNECTOR_TOKEN salix_conn_...
    COMMA_WIFI_PASSWORD   (instead of --password)

The first encrypted access makes the OS ask for the passkey shown on the device. On macOS a system
dialog comes up; type the 6 digits there.
"""

import argparse
import asyncio
import json
import os
import sys
import time

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError


def uuid(n):
    return f"f2d2{n:04x}-eb8e-4a64-9ec7-ab0cbb8107c8"


SERVICE = uuid(1)
WIFI_SCAN = uuid(2)
WIFI_CREDENTIALS = uuid(3)
CREDENTIALS = uuid(4)
STATUS = uuid(5)
INFO = uuid(6)
SETUP_COMPANY_ID = 0xFFFF  # Testing only; replace before production.

FINAL_STATES = {"saved", "wifi_failed"}


async def find(name=None, timeout=15.0):
    print(f"scanning for {name or 'Comma-*'} ...")
    found = await BleakScanner.find_device_by_filter(
        lambda d, adv: SERVICE in [u.lower() for u in adv.service_uuids]
        and (name is None or (adv.local_name or d.name) == name),
        timeout=timeout,
    )
    if not found:
        sys.exit("no device advertising the Comma provisioning service")
    print(f"found {found.name} ({found.address})")
    return found


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def setup_summary(data):
    # Bleak removes the two company-ID bytes and uses them as the manufacturer_data key.
    unknown = "state=unknown short_id=unknown"
    if data is None:
        return f"{unknown} (no setup data)"
    if len(data) < 5:
        return f"{unknown} (truncated setup data)"
    if data[0] != 0x01:
        return f"{unknown} (unsupported version 0x{data[0]:02X})"
    if data[1] != 0x01:
        return f"{unknown} (unsupported model 0x{data[1]:02X})"
    states = {0x00: "unprovisioned", 0x01: "provisioned-pairing"}
    state = states.get(data[2], "unknown")
    return f"state={state}(0x{data[2]:02X}) short_id={data[3:5].hex().upper()}"


async def cmd_scan(args):
    seen = {}

    def on_adv(d, adv):
        # Name and setup data arrive in the scan response, sometimes after the UUID report.
        if SERVICE not in [u.lower() for u in adv.service_uuids] and d.address not in seen:
            return
        previous_name, _, data = seen.get(d.address, (None, None, None))
        if SETUP_COMPANY_ID in adv.manufacturer_data:
            data = adv.manufacturer_data[SETUP_COMPANY_ID]
        seen[d.address] = (adv.local_name or previous_name or d.name, adv.rssi, data)

    async with BleakScanner(on_adv, service_uuids=[SERVICE], scanning_mode="active"):
        await asyncio.sleep(args.timeout)
    for address, (name, rssi, data) in seen.items():
        print(f"{name or '(name pending)'}  {address}  {setup_summary(data)}  rssi {rssi}")
    if not seen:
        print("none")


async def read_json(client, char):
    raw = await client.read_gatt_char(char)
    return json.loads(raw.decode())


async def cmd_info(args):
    dev = await find(args.name)
    async with BleakClient(dev, timeout=30) as client:
        await pair(client)
        log(f"info   {await read_json(client, INFO)}")
        log(f"status {await read_json(client, STATUS)}")


async def pair(client):
    # bleak pairs on demand on macOS (the first encrypted read triggers it). Elsewhere ask now.
    if sys.platform != "darwin":
        try:
            await client.pair()
        except (BleakError, NotImplementedError) as e:
            log(f"pair(): {e}")
    log("reading info (accept the pairing prompt and type the code on the device screen) ...")
    for attempt in range(60):
        try:
            return await client.read_gatt_char(INFO)
        except (BleakError, asyncio.TimeoutError, TimeoutError) as e:
            if not client.is_connected:
                sys.exit("disconnected while pairing (wrong code or cancelled?)")
            if attempt % 5 == 0:
                log(f"waiting for pairing: {e}")
            await asyncio.sleep(2)
    sys.exit("pairing did not complete")


async def wifi_scan(client):
    pages = {}
    done = asyncio.Event()

    def on_scan(_, data):
        page = json.loads(bytes(data).decode())
        if page.get("error"):
            log(f"scan error: {page['error']}")
        pages[page["page"]] = page
        if len(pages) == page["pages"]:
            done.set()

    await client.start_notify(WIFI_SCAN, on_scan)
    await client.write_gatt_char(WIFI_SCAN, b"{}", response=True)
    await asyncio.wait_for(done.wait(), 30)
    await client.stop_notify(WIFI_SCAN)
    aps = [ap for i in sorted(pages) for ap in pages[i]["aps"]]
    log(f"{len(aps)} networks in {len(pages)} notification(s)")
    for ap in aps:
        print(f"  {ap['rssi']:4d} dBm  {'lock' if ap['secure'] else 'open'}  {ap['ssid']}")
    return aps


async def cmd_wifi_scan(args):
    dev = await find(args.name)
    async with BleakClient(dev, timeout=30) as client:
        await pair(client)
        await wifi_scan(client)


async def watch_status(client, until, timeout):
    q = asyncio.Queue()
    await client.start_notify(STATUS, lambda _, data: q.put_nowait(json.loads(bytes(data).decode())))
    deadline = time.monotonic() + timeout
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            return None
        try:
            status = await asyncio.wait_for(q.get(), left)
        except asyncio.TimeoutError:
            return None
        log(f"status {status}")
        if status["state"] in until:
            return status


async def cmd_provision(args):
    password = args.password if args.password is not None else os.environ.get("COMMA_WIFI_PASSWORD", "")
    voice_key = os.environ.get("COMMA_VOICE_KEY")
    connector_token = os.environ.get("COMMA_CONNECTOR_TOKEN")
    if not voice_key or not connector_token:
        sys.exit("set COMMA_VOICE_KEY and COMMA_CONNECTOR_TOKEN")
    creds = {
        "server": args.server,
        "connect_url": args.connect_url,
        "workspace_id": args.workspace,
        "group_id": args.group,
        "connector_token": connector_token,
        "voice_key": voice_key,
        "device_name": args.name_to_set,
    }

    dev = await find(args.name)
    t0 = time.monotonic()
    async with BleakClient(dev, timeout=30) as client:
        await pair(client)
        log(f"info {await read_json(client, INFO)}")
        if args.scan_first:
            await wifi_scan(client)

        log(f"writing wifi_credentials for {args.ssid}")
        statuses = asyncio.create_task(watch_status(client, {"wifi_up", "wifi_failed"}, 45))
        await asyncio.sleep(0.3)  # let the subscription land first
        await client.write_gatt_char(
            WIFI_CREDENTIALS, json.dumps({"ssid": args.ssid, "password": password}).encode(), response=True
        )
        status = await statuses
        await client.stop_notify(STATUS)
        if not status or status["state"] != "wifi_up":
            sys.exit(f"Wi-Fi did not come up: {status}")

        log("writing credentials")
        statuses = asyncio.create_task(watch_status(client, {"connector_up", "connector_failed"}, 30))
        await asyncio.sleep(0.3)
        await client.write_gatt_char(CREDENTIALS, json.dumps(creds).encode(), response=True)
        status = await statuses
        if not status:
            sys.exit("no connector status within 30 s of saving")
        log(f"provisioned in {time.monotonic() - t0:.1f} s: {status}")


async def cmd_wrong_password(args):
    dev = await find(args.name)
    async with BleakClient(dev, timeout=30) as client:
        await pair(client)
        statuses = asyncio.create_task(watch_status(client, {"wifi_up", "wifi_failed"}, 45))
        await asyncio.sleep(0.3)
        await client.write_gatt_char(
            WIFI_CREDENTIALS, json.dumps({"ssid": args.ssid, "password": "definitely-wrong-1"}).encode(), response=True
        )
        log(f"result: {await statuses}")


async def cmd_bad_writes(args):
    dev = await find(args.name)
    async with BleakClient(dev, timeout=30) as client:
        await pair(client)
        cases = [
            (WIFI_CREDENTIALS, b"not json", "0x80 bad JSON"),
            (WIFI_CREDENTIALS, b'{"password":"12345678"}', "0x81 missing ssid"),
            (WIFI_CREDENTIALS, b'{"ssid":"x","password":"short"}', "0x82 bad value"),
            (CREDENTIALS, b'{"server":"https://x"}', "0x81 missing fields"),
            (
                CREDENTIALS,
                json.dumps(
                    {
                        "server": "https://x",
                        "workspace_id": "w",
                        "group_id": "g",
                        "connect_url": "wss://x/v1/connect",
                        "connector_token": "salix_conn_x",
                        "voice_key": "not_a_voice_key",
                        "device_name": "n",
                    }
                ).encode(),
                "0x82 voice_key prefix",
            ),
        ]
        ok = True
        for char, value, expect in cases:
            try:
                await client.write_gatt_char(char, value, response=True)
                log(f"FAIL accepted: {expect}")
                ok = False
            except BleakError as e:
                log(f"ok rejected ({expect}): {e}")
        sys.exit(0 if ok else 1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", help="advertised name, e.g. Comma-5D28 (default: first found)")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("--timeout", type=float, default=8)
    sub.add_parser("info")
    sub.add_parser("wifi-scan")
    sub.add_parser("bad-writes")
    w = sub.add_parser("wrong-password")
    w.add_argument("--ssid", required=True)
    pr = sub.add_parser("provision")
    pr.add_argument("--ssid", required=True)
    pr.add_argument("--password")
    pr.add_argument("--server", default="https://salix-staging.cue.surf")
    pr.add_argument("--connect-url", default="wss://salix-staging.comma.surf/v1/connect")
    pr.add_argument("--workspace", required=True)
    pr.add_argument("--group", required=True)
    pr.add_argument("--device-name", dest="name_to_set", default="T-Circle")
    pr.add_argument("--scan-first", action="store_true")
    args = p.parse_args()
    handlers = {
        "scan": cmd_scan,
        "info": cmd_info,
        "wifi-scan": cmd_wifi_scan,
        "provision": cmd_provision,
        "wrong-password": cmd_wrong_password,
        "bad-writes": cmd_bad_writes,
    }
    asyncio.run(handlers[args.cmd](args))


if __name__ == "__main__":
    main()

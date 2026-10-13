# SPDX-License-Identifier: Apache-2.0
"""Offline checks for the BLE scan contract: python -m unittest discover -s tools -v."""

import contextlib
import io
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import ble_provision as ble


def report(address="board", *, name=None, services=(), data=None, device_name=None):
    return (
        SimpleNamespace(address=address, name=device_name),
        SimpleNamespace(local_name=name, service_uuids=services, manufacturer_data=data or {}, rssi=-48),
    )


class SetupSummaryTests(unittest.TestCase):
    def test_known_states_and_mac_byte_order(self):
        for state, label in ((0, "unprovisioned"), (1, "provisioned-pairing")):
            with self.subTest(state=state):
                self.assertEqual(
                    ble.setup_summary(bytes([1, 1, state, 0x1A, 0x2B])),
                    f"state={label}(0x{state:02X}) short_id=1A2B",
                )

    def test_short_id_keeps_leading_zeroes(self):
        self.assertIn("short_id=000A", ble.setup_summary(bytes.fromhex("01 01 00 00 0A")))

    def test_missing_or_short_payload_is_unknown(self):
        for data in (None, b"", b"\x01", b"\x01\x01\x00", b"\x01\x01\x00\x5d"):
            with self.subTest(data=data):
                self.assertIn("state=unknown short_id=unknown", ble.setup_summary(data))

    def test_unsupported_version_or_model_is_not_decoded(self):
        for data in ("02 01 00 1A 2B", "01 02 00 1A 2B"):
            with self.subTest(data=data):
                self.assertIn("state=unknown short_id=unknown", ble.setup_summary(bytes.fromhex(data)))

    def test_unknown_state_is_not_unprovisioned(self):
        self.assertEqual(
            ble.setup_summary(bytes.fromhex("01 01 02 1A 2B")),
            "state=unknown(0x02) short_id=1A2B",
        )


class ScanTests(unittest.IsolatedAsyncioTestCase):
    async def scan(self, reports):
        class Scanner:
            def __init__(self, callback, **kwargs):
                self.callback = callback

            async def __aenter__(self):
                for device, advertisement in reports:
                    self.callback(device, advertisement)
                return self

            async def __aexit__(self, *args):
                return False

        output = io.StringIO()
        with patch.object(ble, "BleakScanner", side_effect=Scanner) as scanner, \
                patch.object(ble, "BleakClient") as client, contextlib.redirect_stdout(output):
            await ble.cmd_scan(SimpleNamespace(timeout=0))
        scanner.assert_called_once()
        self.assertEqual(scanner.call_args.kwargs, {"service_uuids": [ble.SERVICE], "scanning_mode": "active"})
        client.assert_not_called()
        return output.getvalue()

    async def test_delayed_scan_response_is_merged_and_preserved(self):
        output = await self.scan([
            report(services=[ble.SERVICE.upper()]),
            report(name="Comma-1A2B", data={0xFFFF: bytes.fromhex("01 01 00 1A 2B")}),
            report(services=[ble.SERVICE], device_name="stale OS name"),
        ])
        self.assertEqual(output, "Comma-1A2B  board  state=unprovisioned(0x00) short_id=1A2B  rssi -48\n")

    async def test_service_filter_and_company_id(self):
        output = await self.scan([
            report("unrelated", name="Comma-other", data={0xFFFF: bytes.fromhex("01 01 00 1A 2B")}),
            report(name="Comma-1A2B", services=[ble.SERVICE], data={0x1234: bytes.fromhex("01 01 00 1A 2B")}),
        ])
        self.assertNotIn("unrelated", output)
        self.assertIn("state=unknown short_id=unknown (no setup data)", output)

    async def test_new_unsupported_payload_replaces_old_decoded_state(self):
        output = await self.scan([
            report(services=[ble.SERVICE], data={0xFFFF: bytes.fromhex("01 01 00 1A 2B")}),
            report(name="Comma-1A2B", data={0xFFFF: bytes.fromhex("02 01 00 1A 2B")}),
        ])
        self.assertIn("unsupported version 0x02", output)
        self.assertNotIn("unprovisioned", output)

    async def test_no_devices(self):
        self.assertEqual(await self.scan([]), "none\n")


if __name__ == "__main__":
    unittest.main()

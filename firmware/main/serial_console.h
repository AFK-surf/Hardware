// SPDX-License-Identifier: Apache-2.0
#pragma once

// Wi-Fi from a computer over the USB serial port (CONFIG_COMMA_SERIAL_WIFI), for when typing on the
// round screen is impractical. Lines on the USB Serial/JTAG port, never the network:
//
//   comma wifi join <ssid hex> [<password hex>]   switch as the on-screen settings do (wifi_switch.h)
//   comma wifi status                             network, signal, Comma state, credential fingerprint
//
// Hex keeps any SSID or password on one line. Commands are not echoed, and the password is never
// logged; it is wiped once handed to wifi_switch. tools/serial_wifi.py sends them. Anyone with the
// USB port can already read the stored credentials (docs/security.md), so this adds no new access.

// Start reading the port. Called once from the UI task.
void serial_console_start(void);

// Run a command that arrived and report a switch it started once it ends. UI task, every pass.
void serial_console_poll(void);

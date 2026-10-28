# Security

- **Pairing.** Every board uses LE Secure Connections only (legacy pairing is refused) and requires an
  encrypted link before any setup characteristic can be read or written. Screen boards show a random
  six-digit passkey (MITM protection). The ESP32 DevKit has no display or keypad, so it pairs with
  **Just Works**: the link is encrypted, but an attacker within Bluetooth range during setup can
  intercept it. Physical access (holding BOOT) is required to reopen setup on a configured board, but
  that is not cryptographic protection. Set up a DevKit where nobody else is in radio range.
- **Bluetooth is off** on a configured board except in pairing mode, and never on during a call.
- **Stored credentials are not encrypted.** These builds do not enable Secure Boot, flash encryption
  or NVS encryption, so anyone holding the board can read its Wi-Fi password, Connector token and
  voice key over USB. Both credentials are per device and are revoked when you remove the device in
  the app; factory reset erases them.
- **The gateway is read-only until you allow it.** The board itself enforces this: while device
  operations are off, it refuses every LAN request without touching the network. Requests may only
  go to addresses on the board's own subnet and private ranges, never to the board itself, loopback,
  broadcast or multicast addresses.
- Changing the workspace, group, backend URLs, Connector token or voice key resets operation
  permission; allow operations again in the app. Changing only Wi-Fi or the device name preserves
  permission. With compiled-in development credentials, permission lasts only until reboot.
- **On-screen settings (Waveshare 1.43C).** Like the BOOT hold, they need physical access. They can
  switch the board to another Wi-Fi network or turn Wi-Fi off, and start pairing mode with its usual
  passkey; they never show, change or export the Comma credentials, and a Wi-Fi-only change keeps
  operation permission. The password typed on the screen is shown only on request, never logged, and
  wiped from memory once the network is tried or the entry is cancelled; a network is stored only
  after it has connected. The same switch is offered on the USB Serial/JTAG port only
  (`tools/serial_wifi.py`), never over the network; USB access already exposes the stored
  credentials. Its password is sent hex-encoded, is not echoed and is never logged; `comma wifi
  status` prints a 4-byte SHA-256 fingerprint of the non-Wi-Fi config to show it did not change.
  The screen mirror (`tools/screen_mirror.py`) uses the same port: it shows the screen, including
  a password while "Show" is on, and its clicks act as touches, so it gives the computer the same
  control as a finger on the glass. It runs only while the computer keeps asking for it.
- The Bluetooth advertising data uses the test company ID `0xFFFF`.
- There is no OTA update; update by USB.

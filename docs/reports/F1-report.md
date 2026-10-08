# F1 report · Connector dials with the device's own name

2026-10-08 · IDF 6.0.2 · flashed to the board on `/dev/cu.usbmodem1101` (no factory reset).

## What changed

- `connector.c`
  - The dial is `?name=<device_name, percent-encoded>&alias=<comma-xxxx>&os=esp-idf&arch=xtensa&scope=…`.
    `name` comes from the `device_name` saved at BLE setup. If it is empty (blank or older provisioning),
    it falls back to the BLE name `Comma-XXXX`. It never falls back to "T-Circle".
  - `alias` is always the lower-case BLE name (`comma-5d28`). It is derived from the MAC, not from
    `device_name`, so it stays the same if the user renames the device. It always ends in
    `-<short id>`, which is what setup-offer matching needs. On a fresh setup it equals the
    lower-cased `name`.
  - `metadata.system_info.hostname` is the same name as the dial's `name`. It is JSON-escaped with
    cJSON, so quotes, backslashes and non-ASCII are safe.
  - `url_encode()`: RFC 3986 unreserved characters pass through, and every other byte becomes
    `%XX`, so UTF-8 is encoded byte by byte. If the result does not fit, the dial fails and logs
    "connect URL or token too long". With the current sizes that cannot happen.
  - `s_uri` grew from 384 to 512 bytes, enough for a 159-byte URL plus a 63-byte name fully encoded.
    The metadata buffer grew from 768 to 1024 bytes and is now static, because only the connector
    task uses it.
- `provisioning.c/.h`: `provisioning_ble_name()` holds the `Comma-XXXX` formula in one place. BLE
  advertising and the Connector fallback both use it. BLE advertising itself is unchanged.
- `device_config.c`: the dev override (`secrets.h`) no longer sets `"T-Circle (dev)"`, so a dev
  build also dials as `Comma-XXXX`.
- Docs: `connector-protocol.md` §1–3 and `ble-provisioning.md` (`device_name` paragraph).

BLE advertising, pairing and the credential JSON are unchanged. The dial log line prints the URL,
which holds no token. The token goes only in the header and is never logged.

## Backend check (Cue main 13e18c1f1)

- `salix_web/connect.ex` `connector_scope/3`: `name` = trimmed `?name=`, else the token's name.
  `alias` = trimmed `?alias=`, else the token's alias, else `name`. **There is no format check** on
  either, so `comma-5d28` is accepted.
- `comma_web/device_setup_offers.ex` `matches?/4` compares `name`, `alias`, `display_name` and
  `system_info.hostname`, ignoring case: either an exact name match or a value ending in
  `-<SHORT_ID>`. Now `name`, `alias` and `hostname` all match, so "already connected" and offer
  resolution can see the board.
- `comma_core/devices.ex` `device_display_name/1` uses this order: `display_name` (a rename in
  Settings), then `system_info.hostname`, then `name`. A later rename in the app therefore still
  wins, and the firmware does not overwrite it on reconnect.
- The Android app mints the token with `{"name": <device name>}` and no alias
  (`SettingsApi.mintConnectorToken`), and writes the same trimmed name as `device_name` over BLE.

## Verification

- `idf.py build`: no warnings in the changed files. The app image is 0x1eacd0 bytes, with 52% of the
  partition free.
- Host test of `url_encode` gives the same result as Python `urllib.parse.quote(safe='')`:
  `Living Room` → `Living%20Room`, `客厅 灯` → `%E5%AE%A2%E5%8E%85%20%E7%81%AF`,
  `a&b=c?d#e+f%` → `a%26b%3Dc%3Fd%23e%2Bf%25`. When the output does not fit, it returns false.
- Board serial log after flashing (no reset; it kept its setup):
  ```
  config: provisioned as "Comma-5D28" on Wi-Fi "ToEverything-IOT"
  connector: dialing wss://salix-staging.comma.surf/v1/connect?name=Comma-5D28&alias=comma-5d28&os=esp-idf&arch=xtensa&scope=local_file_read
  connector: socket open
  connector: sent metadata scope=local_file_read
  connector: connected run=run_7wDOlSpWblmGxh3I device=dev_jueVjQgP8FlzV8H0NpJVRg generation=2
  ```
- Settings › Devices showing "Comma-5D28": the user checks this on the phone (see below).

## Not tested

- A name with spaces or non-ASCII on the real board. That needs a new BLE setup with a renamed
  device. It is covered only by the host test above.
- The empty-`device_name` fallback on the board. The current board has a saved name.

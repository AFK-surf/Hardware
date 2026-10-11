# Comma BLE provisioning contract

Firmware: `firmware/main/provisioning.c` (B1). App side: comma-android "Add device" (B2).
Test client: `firmware/tools/ble_provision.py` (bleak). Visible text in the app is English.

## When the device advertises

| Device state | BLE |
| --- | --- |
| Unprovisioned (fresh flash, or after factory reset) | advertises until provisioned, no timeout |
| Provisioned, idle | **off** (NimBLE fully shut down) |
| Provisioned, pairing mode (hold BOOT 3 s, let go before 10 s) | advertises; leaves after 5 min without a connection, or on a tap on the T-Circle screen |
| In a call | **off**; pairing mode cannot start during a call, and a call cannot start while BLE is on |

Hold BOOT 10 s = factory reset: T-Circle shows a countdown and DevKit logs it from 3 s on, then NVS is erased (credentials,
Wi-Fi, BLE keys) and the device restarts unprovisioned. A hold only counts if it starts after boot: if BOOT is
still down when the device restarts, it is ignored until let go, so holding on erases once. The app tells the user this on unpair.

## Advertising

- Advertising data: flags + the complete 128-bit service UUID `f2d20001-eb8e-4a64-9ec7-ab0cbb8107c8`.
- Scan response: complete local name `Comma-XXXX` plus manufacturer-specific setup data (below).
  `XXXX` is the last two bytes of the Wi-Fi
  MAC in upper-case hex (board `98:a3:16:f5:5d:28` → `Comma-5D28`). Filter scans by the service UUID,
  not by name.
- Address: a **new random static address each pairing session** (each boot into pairing). Don't
  remember the BLE address across sessions; use `info.mac` as the stable identity.
- Connectable, one connection at a time.

The manufacturer-specific AD structure (type `0xFF`) contains these **7 payload bytes**; offsets
exclude the AD length and type bytes:

| Byte | Field | Value |
| --- | --- | --- |
| 0–1 | Company ID, little endian | `0xFFFF` (`FF FF`), reserved for testing |
| 2 | Format version | `0x01` |
| 3 | Model | `0x01` = existing Comma setup family (T-Circle-S3 and DevKit) |
| 4 | State | `0x00` = unprovisioned; `0x01` = provisioned, pairing mode |
| 5–6 | Short ID | Last two Wi-Fi MAC bytes, in name order: `Comma-5D28` → `5D 28` |

`0xFFFF` is a **test-only company ID**; replace it with an assigned Bluetooth SIG company ID
before production. Ignore unknown format versions; later versions may append fields. A missing
or unsupported payload does not mean unprovisioned. The short ID is a display hint, not a unique
identity or proof of ownership; `info.mac` remains the stable identity after pairing.

The DevKit keeps the existing model byte for compatibility with the current app scanner; use
`info.model` (`t-circle-s3` or `esp32-devkit`) as the board identity after pairing. No new advertisement
format or app hint was introduced.

Both legacy packets fit in 31 bytes: advertising is **21 bytes** (flags `1 + 2`, UUID `16 + 2`);
scan response is **21 bytes** (name `10 + 2`, manufacturer data `7 + 2`), leaving 10 bytes free.
The `+ 2` is the length and type byte for each AD structure. For `Comma-5D28`, manufacturer data
is `FF FF 01 01 00 5D 28` when unprovisioned, or `FF FF 01 01 01 5D 28` in provisioned pairing
mode. The firmware logs these bytes once on each successful advertising start.

Without connecting or pairing, run `python tools/ble_provision.py scan --timeout 8` from
`firmware/` (requires `bleak`). It actively scans by service UUID and prints name, state,
short ID and RSSI; for example `Comma-5D28 ... state=unprovisioned(0x00) short_id=5D28 ...`.
Name and manufacturer data may arrive after the first advertisement; the tool merges those
reports. Bleak exposes company ID `0xFFFF` as the `manufacturer_data` key and the remaining five
bytes as its value.

## Security

- Both boards accept **LE Secure Connections only**, reject legacy pairing, and require encryption
  with a 16-byte key before any protected characteristic can be accessed.
- **T-Circle:** display-only IO, random six-digit on-screen passkey, MITM protection, and
  `READ_AUTHEN`/`WRITE_AUTHEN` as before. Enter the code in the system pairing dialog.
- **ESP32 DevKit:** NoInputNoOutput IO, bonding on, MITM off, and encryption-only GATT permissions.
  Just Works needs no passkey; the Android app already calls the system's `createBond()`, so its
  protocol needs no change. OS confirmation behavior still needs an unlocked-device acceptance test.
  **Just Works cannot prevent an active man-in-the-middle.** BOOT gates access to setup on an already
  provisioned board, but physical presence does not provide cryptographic MITM protection.
- NimBLE's `SM_SC_ONLY=1` additionally demands authenticated Level 4 GATT security; that rejects
  Just Works even if GATT's AUTHEN flags are removed. DevKit therefore uses `SM_SC_ONLY=0`, while
  `SM_SC=y` and `SM_LEGACY=n` still refuse legacy pairing. T-Circle keeps `SM_SC_ONLY=1`.
- The phone initiates pairing (`createBond()` or first protected access). Firmware does not send an
  early Security Request on connection, preserving the existing app timing and SMP timeout behavior.
- Bonds live in device RAM for the current session only, so a dropped connection can reconnect
  without a new code until the device leaves pairing mode or restarts. Because the address changes
  each session, an old Android bond never matches a new session; the app may call `removeBond` on
  the old device object after a successful setup, but it does not have to.
- Request MTU 517 right after connecting (`requestMtu(517)`). Writes are up to 512 bytes. The scan
  list is split to fit whatever MTU was negotiated.

## GATT

Service `f2d20001-eb8e-4a64-9ec7-ab0cbb8107c8`. All values are UTF-8 JSON. Characteristic UUIDs are
`f2d2XXXX-eb8e-4a64-9ec7-ab0cbb8107c8`:

| XXXX | Name | Properties | Value |
| --- | --- | --- | --- |
| `0002` | `wifi_scan` | write, notify | write anything (e.g. `{}`) to scan; results arrive as notifications |
| `0003` | `wifi_credentials` | write | `{"ssid": "...", "password": "..."}` |
| `0004` | `credentials` | write | see below |
| `0005` | `status` | read, notify | see below |
| `0006` | `info` | read | `{"fw_version", "mac", "model": "t-circle-s3" or "esp32-devkit", "name", "provisioned"}` |

Use write-with-response for all writes; the response carries the validation result.

### `wifi_scan`

Subscribe first, then write. The scan takes 2–4 s. Results come as one or more notifications:

```json
{"page": 0, "pages": 2, "aps": [{"ssid": "ToEverything-IOT", "rssi": -48, "secure": true}, ...]}
```

Collect pages `0..pages-1` and concatenate `aps`. At most 20 networks, unique by SSID (strongest
RSSI kept), strongest first; hidden networks are left out. On failure a single page carries
`"error": "busy"` (a `wifi_credentials` trial is running; it ends within ~30 s, retry then) or
`"failed"`, with an empty `aps`. Reconnects to the stored network never make a scan busy: while an
app is connected in pairing mode the device holds them (so a stored network that is gone does not
block the radio), and an attempt in flight is cancelled for the scan. They resume when the app
disconnects or pairing mode ends. A connected stored network stays connected. The radio is **2.4 GHz only**: say so in the UI ("Only 2.4 GHz networks are shown").
`secure: false` means an open network; send an empty password.

### `wifi_credentials`

`{"ssid": "...", "password": "..."}`. SSID 1–32 bytes, password empty (open network) or 8–64
characters. Writing starts a trial connection right away and `status` reports it
(`wifi_connecting` → `wifi_up` or `wifi_failed`). The app may write again after a failure. Nothing
is stored until `credentials` is also written and Wi-Fi is up.

### `credentials`

```json
{
  "server": "https://salix-staging.cue.surf",
  "connect_url": "wss://salix-staging.comma.surf/v1/connect",
  "workspace_id": "...",
  "group_id": "grp1_...",
  "connector_token": "salix_conn_...",
  "voice_key": "salix_vk_...",
  "device_name": "Kitchen"
}
```

All fields remain required on both boards, including the no-voice DevKit, to preserve the existing
GATT contract. `server` is the voice host. `connect_url` is the Connector URL: copy
`connect_url` from the connector-token response as given (it is the Salix host, which differs from
the Comma API host; see `docs/connector-protocol.md`). Limits (bytes): `server` 127, `connect_url`
159, `workspace_id`/`group_id` 95, `connector_token`/`voice_key` 159, `device_name` 63. `server` must
start with `https://` (or `http://` for a local dev backend), `connect_url` with `wss://` (or
`ws://`), `connector_token` with `salix_conn_`, `voice_key` with `salix_vk_`. The whole JSON must
fit in 512 bytes.

`device_name` is the device's name in Comma (the app sends the BLE name, e.g. `Comma-5D28`). The
Connector dials with it as `name` and reports it as `system_info.hostname`, so Settings › Devices shows
it; `alias` is always the lower-case BLE name (`comma-5d28`). See `docs/connector-protocol.md` §2. A
device whose stored `device_name` is empty uses the BLE name instead.

Order: write `wifi_credentials`, wait for `wifi_up`, mint the two credentials, write `credentials`.
Minting after `wifi_up` means a wrong Wi-Fi password never leaves orphan tokens. The device also
accepts `credentials` first and holds them in RAM until Wi-Fi is up.

When both are in and Wi-Fi is up, the device stores everything in NVS (atomically: an interrupted
save leaves it unprovisioned), stops advertising and notifies `saved`. It then keeps BLE up until the
app disconnects or 30 s pass, shuts BLE down, and becomes a normal device (calls allowed). The app
may disconnect after `saved`, or wait a few seconds for `connector_up` first.

This setup order remains the same for both boards. DevKit's simultaneous BLE/TLS heap test is still
pending; H1 has not selected an early-BLE-shutdown policy without a measurement. DevKit LED states:
off normally; 500 ms on/off while advertising; 100 ms on/off while connected; solid about 3 s after
`saved`, including when BLE closes during that interval. Serial logs describe each transition.

In pairing mode on an already provisioned device, writing only `credentials` (no
`wifi_credentials`) keeps the stored Wi-Fi network; this is how credentials are rotated. Writing
`wifi_credentials` there tries the new network; the stored one is never overwritten by a network that
did not connect. The device goes straight back to the stored network on `wifi_failed`, when the app
disconnects before `saved`, or when pairing mode ends without a save. Pairing mode stays open
(advertising) until its timeout, so the app can reconnect and try again, but a reconnecting app must
write `wifi_credentials` again: a trial does not survive a disconnect, even one that reached `wifi_up`.

### `status`

Read it any time; subscribe for changes:

```json
{"state": "wifi_up", "ssid": "ToEverything-IOT", "ip": "192.168.1.23", "provisioned": false}
```

| `state` | Meaning | Extra fields |
| --- | --- | --- |
| `idle` | paired, waiting for `wifi_credentials` | |
| `wifi_connecting` | trying the network | `ssid` |
| `wifi_failed` | gave up | `ssid`, `error`: `wrong_password`, `not_found` (also: 5 GHz-only or out of range), `timeout` (30 s), `failed` |
| `wifi_up` | connected; waiting for `credentials` (or saving failed: `error: "save_failed"`) | `ssid`, `ip` |
| `saved` | stored; BLE about to close | `ssid`, `ip` |
| `connector_connecting`, `connector_up`, `connector_failed` | the Connector starts as soon as `saved`, so these arrive while BLE is still up (until the app disconnects or 30 s). `connector_up` carries the backend `device_id`; `connector_failed` means the token was rejected (401/403) | `device_id` |

`provisioned` is true once the device has stored credentials (also during pairing mode on a
provisioned device). Unknown fields and states may be added; ignore them. The app should confirm
"online" from the backend (`GET /v1/comma/workspaces/:ws/devices/:id`), not from `status`.

### Write errors

Writes that fail validation are rejected with an ATT error and change nothing:

| Code | Meaning |
| --- | --- |
| `0x05` / `0x0F` | not paired yet (insufficient authentication / encryption): pair and retry |
| `0x0D` | value empty or longer than 512 bytes |
| `0x80` | not a JSON object |
| `0x81` | a required field is missing or empty |
| `0x82` | a field has the wrong type, is too long, or is invalid (password 1–7 chars, a token prefix, a URL scheme) |

On Android these arrive as the `status` of `onCharacteristicWrite` (`GATT_ERROR`-style ints, e.g.
`128` for `0x80`).

## Device screen

| Screen | When |
| --- | --- |
| "Hello / Open the Comma app to set up Comma-5D28" | unprovisioned, advertising |
| "Pairing mode / ..." | same, provisioned device in pairing mode |
| "Pairing code / 482 019 / Enter it on your phone" | pairing in progress |
| "Paired / Choose a Wi-Fi network in the app" | paired |
| "Connecting to / <ssid>" | `wifi_connecting` |
| "Wi-Fi failed / Wrong password. Try again." (or not found / no answer) | `wifi_failed` |
| "Wi-Fi connected / Finishing setup in the app" | `wifi_up` |
| "All set / Tap to call Comma" | `saved`, for 2 s after BLE closes, then the logo |
| "Pairing failed / Try again from the app" | wrong passkey or pairing cancelled |
| "Let go to pair / 7 / Keep holding to reset" | BOOT held 3–10 s |

## Storage and its risk

Credentials are in NVS namespace `comma`; the firmware reads them through one accessor
(`device_config_get`, `main/device_config.h`) for both voice and the Connector. `main/secrets.h`
with `COMMA_DEV_SECRETS 1` replaces NVS on dev boards only.

**NVS is not encrypted.** Anyone holding the board can dump the flash over USB and read the Wi-Fi
password, connector token and voice key. Encrypting NVS on the ESP32-S3 needs either flash
encryption or an HMAC key burned into eFuse; both permanently change the chip (eFuse writes are
irreversible, and release-mode flash encryption locks out plain USB flashing), and without secure
boot an attacker can still flash firmware that decrypts NVS for them. That is the wrong trade for
the one shared dev board. Mitigations now: both credentials are per device and revocable (unpair
revokes them, B3), and factory reset erases them. For production: secure boot v2 + flash encryption
(release mode) + NVS encryption with the HMAC scheme, provisioned at the factory.

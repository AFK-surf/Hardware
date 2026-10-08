# E2c · Android: keep finding new devices while the app is open; redesign and animate the setup card

Work in the Cue repo, `clients/apps/android`, on a new branch off main. Read first:
- `clients/apps/android/AGENTS.md`, `docs/DESIGN.md` and `docs/design-foundation.md` (tokens, `DesignSpring`, no bounce, no ripples);
- the nearby-device code from #2740: `settings/devices/NearbyScanController.kt`, `NearbyDevices.kt`, `DeviceSetupOffer.kt`, `DeviceSetupOfferCard.kt`, `conversation/SetupOfferProjection.kt`;
- how chat rows enter: `chat/render/ChatTimelineAdapter.kt` and `ChatTimelineView.kt`.

## Context

A new or reset T-Circle (Comma's ESP32 device) advertises over BLE while it is unprovisioned. When the app comes to the foreground it scans for about 8 s and reports any unprovisioned Comma device. The backend (#2731) then posts an agent message in Home with a `device_setup_offer` card. The card has Connect / Not now buttons, and Connect opens Add device with the device chosen. Card states come from `device_setup_offer.updated` and are ordered by `revision`: `open` / `started` / `dismissed` / `done` / `expired`.

Found on the phone on 2026-10-08:
1. The user opened the app, then reset the board. No offer came until the app was sent to the background and reopened. The scan only runs at the moment the app comes to the foreground. That's the common real case: the app is already open, and the user unboxes and plugs in the device.
2. The card is plain: a name, "Ready to set up", a blue "Connect" and a grey "Not now". It doesn't say what the thing is, and its states look alike.
3. The card pops in with no motion, while other agent messages animate in.

## Do

### 1. Keep scanning while the app is open

- While the app is in the foreground and the other conditions of `NearbyScanController.update` hold, run a short scan **every 30 s**, plus once when the Home chat comes back into view. Stop immediately when the app leaves the foreground or Add device opens. Don't run a foreground service.
- Report deduping:
  - keep the 30-minute local memory for the outcomes `offered` and `duplicate`;
  - for `already_connected` (the server still saw the old device online, e.g. right after a reset), allow a re-report after **60 s**, so a just-reset device is offered within about a minute.
- If the backend's report response isn't available to the controller today, plumb the per-device `outcome` through.
- Tests:
  - a device that appears while the app is already open is reported within one interval;
  - no repeat reports within 30 min after `offered`;
  - re-report after 60 s following `already_connected`;
  - no scanning in the background or while Add device is open.

### 2. Redesign the card

Use the existing tokens, type styles and radii (match the task cards in chat; see `tasks/ui/TaskCardView.kt`). Don't introduce new colours or fonts.

```
┌──────────────────────────────────────┐
│  ◉   Comma-5D28                       │   device icon (ic_settings_comma_device, in a round tile)
│      New device · Nearby              │   secondary text
│                                       │
│  [         Set up         ]  Not now  │   primary full-width-ish button + text button
└──────────────────────────────────────┘
```

| State | Icon | Subtitle | Actions |
|---|---|---|---|
| `open` | device icon | New device · Nearby | **Set up** (primary), **Not now** (text button, not a filled grey button) |
| `started` | device icon | Setting up… | primary area shows a small spinner + "Setting up…", disabled; no Not now |
| `done` | check in a positive/success tile (use the existing success token) | Connected · Online | none |
| `dismissed` | device icon, card dimmed | Dismissed | none |
| `expired` | device icon, card dimmed | Expired | none |

- Rename "Connect" to "Set up".
- Copy is English, matching the app's existing strings setup. If the app has a zh-CN strings file for this area, add Chinese too:

  | | English | Chinese |
  |---|---|---|
  | subtitle | New device · Nearby | 新设备 · 在附近 |
  | primary | Set up | 设置 |
  | secondary | Not now | 暂不 |
  | in progress | Setting up… | 正在设置… |
  | done | Connected · Online | 已连接 · 在线 |

- Keep the existing rules: Set up only on the reporting phone; other phones see "Set up from the phone near it".
- Accessibility: the card announces name + state, and buttons have content descriptions.

### 3. Motion

- The card enters **with its message**, using the same entrance as other agent messages. Don't add a separate effect.
- State changes:
  - cross-fade the changing parts (icon, subtitle, action area);
  - animate height with `DesignSpring` or the project's critically damped spring: **no overshoot / bounce**;
  - alpha about 200 ms ease-out, as the design docs specify.
- No ripple.

### 4. Verify

- Run the unit tests and lint.
- Use screenshots or the existing screenshot/demo harness, if there is one, for each card state in light and dark.
- Phone test: the test phone `23112153D000003` is shared, so ask the user before installing, and check the foreground first (PIN from the user).
  1. Open the app **first**.
  2. The user resets the board (hold BOOT 10 s).
  3. Measure the time until the offer appears without leaving the app (target ≤ ~40 s).
  4. Then Set up → Setting up… → Connected.
  5. Record a short screen recording of the entrance and the state changes for the user.

## Don't

- Don't change the server or the BLE protocol.
- Don't change the pairing, Wi-Fi or credential steps of Add device (that's a later step, E4).

## Report

Open a draft PR as usual. Don't put verification results in the PR text (AGENTS.md); tell the user in chat, with the screenshots or recording paths.

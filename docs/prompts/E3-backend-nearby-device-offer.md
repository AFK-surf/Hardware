# E3 · Backend: the agent offers to set up a nearby Comma device

For the Comma backend team (AFK-surf/Cue, `systems/`).

## Why

Comma's hardware product is the T-Circle-S3 (ESP32-S3). It's a tap-to-call voice terminal plus an always-on LAN gateway. It runs the Connector protocol, and C2–C4 built `device.lan_request`, `device.miio_request` and device secrets on top of it. Today the user sets one up from Android Settings › Devices › Add device (#2699): BLE pairing with a passkey shown on the device, then Wi-Fi, then credentials.

New feature, three parts:
1. The user plugs in a new T-Circle and opens the Comma Android app.
2. The app runs a short BLE scan (E2).
3. If an unprovisioned Comma device is nearby, **the Comma agent posts a proactive message** in the user's Home Conversation: "I found a Comma device nearby (Comma-5D28). Want to set it up?", with a **Connect** button. On the phone that found it, the button opens Add device with that device already chosen.

The product ships overseas, so visible text is English. Part E1 (firmware) puts model, state and a short id in the BLE advertising. Part E2 (Android) does the scan and the button. This part is the server side.

## Do

1. **Report endpoint.**
   - `POST /v1/comma/workspaces/:workspace_id/nearby-devices`
   - body: `{request_id, reporter_device_id, devices: [{name, model, state, short_id, rssi}]}`
     - `name` e.g. `"Comma-5D28"`; `model` e.g. `"t-circle-s3"`; `state` is `"unprovisioned"`; `short_id` is 4 hex characters.
     - `reporter_device_id` is the phone's Comma device id (the one already used for message device origins).
   - Auth: any workspace member who can add devices (the same rule as Add device / connector-token minting).
   - Answer at once. The message appears asynchronously.
2. **Dedupe on the server.** At most one offer per (workspace, short_id) per 30 minutes, even if two phones report or the app is reopened. If the workspace already has an online device whose MAC ends with that short id, don't offer: it's a reset or re-paired device the user is handling. Use your judgment on whether the device records hold the MAC; if they don't, skip this check and say so.
3. **The message.**
   - An agent message in the Home Conversation, plus a push notification like other proactive messages. Respect the owner's "automatic messages" switch only if that switch is meant to cover this kind; your call, explain it.
   - Deterministic text from a template, not a model call: it must appear within a second or two of opening the app.
   - Attach a structured action the clients can render. **Reuse an existing card or action mechanism if one fits** (Task status cards? suggested replies?) before inventing one, and follow `AGENTS.md` → Domain concepts. Proposed shape:
     ```json
     {"type": "device_setup_offer", "offer_id": "...", "name": "Comma-5D28", "model": "t-circle-s3",
      "short_id": "5D28", "reporter_device_id": "...", "status": "open"}
     ```
   - The text alone must make sense to clients that don't know the card (Mac, web, older Android): e.g. "…Open Comma on the phone near it to set it up."
4. **Resolve the offer.**
   - `POST …/nearby-devices/offers/:offer_id {action: "dismiss" | "started"}` sets `status`, and every client sees the card update.
   - When a device with that short id finishes setup (its connector comes online), mark the offer `done`.
   - Open offers expire after 30 minutes.
5. **The agent knows about it.** The Router sees the offer in context. If the user replies in text ("yes", "set it up"), it answers that setup happens on the phone (tap Connect). It has no BLE tool and must not pretend to set up the device itself.
6. **Tests:** report → one message with the card; duplicate report → no second message; dismiss and done update the card; a non-member gets 403.

## Out of scope

- Background scanning, Fast Pair.
- Any change to setup itself (BLE pairing, credentials).

## PRs

Draft PRs as usual. Send E2 (Android) the final endpoint and card shape as soon as they're fixed: E2 is built against them.

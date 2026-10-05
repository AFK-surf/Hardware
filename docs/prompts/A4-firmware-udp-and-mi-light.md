# A4 · Firmware: `udp_request` + Mi light end to end (pairs with backend C2/C3)

Read first: `~/Desktop/AFK/Hardware/docs/connector-plan.md`, `docs/connector-protocol.md`, and the A1–A3 reports. Read A1's answer about cloud compute especially carefully.

## Goal

This is the product's proof point. With the user's **Mac Connector turned off**, the user says "关灯" (turn off the light) in a call from the phone app or the T-Circle, and the office Mi Home light turns off. All LAN traffic goes through the T-Circle.

## Context

- The user controls this light today through the Mac Connector: the agent runs Python with miIO on the Mac (read state, send off, read back: `BEFORE ['on','20','3400','2'] → OFF ['ok'] → AFTER ['off',…]`).
- miIO is UDP on port 54321 with AES built from a device token: a hello handshake, then encrypted commands. The T-Circle must not learn miIO. The cloud builds and parses the packets; the device only sends and receives bytes.

## Do

1. Firmware: `udp_request {host, port, payload_b64, timeout_ms, max_responses?}` → `{responses: [{from, payload_b64}], timed_out}`. Same LAN allowlist and access gate as A3. Advertise it in `metadata`.
2. Coordinate with backend C2 (protocol method, timeouts, `device.lan_request` UDP mode) and C3 (the Mi light skill that runs the miIO crypto in the cloud environment A1 identified).
3. End to end:
   - turn the Mac Connector off and confirm it shows disconnected;
   - say "关灯" in a phone call and check that the light changes state (the agent reads it back);
   - repeat from a T-Circle call;
   - measure the time from the end of speech to the light changing.

## Report

Write `~/Desktop/AFK/Hardware/docs/reports/A4-report.md` with timings and logs. Commit (don't push).

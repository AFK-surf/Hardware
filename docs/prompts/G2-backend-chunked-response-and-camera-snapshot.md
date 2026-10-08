# G2 · Backend: chunked gateway responses + "look at the camera"

For the Comma backend team (AFK-surf/Cue, `systems/`). This comes after the ONVIF WS-Security auth request, which it reuses.

## Background

A T-Circle (ESP32 LAN gateway, Connector protocol) can reach the office TP-LINK TL-IPC45AW camera at 192.168.5.33. Verified on the LAN on 2026-10-08:
- ONVIF device and media services at `http://<ip>:2020/onvif/service`;
- WS-Security UsernameToken digest works (user `admin`);
- GetDeviceInformation and GetProfiles return 200 (profile token `profile_1`);
- RTSP `/stream1` on 554.

One snapshot is **~800 KB**. `http_request` through a gateway today returns a UTF-8 text body capped at 64 KB, so the agent can't see the picture.

The firmware is adding a **chunked binary response mode** (G1). The contract is below and in `Hardware/docs/connector-protocol.md`.

## Contract (implemented by the T-Circle firmware, G1)

**Capability.** In metadata, `capabilities.lan_gateway.response_chunks = {"max_total_bytes": 4194304, "chunk_bytes": 32768}`. Use chunk mode only on gateways that advertise it.

**Request.** `http_request` params gain `"response_mode": "chunks"`.

**Device → server:**

1. Chunk frames:

   ```json
   {"id":"req_…","type":"response_chunk","seq":0,"data_b64":"…"}
   ```

   `seq` starts at 0 and frames arrive in order.

2. Then one final frame:

   ```json
   {"id":"req_…","type":"response","result":{
     "status":200,"headers":{…},"body_encoding":"chunks","chunks":N,"body_bytes":B,
     "sha256":"<hex>","truncated":false,"final_url":"…","method":"GET"}}
   ```

3. Or an `error` frame with the same `id`. Discard the partial body.

## Do

1. **Connector side.**
   - Accept `response_chunk` frames for a pending request id.
   - Buffer them up to `max_total_bytes`, outside the BEAM process heap if that's the house style for blobs.
   - Check the order and count, and verify `sha256`.
   - Time out on missing chunks. Use the existing request timeout and add a per-chunk idle limit.
   - Clean up on disconnect.
   - Add a protocol timeout entry. Also add Mac Connector (`salix-connect`) parity, or state that it returns full bodies anyway.
2. **Tool.** `device.lan_request` HTTP mode gains `response: "binary"` (or similar).
   - On a gateway that supports chunks, use chunk mode. Otherwise fail clearly ("this gateway can't return binary bodies").
   - Store the reassembled body as a media object, reusing the existing image/attachment storage.
   - Return a reference plus content type and size, not the bytes.
   - For `image/*`, make the image visible to the model (vision input), downscaled to a sensible size (e.g. long side ≤ 1280).
   - Let the agent attach it to its reply, so the user sees the photo in chat.
3. **Camera skill** (extends the ONVIF one): "take a look" =
   - GetSnapshotUri for `profile_1`, using ONVIF auth;
   - GET the URI in binary mode. The snapshot URL may need HTTP Digest: do the 401 challenge round trip server-side with the saved password;
   - look at the image and answer;
   - offer to post the photo.
4. **Safety.**
   - Same gates as `lan_request` (gateway online, private destination, allow_operations, write-safety where relevant).
   - Snapshots are personal data. Store them under the workspace with the normal retention for chat media, and never in logs.
5. **Tests:**
   - fake gateway sends chunks → reassembled bytes equal the source, and sha256 is checked;
   - a missing or duplicate chunk → error;
   - truncated → reported;
   - a non-chunk gateway → clear error;
   - the image reaches the model input.

## End-to-end (with the user, after deploy and after G1 is on the board)

1. "看看办公室现在有没有人": the agent takes a snapshot through the T-Circle (Mac Comma quit), describes it, and posts the photo in Home.
2. Do the same during a T-Circle voice call: the call must not stutter.

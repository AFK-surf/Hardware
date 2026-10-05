# C1 · Backend: make comma.voice.v1 pacing tolerate network stalls

Repo: AFK-surf/Cue, `systems/`. Open one PR for A+B. C is a separate investigation.

## Background

Measured 2026-10-05 on an ESP32 client (LilyGO T-Circle-S3) on office Wi-Fi in China, connecting to staging through Cloudflare with pcm16_24k.

- Downlink stalls of 2–4 s, where the client receives no bytes at all, happen every few tens of seconds. When the stalled bytes arrive, the client plays them and acks every output.mark promptly: client-side pending marks = 0 and uplink sends take ~12 ms. The server still closes with 4410 slow_reader, because the oldest mark's real-time deadline passed more than 2 s earlier. Calls end after 6–30 s. The Android app's 03b report shows the same 4410 drops (6–68 s calls).
- After a stall of about 2 s in both directions, the audio held in the client's TCP send buffer arrives in one burst together with its small backlog. The server closes with 4400 too_fast ("caller audio arrived faster than 1.25x real time") about 10 s into a call.
- Dead clients are already caught separately: WebSocket ping every 15 s, pong_timeout 45 s (4408), idle_timeout 60 s.

## A. 4410 slow_reader: tolerate longer stalls

- The threshold is `@slow_reader_ms 2_000` at `apps/salix_voice/lib/salix_voice/carrier/pacing.ex:31`, used at :193 (`now_ms - deadline > @slow_reader_ms`).
- Raise it to 8_000 and make it configurable the same way `lead_ms` is (opts, pacing.ex:74), so ops can tune it per carrier.
- Keep `@lead_ms 500` unchanged, so the server still never writes more than 500 ms ahead of playback and barge-in latency stays the same.
- Check that nothing else assumes 2 s: the Twilio carrier, CallActor and the tests.

## B. 4400 too_fast: allow a catch-up burst after a stall

- The check lives in pacing.ex:27-29 (`@window_ms 5_000`, `@max_ratio 1.25`) and `ingest/3` (:43-45). voice_socket.ex:119-120 closes on `{:error, :too_fast}`.
- Today, a client that went silent for 2 s because of the network and then delivers 2 s of buffered audio is treated like a client streaming too fast.
- Change the check so that audio which only makes up for earlier missing time is allowed. For example:
  - compare bytes received against wall-clock time since session.start, plus a fixed allowance such as 3 s of audio, instead of a 5 s sliding window at 1.25x; or
  - keep the sliding window and add a burst credit.
- A client that really streams faster than real time for a sustained period must still be closed. Keep the 4400 reason/message wording for real violations.

## For A and B

- Update the docs:
  - `docs/messaging-voice.md:158` (the 4400 row) and `:162` (the 4410 row, "more than 2 s behind");
  - the moduledocs at `voice_socket.ex:13-16` and `pacing.ex:8-12`.
- Tests go in `apps/salix_voice/test/carrier_web_socket_test.exs`, `apps/salix_web/test/voice_socket_test.exs` and pacing unit tests. Cover:
  - a 3 s downlink stall followed by on-time acks → no 4410;
  - acks never arriving → 4410 after the new threshold;
  - a 2 s silent gap followed by a 2 s burst → no 4400;
  - sustained 1.5x real-time input → 4400.
- Keep `log_slow_reader` (`voice_socket.ex:401+`) and add the configured threshold to its log line.

## C. (separate, investigate only) pcmu_8k over the WebSocket gets no model output

- Setup: the same ESP32 client with audio_format "pcmu_8k". The encoder is G.711 μ-law at 8 kHz, verified against the G.711 reference values.
- What works: session.started returns format=pcmu_8k, and the transport is clean. Paced silence arrives, marks are acked, and nothing closes in 30 s.
- What fails: the agent never greets, there are no transcripts for either role, and loud caller speech gets no response. The same client with pcm16_24k works.
- The relevant config is `apps/salix_voice/lib/salix_voice/model/gpt_live.ex:100-119`: `audio: %{"format" => %{"type" => "audio/pcmu", "rate" => 8000}}`.
- Check what GPT-Live expects for μ-law (field placement or names, for example separate input and output formats), and whether the Twilio path, which uses the same format, has ever worked on staging. Report your findings before changing anything.

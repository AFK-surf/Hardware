# V2 · Firmware: noise suppression + voice activity detection on the mic

Read first: `t-circle-s3/main/cue_voice.c`, `board_audio.c` and `playback.c` (their header comments explain earlier fixes); `docs/reports/V1-report.md`, `D1-report.md` and `A5-report.md` (call stats). Work on Hardware main, IDF 6.0.2.

## Problem

Call `vc_01M4A8GXARVM9ZH66KWNRZKQCK` on 2026-10-07 at 12:04 CST, in the office:
- The agent (GPT-Live) stopped mid-story at 12:05:07.
- For the next 56 s the server streamed only silence and never transcribed or answered, though the device's uplink was healthy (no drops, no retransmits).
- The backend confirmed the Router wasn't stuck: GPT-Live simply sent no more delegations.

The most likely cause is that GPT-Live's server-side turn detection kept hearing "the caller is still talking". The T-Circle sends raw mic audio with no noise suppression:
- the PDM mic gain is ×2.5;
- office noise (voices, HVAC, keyboards) goes up continuously;
- per-second mic **peaks** were 1500–2700 during that call, against about 800–1000 in a quiet room;
- the current echo gate only mutes the mic while agent speech is queued plus 300 ms, so room noise and speaker reverb reach GPT-Live whenever the agent pauses.

GPT-Live doesn't document turn-detection settings, so the fix has to be on the device: **send speech, not noise.**

## Do

1. **Measure first.** Add per-second mic RMS (not just peak) to the call stats. Record about 20 s each of:
   - a quiet room;
   - normal office noise;
   - the user speaking at arm's length;
   - the agent speaking (speaker echo into the mic).
   
   Put the numbers in the report.
2. **Noise suppression + VAD with ESP-SR AFE** (`espressif/esp-sr`):
   - Check that it builds and runs on IDF 6.0.2 / ESP32-S3. If it doesn't, report that and fall back to the WebRTC-style NS/VAD available for ESP-IDF, or a tuned energy VAD with hangover, and say which one you chose and why.
   - The AFE works at 16 kHz mono and the call is pcm16_24k. Either capture at 16 kHz and upsample to 24 kHz for the uplink, or resample 24→16 for the AFE and send the denoised 16 kHz audio upsampled to 24 kHz. Pick the one with less CPU and latency, and explain it.
   - Uplink rule: send the **denoised** audio while VAD says speech, with ~300 ms pre-roll and ~500 ms hangover so word starts and ends aren't clipped. Otherwise send digital silence (zeros) at the same real-time pace. The server needs a continuous real-time stream; keep the V1 backlog and timeout behaviour.
   - Keep the existing echo gate (mute while agent speech plays + tail) for now. Note in the report whether AFE AEC with the speaker as the reference signal looks feasible later. That would allow barge-in; don't build it in this step.
3. **Budget:** AFE buffers go in PSRAM. Lowest internal free heap during a call must stay ≥ 40 KB (it's about 105–113 KB now). Report CPU load per core during a call, and chip temperature after a 2-minute call (D1 measured 67–68 °C).
4. **Verify with the user** (ask before flashing; serial `/dev/cu.usbmodem1101`; check nothing else holds it):
   - Office, normal noise: ask the agent for a long story. It must finish without stopping mid-way. Then stay silent for 10 s; the agent must not hang. Then speak; it must answer.
   - Speak softly at arm's length: the first and last words must reach the transcript (check the `cue: transcript role=caller` lines).
   - 3 calls of 2+ minutes with normal back-and-forth: no drops, no stuck silences.
   - Compare with a build that has V2 turned off (one compile flag), in the same conditions, at least once.

## Don't

- Don't change the wire format (`comma.voice.v1`, pcm16_24k) or the Connector/LAN code.
- Don't factory-reset or re-provision the board.

## Report

Write `docs/reports/V2-report.md` with the measurements, the choices and the before/after behaviour. Commit only your own files (`git add` explicit paths; other agents use this checkout). Don't push; the repo has no remote.

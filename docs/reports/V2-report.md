# V2 report: noise suppression + VAD on the T-Circle mic

2026-10-07 · `t-circle-s3/main/voice_clean.c/.h/.lf` (new), `cue_voice.c`, `board_audio.c/.h`, `main.c`, `main/CMakeLists.txt`, `main/idf_component.yml`, `partitions.csv`, `sdkconfig(.defaults)`, `dependencies.lock` · board on office Wi-Fi, staging · IDF 6.0.2

## Result

| Check | Result |
|---|---|
| Per-second mic RMS in the call stats | **Done.** New `cue: voice` line every second: `mic_rms` (raw mic while open), `echo_rms` (raw mic while muted for agent speech), and with V2 `ns_rms`, `vad_ms`, `sent_ms`, `preroll_debt_ms`, `afe_ms`, `cpu0`/`cpu1`, `stack_free`. |
| Measurements (4 conditions) | **3 of 4.** Office noise, caller speech at arm's length and agent echo were measured in two office calls. **Quiet room not measured**: no call was made in a quiet room. See *Measurements*. |
| ESP-SR AFE on IDF 6.0.2 / ESP32-S3 | **Builds and runs.** `espressif/esp-sr` 2.5.5 (pulls esp-dl 3.3, dl_fft). AFE type VC, one mic: `NS(nsnet2) -> VAD(vadnet1_medium)`, models in a new 1 MB `model` partition. No fallback needed. |
| Resampling | **Mic captured at 16 kHz**, denoised, upsampled 2:3 to 24 kHz for the wire. See *Choices*. |
| Uplink rule | **Done.** Denoised audio while the VAD hears speech, with 300 ms pre-roll and 500 ms hangover; zeros otherwise, paced by the mic clock. Wire format unchanged (`comma.voice.v1`, `pcm16_24k`). V1 backlog cap and stall timeout kept (cap raised by the pre-roll, see *Choices*). |
| Echo gate | **Kept** as before (mute while agent audio plays + 300 ms). AEC: see *Later: AEC and barge-in*. |
| V2 off with one flag | **Done.** `idf.py -B build-v2off -DCOMMA_VOICE_CLEAN_OFF=1 build`: raw 24 kHz mic exactly as before V2; esp-sr isn't even linked (1.33 MB image). |
| Internal heap ≥ 40 KB during a call | **Met.** Lowest internal free: **95.5 KB** (call 1) and **87.4 KB** (call 2). Idle 130 KB (was ~152 KB before V2). See *Memory*. |
| CPU per core during a call | **Measured: high.** AFE ~790 ms of CPU per second of audio in a call (median; 605–895). Core 1 median **89 %** (max 98 %), core 0 median 35 %. No mic frames were lost to CPU. See *CPU*. |
| Chip temperature after 2 min | **67 °C** in both calls (56→67 and 59→67 °C), same as D1's 67–68 °C. |
| Long story finishes; 10 s silence; answer | **Not met in one call, and not because of noise.** See *Calls*: in call 2 the agent stopped after a paragraph while the uplink was pure zeros, and answered at once when spoken to. |
| Soft speech at arm's length, first/last words | **Passed once** (「今天的天气怎么样？告诉我，谢谢」 whole). One possible lost first word: 「你讲个长故事」 came back as 「我讲一个长故事」. |
| 3 calls ≥ 2 min, no drops/stuck silences | **2 calls** (124 s ended by tap; 198 s dropped by a server reset on bad Wi-Fi). The user stopped testing there. |
| Comparison with V2 off | **Not done.** The V2-off build was flashed, but the user stopped testing before that call. The board runs the V2 build again. |

## Measurements

Two calls in the office, board on the desk at arm's length. "Second" means one `cue: voice` line. Silence seconds: mic open, no agent echo, VAD silent, nothing sent. Speech seconds: mic open, VAD on for ≥ 800 ms.

| Condition | Raw mic RMS (`mic_rms`) | After nsnet2 (`ns_rms`) | Seconds |
|---|---|---|---|
| Quiet room | not measured | not measured | 0 |
| Office noise (talk, HVAC, keyboards) | 312–460, median **382** | 1–135, median **40** | 79 |
| Caller speaking at arm's length | 365–486, median **408** | 142–345, median **191** | 13 |
| Agent speaking, echo into the mic (`echo_rms`, mic muted) | 296–15 951, median **10 125** | (muted) | 96 |

Per-second **peaks** while open: median 1292, max 9187 (call 1), in line with the 1500–2700 of `vc_01M4A8GXARVM9ZH66KWNRZKQCK`.

What this shows:

- **In raw energy, speech at arm's length barely stands out from office noise** (median 408 vs 382). Any energy VAD or energy gate would either open on the office or miss the caller. That rules out the energy-VAD fallback.
- **nsnet2 separates them by ~5×** (40 vs 191 median), and vadnet fires on the caller. Of 118 seconds with the mic fully open, the VAD fired in 35, and all of those had a caller transcript nearby, except a few 20–80 ms triggers right as the echo gate opened after agent speech (speaker reverb tail; each sends at most the 500 ms hangover). Before V2, all 118 s of office noise went to GPT-Live.
- **Agent echo is ~25× louder than the caller** (median 10 125 vs 408). Barge-in needs real AEC, not a level trick.

## Choices

- **ESP-SR AFE, VC type, one mic: nsnet2 + vadnet1_medium.** WebRTC NS and VAD (no models) were the alternative. The numbers above show why a neural VAD is needed here. AGC is off: it would lift the noise floor in pauses. `vad_min_speech_ms` 128 (default), `vad_min_noise_ms` 96 (shortest allowed; our own hangover sits on top).
- **Capture at 16 kHz, upsample 16→24 kHz** (rather than capture at 24, decimate to 16, process, upsample). One resampler instead of two, no anti-alias decimator, and the AFE sees native samples. The upsampler is a 48-tap polyphase windowed sinc (cutoff 7.6 kHz, ~0.5 ms delay, ~16 MAC per output sample; negligible next to the AFE). The mic's DC-block and 4.83 kHz low-pass coefficients are recomputed for 16 kHz, same corners. The uplink stays paced by the mic's I2S clock (320 samples per 20 ms frame). Added latency: one AFE chunk (512 samples = 32 ms) plus the VAD's own decision delay, which the pre-roll covers.
- **Pre-roll and hangover** (`cue_voice.c`, `clean_gate_frame`). A 300 ms ring of denoised frames that went out as zeros. When the VAD fires, the ring goes out at once, then live audio until 500 ms after the VAD stops. Sending the ring puts the stream up to 300 ms ahead of real time, so the next zero frames are skipped until that's paid back (`preroll_debt_ms`). The debt never exceeds 300 ms. The server's 4400 check (1.25× over 5 s) sees at most 1.06×.
- **Backlog cap +300 ms with V2.** A pre-roll lands in the mic queue all at once, so the V1 cap (300 ms) would have trimmed the start of every utterance whenever 100 ms was already queued. With V2 the cap is 600 ms. After a stall at most ~1 s is ahead, still inside the server's 1.25 s. V2 off keeps 300 ms.
- **The AFE keeps hearing the mic while it's muted** (agent speaking), so the noise estimate never goes stale. Nothing heard while muted can become pre-roll.
- **No AFE → raw mic.** If the models are missing or the AFE can't be created, `voice_clean` passes the raw 16 kHz mic through (upsampled, always "speech"), so calls still work. Logged as `noise suppression unavailable`.

## Memory

| | Internal free |
|---|---|
| Before V2 (A5/D1 reports) | idle ~152 KB, lowest in a call 105–113 KB |
| V2 as first flashed | idle **77 KB**: a call would have landed near 30 KB |
| V2 as committed | idle **130 KB**, lowest in a call **95.5 / 87.4 KB** |

Where the RAM went, and what was done:

1. **AFE heap, 76 KB → 10.7 KB.** ESP-SR and esp-dl allocate with plain `malloc`, and `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` puts every block under 16 KB in internal RAM, even with `AFE_MEMORY_ALLOC_MORE_PSRAM`. `voice_clean_init` lowers that threshold to 64 B (`heap_caps_malloc_extmem_enable`) only while the AFE is built at boot, then restores it.
2. **Static RAM +44 KB.** esp-dl 9.8 KB, dl_lib 7.7 KB, audio_processor 4.9 KB (IRAM code and `.data`, left alone), and **nsnet3's 19 KB weight buffer**, linked even though nsnet2 is in use. `voice_clean.lf` maps `libnsnet3.a`'s `.bss` to PSRAM. That needs `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, which, as IDF documents, also moves the `.bss` of lwIP, net80211, pp, wpa_supplicant and bt to PSRAM (−14.7 KB internal). Their dynamic buffers were already in PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). **Watch item:** call 2 had a bad network stretch (below). Its RSSI and downlink pattern match the V1 Wi-Fi problems, but this change can't be ruled out without an A/B on the same Wi-Fi.
3. **Uplink task stack** 6 → 8 KB, internal. The AFE runs nsnet2/vadnet on it. A 16 KB stack peaked at 2.2 KB used. A PSRAM stack was tried first and was no faster.

Flash: app 1.39 → 2.0 MB (4 MB partition), models 0.6 MB in `model` (`0x410000`, 1 MB). Adding the partition doesn't touch NVS. The board was not re-provisioned.

## CPU

`afe_ms` is the time spent in `voice_clean_feed/pop` per second. `cpuN` is 100 − the idle task's share (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`, new).

| | AFE ms per s of audio |
|---|---|
| Boot bench, nothing else running (`-DCOMMA_VOICE_CLEAN_BENCH=1`) | nsnet2 + vadnet **479**, nsnet2 only 354, vadnet only **109**, WebRTC `ns_pro` (outside the AFE) 131–135 |
| In a call | **790** median (605–895) |

- In a call the uplink task's core sits at ~89 %. Nothing is lost to it (`mic_dropped=0` throughout call 1), but the margin is thin. The ~1.6× gap to the bench is contention: Wi-Fi, TLS, playback and the display share the cores and the PSRAM bus. It isn't the stack placement (tested).
- **Recommended follow-up if headroom matters:** vadnet on the raw mic without nsnet2 (~110 ms/s bench), or vadnet + WebRTC NS (~245). The gating, which is what keeps office noise off the wire, comes from the VAD; nsnet2 only cleans the audio while the caller talks. The AFE variants inside the AFE with WebRTC NS or nsnet3 couldn't be benched (their fetch timed out with 512-sample feeds); worth a look if needed.
- Temperature didn't change: 67 °C after 2 min, as in D1.

## Calls

**Call 1, V2, office** (`vc_01M4ABF4WT3AVSDS5P5CNNDCGE`, 124 s, ended by tap). Jokes, the Mi lamp, follow-ups in Chinese. Every caller utterance was transcribed and answered, with no stuck silences. Sends ≤ 230 ms, no dropped mic frames, RSSI −48…−53.

**Call 2, V2, office** (`vc_01M4AC1KHM5VVM7QPBVDHYQSPM`, dropped at 198 s).
- 20 s of silence at the start: the VAD stayed off and 20 s of zeros went out (raw `mic_rms` ~380, peaks ~1000–1500). Then 「我讲一个长故事」 (said: 你讲个长故事?).
- The story ran from 96 s and **stopped after a paragraph at ~126 s** ("…it felt warm in his hand."). From 127 s to 147 s the uplink carried **only zeros** (`vad_ms=0 sent_ms=0`) and the downlink only silence (`rx_peak=0`). At 148 s the caller spoke and the agent answered 6 s later ("Okay, I'll check on that… And I'll keep going with the story"). So at least this stop **can't be the server hearing room noise as speech**: there was none on the wire. GPT-Live ended its turn mid-story by itself, like the 12:05 call. V2 makes the device side clean, which takes the device out of the suspect list. **For the backend:** call id above, stop at 13:06:47 +08:00.
- The soft sentence at the end came through whole.
- **The drop**: the server reset the TCP connection (`Connection reset by peer`, no close code) at 13:07:59. Wi-Fi was much worse than in call 1: RSSI −53…−71, the downlink below real time for long stretches (`rx_ms` 100–700), `srtt` 500 ms, the uplink backlog up to 700 ms with frames trimmed (`mic_dropped` up to 34/s). The same network pattern as V1. See the watch item under *Memory*.

**V2-off comparison: not made** (the user stopped testing). For a later A/B, flash `build-v2off` and compare `cue: voice mic_rms` and the transcripts in the same spot.

## Later: AEC and barge-in

Feasible with the same AFE (`aec_init`, input format `"MR"`: mic + playback reference at 16 kHz), but not in this step:

- The reference has to be the speaker signal **time-aligned with the mic**: the 24 kHz playback stream (after gain, before the MAX98357A) decimated to 16 kHz and fed alongside each mic frame. The DMA delay between I2S1 out and I2S0 in has to be stable within the AEC filter length.
- The echo is ~25× the caller's level and the speaker is driven hard (×3.75 with a soft limiter). A nonlinear echo path is the hard case for a linear AEC, so the AFE's NLP stage would be doing much of the work.
- CPU: AEC on top of nsnet2 won't fit the current core budget. AEC + vadnet without nsnet2 might.

## Not done / follow-ups

- Quiet-room measurement; V2-off comparison call; a third 2-min call; office long-story test on V2 with good Wi-Fi.
- CPU headroom (see *CPU*): consider vadnet-only.
- A/B the PSRAM `.bss` move on bad Wi-Fi.
- Dev-only bench kept: `idf.py -B build-bench -DCOMMA_VOICE_CLEAN_BENCH=1 build` times AFE variants at boot.

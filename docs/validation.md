# Firmware validation

Normal-firmware validation completed on 2026-10-13 with ESP-IDF 6.0.2 and the locked dependencies.
The four builds passed. The local Connector workload used WS; saved-device reconnection was checked
separately against the backend over WSS.

| Check | T-Circle-S3 | StickS3 | Waveshare AMOLED | ESP32 DevKit |
| --- | --- | --- | --- | --- |
| HTTP interim responses, orderly close and failed reads | Passed | Passed | Passed | Passed |
| Text at the board cap and exact 1 MiB chunk transfer, three rounds | Passed | Passed | Passed | Passed |
| UDP binary echo, three rounds | Passed | Passed | Passed | Passed |
| Queued expiration, permission revocation, disconnect cancellation and recovery | Passed | Passed | Passed | Passed |
| LAN discovery, three rounds | Passed | Passed | Passed | Passed |
| TCP echo, pull and refused-port handling, three rounds | Passed | Passed | Passed | Not supported |
| User-confirmed microphone, playback and hang-up | Passed | Passed | No voice | No voice |

Discovery can return a bounded result with `truncated=true`; a pass does not mean that every host
on the subnet was found. StickS3 playback is set to -3 dB and passed the user's listening check.

Host regressions cover configuration persistence and failed saves, credential-generation races,
request cancellation/deadlines, HTTP parsing, and offline call input. Existing LAN, JSON, audio and
Python suites also passed. See [the development instructions](../README.md#host-tests) to run them.

Still unverified on hardware: offline tap/button cancellation and credential replacement across
workspaces. An intermittent 45-second Connector silence observed earlier has not been diagnosed;
successful call checks do not establish a fix for it.

A one-off setup failure that succeeded on retry also has no confirmed cause. Later successful
pairing runs and fixes to other app behavior do not establish that this original failure is fixed.

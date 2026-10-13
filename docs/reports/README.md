# Hardware engineering records

Start with [the public validation summary](../validation.md) for current coverage and limitations.
These internal records retain the measurements and incidents behind that summary. They describe
the firmware tested at the time; historical reports are not the current specification.

| Topic | Record |
| --- | --- |
| Current fixes and four-board hardware results | [2026-10-13 validation](2026-10-13-firmware-fixes-hardware.md) |
| Original findings and follow-up | [Code review](2026-10-09-code-review.md) |
| Multi-board layout and ESP32 DevKit | [Board bring-up](H1-report.md) |
| Bounded discovery and UDP probes | [Discovery validation](H2-report.md) |
| Shared voice/UI and Waveshare AMOLED | [Display-board bring-up](H3-report.md) |
| StickS3 audio, keys and earlier incorrect-board flash | [StickS3 bring-up](H4-report.md) |
| Public export, naming and intermittent Connector investigation | [Project preparation](H5-report.md) |
| Voice uplink and microphone processing | [Uplink investigation](V1-report.md), [noise processing](V2-report.md) |
| Chunked HTTP and TCP tunnels | [Chunk transfers](G1-report.md), [tunnels](G3-report.md) |

Raw device logs, firmware backups and build outputs stay outside tracked documentation. Existing
reports and screenshots are retained rather than removed during routine repository cleanup.

## Open follow-up

| Item | Current evidence | Needed to close |
| --- | --- | --- |
| Intermittent 45-second Connector silence | Diagnostic logging added; later calls did not reproduce it. StickS3 retests did not reproduce the original voice-first order. | Reproduce with synchronized voice/Connector logs, establish a cause, and test the resulting fix. |
| Offline call cancellation and pairing | Nonblocking UI change and host simulation passed. | With Wi-Fi unavailable, confirm a second tap cancels and a BOOT hold enters pairing promptly; reconnection must not start the cancelled call. |
| Credentials moved across workspaces | Credential-bound consent and failed-save host checks passed. | Re-provision a test board into another workspace, verify operation permission starts disabled and stays disabled after reboot, and confirm explicit re-enabling works. |
| Original StickS3 setup failure | [Pairing follow-up](B3-android-pairing-report.md) completed three StickS3 runs and one T-Circle comparison, but the original error has no captured response or request ID. | Capture the failing stage, app/BLE status and backend error before retrying; distinguish it from the other app bugs already fixed. |

These items remain open. The filming image and repository cleanup do not claim to resolve them.

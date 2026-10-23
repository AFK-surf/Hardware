# Adding or changing a board

1. Read the current README, Connector protocol, BLE contract, and devices/README.md. Keep the shared gateway
   in `firmware/main/`; put drivers and feedback in `firmware/boards/<board>/`. A voice/screen board
   reuses `firmware/voice/` and `firmware/ui/` and implements only `voice/board_audio.h` (mic and
   speaker at the pipeline's rates), `voice/board_input.h` (tap, BOOT) and `ui/board_display.h`
   (panel, framebuffer, sleep), with `ui/panel_transfer.h` for the transfer and `boards/common/`
   for parts other boards already drive; shared code never includes a board's own headers.
2. Add a `COMMA_BOARD` choice in `firmware/main/Kconfig.projbuild`, its board ID and client source,
   and an overlay `devices/sdkconfig.<board>`. Select voice, display, pairing method, GPIOs and limits
   from actual hardware. Implement `board.h` and call the shared `app_poll()` for BOOT/startup logic.
3. Add the board/target mapping to `tools/board.sh` and source selection in `main/CMakeLists.txt`.
   IDF collects component requirements before Kconfig: update target-dependent requirements and the
   ESP-SR manifest rule deliberately if introducing another target or a no-voice S3 board. Do not
   silently download voice models on a no-voice board. Check the final map and managed dependencies.
4. Select a partition table, flash size/mode, chip revision and memory settings. Use
   `COMMA_BUFFER_CAPS` for movable buffers/stacks. Measure internal free heap, largest block, minimum
   and stack watermarks with `COMMA_HEAP_DIAGNOSTICS`; no-PSRAM settings need real workload evidence.
5. Preserve T-Circle's generated config, limits, metadata identity, passkey/MITM and voice behavior.
   Build both affected targets, compare config and size/components, and run Python and all C host
   tests. Keep dependency package versions locked; do not run update-dependencies.
6. Keep GATT UUIDs, JSON fields and Connector protocol unchanged. Just Works uses encryption-only
   GATT and 16-byte keys, Secure Connections on, legacy off, and NimBLE SC_ONLY=0 (SC_ONLY=1 also
   requires MITM). Document its inability to prevent active MITM.
7. Test text at the configured cap, 1 MB chunks, discovery, and tunnels three times each, recording
   recovery. The server sends 32 KiB tunnel frames and requires a window >= 32 KiB; a smaller outgoing
   max_frame is not a solution. Enable or disable features from measurements, never expectations.
8. Update this board table and the root README with measured status and unresolved checks. Never
   commit Wi-Fi credentials, keys, generated sdkconfig, build outputs or local TLS test certificates.

Reference memory settings came from the Apache-2.0 muse-gadget-sdk overlays; this project does not
link that SDK. Preserve attribution and license headers if copying implementation code in future.

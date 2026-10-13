# T-Circle filming image

`COMMA_VIDEO_LOGO_ONLY=ON` keeps the existing static Comma logo visible after the display starts,
including during calls. It disables screen sleep and suppresses all text cards. Touch-to-call,
touch-to-hang-up, audio, networking and saved setup keep their normal behavior.

The option defaults to off and is accepted only for a T-Circle-S3 with its display enabled. Build
it separately so the normal image remains available. After activating ESP-IDF 6.0.2, from the
repository root:

```sh
cd firmware
idf.py -B build-video-t-circle-s3 \
  -DIDF_TARGET=esp32s3 -DCOMMA_BOARD=t-circle-s3 \
  -DSDKCONFIG="$PWD/build-video-t-circle-s3/sdkconfig" \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;../devices/sdkconfig.t-circle-s3' \
  -DCOMMA_VIDEO_LOGO_ONLY=ON build
```

Use an already-provisioned board. Pairing codes, error cards and the factory-reset countdown are
hidden in this image; restore normal firmware before changing setup. BOOT holds still retain
their pairing and reset actions.

Read the attached chip's MAC before flashing and match it to the intended T-Circle. USB port names
can be reused when a different board is connected. From `firmware/`:

```sh
python -m esptool --port PORT read-mac
idf.py -B build-video-t-circle-s3 -p PORT flash monitor
```

The boot log identifies the image with `filming mode: static logo only, screen sleep disabled`.
Check that the logo remains visible after at least 30 seconds idle, throughout a call, and after
hang-up. Flashing these images does not require erasing NVS.

After filming, return to the repository root and restore the normal build:

```sh
tools/board.sh t-circle-s3 build
python -m esptool --port PORT read-mac
tools/board.sh t-circle-s3 flash-monitor PORT
```

Validated on a T-Circle on 2026-10-13: the existing setup reconnected automatically after flashing.
The user confirmed that the logo remained visible during idle, a call, and after touch-to-hang-up.
The call ended normally, and Connector heartbeats continued without a reconnect during observation.
The four normal board builds also passed with this option off.

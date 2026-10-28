#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# One project, per-board config and objects. Extra -D options go to idf.py.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOARD=${1:?Usage: tools/board.sh BOARD [build|flash|monitor|flash-monitor] [PORT] [-DKEY=VALUE ...]}
ACTION=${2:-build}
if [ "$#" -ge 2 ]; then shift 2; else shift; fi
case "$BOARD" in
  t-circle-s3) TARGET=esp32s3 ;;
  esp32-devkit) TARGET=esp32 ;;
  waveshare-amoled-1-32) TARGET=esp32s3 ;;
  m5stack-sticks3) TARGET=esp32s3 ;;
  waveshare-amoled-1-43c) TARGET=esp32s3 ;;
  *) echo "Unknown board: $BOARD" >&2; exit 2 ;;
esac
case "$ACTION" in
  build|size|size-components|reconfigure) ACTIONS=("$ACTION"); PORT_ARGS=() ;;
  flash|monitor|flash-monitor)
    PORT=${1:?Pass an explicit serial port}; shift
    PORT_ARGS=(-p "$PORT")
    if [ "$ACTION" = flash-monitor ]; then ACTIONS=(flash monitor); else ACTIONS=("$ACTION"); fi ;;
  *) echo "Unknown action: $ACTION" >&2; exit 2 ;;
esac
if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/tools/idf.py" ]; then
  IDF=("${IDF_PYTHON_ENV_PATH:+$IDF_PYTHON_ENV_PATH/bin/}python" "$IDF_PATH/tools/idf.py")
else
  command -v idf.py >/dev/null || { echo "Activate ESP-IDF 6.0.2 first" >&2; exit 1; }
  IDF=(idf.py)
fi
BUILD="$ROOT/firmware/build-$BOARD"
cd "$ROOT/firmware"
exec "${IDF[@]}" -B "$BUILD" -DIDF_TARGET="$TARGET" -DCOMMA_BOARD="$BOARD" -DSDKCONFIG="$BUILD/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$ROOT/devices/sdkconfig.$BOARD" \
  ${PORT_ARGS[@]+"${PORT_ARGS[@]}"} "$@" "${ACTIONS[@]}"

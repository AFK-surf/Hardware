# Sourced after run.sh enters a LAN host-test directory. Numeric limits only;
# host platform stubs stay unchanged. The overlay is the single source of values.
COMMA_TEST_DEFINES=
case "${1:-}" in
    "") ;;
    --devkit)
        COMMA_TEST_DEFINES=$(awk '/^CONFIG_COMMA_(LAN_HTTP_MAX_BYTES|LAN_HTTP_CHUNK_BYTES|LAN_HTTP_CHUNK_FRAME_BYTES|LAN_DISCOVER_ARENA_BYTES|LAN_DISCOVER_MAX_JSON|LAN_UDP_MAX_ANSWER)=[0-9]+$/ { print "-D" $0 }' ../../../devices/sdkconfig.esp32-devkit)
        ;;
    *) echo "Usage: $0 [--devkit]" >&2; exit 2 ;;
esac

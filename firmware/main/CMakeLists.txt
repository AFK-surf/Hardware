if(CONFIG_COMMA_BOARD_ESP32_DEVKIT)
    set(board_dir "../boards/esp32_devkit")
else()
    set(board_dir "../boards/t_circle_s3")
endif()
set(srcs "main.c" "wifi.c" "connector.c" "connector_json.c" "net_watchdog.c" "lan_http.c"
         "lan_discover_core.c" "lan_dest.c" "lan_udp.c"
         "device_config.c" "provisioning.c" "${board_dir}/board.c")
if(CONFIG_COMMA_LAN_DISCOVER)
    list(APPEND srcs "lan_discover.c")
endif()
if(CONFIG_COMMA_LAN_TUNNELS GREATER 0)
    list(APPEND srcs "lan_tunnel.c")
endif()
set(reqs esp_driver_gpio esp_timer heap freertos esp_wifi esp_event esp_netif
         nvs_flash bt esp_app_format esp_websocket_client esp-tls mbedtls cjson
         tcp_transport http_parser lwip)
# Requirements are collected before Kconfig is available. The supported S3
# board has voice/display; the classic ESP32 target has neither dependency.
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND reqs esp_driver_i2s esp_driver_i2c esp-sr esp_lcd esp_lcd_gc9d01
         esp_driver_spi esp_driver_tsens)
endif()
if(CONFIG_COMMA_HAS_VOICE)
    list(APPEND srcs "${board_dir}/board_audio.c" "${board_dir}/playback.c"
         "${board_dir}/cue_voice.c" "${board_dir}/voice_clean.c")
    set(ldfragments "${board_dir}/voice_clean.lf")
endif()
if(CONFIG_COMMA_HAS_DISPLAY)
    list(APPEND srcs "${board_dir}/display.c" "${board_dir}/board_touch.c"
         "${board_dir}/ui_card.c" "${board_dir}/ui_font.c")
    set(assets "${board_dir}/cue_logo.bin")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "." "${board_dir}"
                       PRIV_REQUIRES ${reqs} EMBED_FILES ${assets} LDFRAGMENTS ${ldfragments})

# Dev only: `idf.py -B build-selftest -DCOMMA_LAN_DISCOVER_SELFTEST_S=20 build` runs lan_discover every
# 20 s and logs the reply (connector.c). Use a separate build directory: the cache keeps the value.
if(COMMA_LAN_DISCOVER_SELFTEST_S)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_LAN_DISCOVER_SELFTEST_S=${COMMA_LAN_DISCOVER_SELFTEST_S})
endif()
# Dev only, with the self-test: `-DCOMMA_UDP_SELFTEST_HOST=192.168.5.19` also sends a miIO hello there
# each round (udp_request).
if(COMMA_LAN_DISCOVER_SELFTEST_S AND COMMA_UDP_SELFTEST_HOST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_UDP_SELFTEST_HOST="${COMMA_UDP_SELFTEST_HOST}")
endif()
# Dev only: `idf.py -B build-fake -DCOMMA_CONNECT_URL_OVERRIDE=ws://<mac-ip>:8765/v1/connect build` dials
# tools/fake_connector.py instead of the provisioned Connector, with a dummy token; the saved
# credentials are left alone (connector.c). Use a separate build directory: the cache keeps the value.
if(COMMA_CONNECT_URL_OVERRIDE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_CONNECT_URL_OVERRIDE="${COMMA_CONNECT_URL_OVERRIDE}")
endif()
# V2 off (one flag): `idf.py -B build-v2off -DCOMMA_VOICE_CLEAN_OFF=1 build` keeps the raw 24 kHz mic on
# the wire, as before V2, for comparison (voice_clean.h).
if(COMMA_VOICE_CLEAN_OFF)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_VOICE_CLEAN=0)
endif()
# nsnet2 before the VAD (V2 as first shipped): `idf.py -B build-ns -DCOMMA_VOICE_NS_OFF=0 build`. The default
# is VAD only (voice_clean.h).
if(DEFINED COMMA_VOICE_NS_OFF)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_VOICE_NS_OFF=${COMMA_VOICE_NS_OFF})
endif()
# Dev only: `idf.py -B build-bench -DCOMMA_VOICE_CLEAN_BENCH=1 build` times AFE variants at boot.
if(COMMA_VOICE_CLEAN_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VOICE_CLEAN_BENCH=1)
endif()
# Dev only: `-DCOMMA_TUNNEL_DEBUG=1` logs each open tunnel's lwIP state every second (lan_tunnel.c);
# `-DCOMMA_TUNNEL_SOCKET_WINDOW=0` turns off the per-socket receive-window cut, for comparison.
if(COMMA_TUNNEL_DEBUG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_TUNNEL_DEBUG=1)
endif()
if(DEFINED COMMA_TUNNEL_SOCKET_WINDOW)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LAN_TUNNEL_SOCKET_WINDOW=${COMMA_TUNNEL_SOCKET_WINDOW})
endif()

# Local TLS memory tests: authenticate a fake WSS server with a test-only CA.
if(COMMA_CONNECT_TEST_CA)
    if(NOT COMMA_CONNECT_URL_OVERRIDE)
        message(FATAL_ERROR "COMMA_CONNECT_TEST_CA requires COMMA_CONNECT_URL_OVERRIDE")
    endif()
    configure_file("${COMMA_CONNECT_TEST_CA}" "${CMAKE_CURRENT_BINARY_DIR}/connector_test_ca.pem" COPYONLY)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/connector_test_ca.pem" TEXT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_CONNECT_TEST_CA=1)
endif()

idf_component_register(SRCS "main.c" "display.c" "board_audio.c" "board_touch.c"
                            "wifi.c" "playback.c" "cue_voice.c" "connector.c" "net_watchdog.c" "lan_http.c"
                            "lan_discover.c" "lan_discover_core.c" "lan_dest.c" "lan_udp.c" "lan_tunnel.c"
                            "device_config.c" "provisioning.c" "ui_card.c" "ui_font.c" "voice_clean.c"
                       PRIV_REQUIRES esp_lcd esp_lcd_gc9d01 esp_driver_spi esp_driver_gpio
                                    esp_driver_i2s esp_driver_i2c esp_driver_tsens esp_timer heap freertos
                                    esp_wifi esp_event esp_netif nvs_flash bt esp_app_format
                                    esp_websocket_client esp-tls mbedtls cjson tcp_transport http_parser lwip esp-sr
                       EMBED_FILES "cue_logo.bin"
                       LDFRAGMENTS "voice_clean.lf")

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

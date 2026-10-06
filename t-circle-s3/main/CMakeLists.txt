idf_component_register(SRCS "main.c" "display.c" "board_audio.c" "board_touch.c"
                            "wifi.c" "playback.c" "cue_voice.c" "connector.c" "net_watchdog.c" "lan_http.c"
                            "lan_discover.c" "lan_discover_core.c"
                            "device_config.c" "provisioning.c" "ui_card.c" "ui_font.c"
                       PRIV_REQUIRES esp_lcd esp_lcd_gc9d01 esp_driver_spi esp_driver_gpio
                                    esp_driver_i2s esp_driver_i2c esp_driver_tsens esp_timer heap freertos
                                    esp_wifi esp_event esp_netif nvs_flash bt esp_app_format
                                    esp_websocket_client esp-tls cjson tcp_transport http_parser lwip
                       EMBED_FILES "cue_logo.bin")

# Dev only: `idf.py -B build-selftest -DCOMMA_LAN_DISCOVER_SELFTEST_S=20 build` runs lan_discover every
# 20 s and logs the reply (connector.c). Use a separate build directory: the cache keeps the value.
if(COMMA_LAN_DISCOVER_SELFTEST_S)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_LAN_DISCOVER_SELFTEST_S=${COMMA_LAN_DISCOVER_SELFTEST_S})
endif()

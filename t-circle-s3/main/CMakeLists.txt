idf_component_register(SRCS "main.c" "display.c" "board_audio.c" "board_touch.c"
                            "wifi.c" "playback.c" "cue_voice.c" "connector.c" "net_watchdog.c" "lan_http.c"
                            "device_config.c" "provisioning.c" "ui_card.c" "ui_font.c"
                       PRIV_REQUIRES esp_lcd esp_lcd_gc9d01 esp_driver_spi esp_driver_gpio
                                    esp_driver_i2s esp_driver_i2c esp_driver_tsens esp_timer heap freertos
                                    esp_wifi esp_event esp_netif nvs_flash bt esp_app_format
                                    esp_websocket_client esp-tls cjson tcp_transport http_parser lwip
                       EMBED_FILES "cue_logo.bin")

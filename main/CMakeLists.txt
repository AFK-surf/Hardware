idf_component_register(SRCS "main.c" "display.c" "board_audio.c" "board_touch.c"
                       PRIV_REQUIRES esp_lcd esp_lcd_gc9d01 esp_driver_spi esp_driver_gpio
                                    esp_driver_i2s esp_driver_i2c esp_timer heap freertos
                       EMBED_FILES "cue_logo_frames.bin")

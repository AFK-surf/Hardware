idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_lcd esp_lcd_gc9d01 esp_driver_spi esp_driver_gpio esp_timer
                       EMBED_FILES "cue_logo_frames.bin")

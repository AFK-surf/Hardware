# SPDX-License-Identifier: Apache-2.0
# Set by tools/board.sh (firmware/CMakeLists.txt); empty for a plain idf.py build.
idf_build_get_property(comma_board COMMA_BOARD)
if(CONFIG_COMMA_BOARD_WAVESHARE_AMOLED_1_32 AND NOT comma_board STREQUAL "waveshare-amoled-1-32")
    message(FATAL_ERROR "Build the Waveshare AMOLED with tools/board.sh waveshare-amoled-1-32: it adds the board's drivers")
endif()
if(CONFIG_COMMA_BOARD_M5STACK_STICKS3 AND NOT comma_board STREQUAL "m5stack-sticks3")
    message(FATAL_ERROR "Build the StickS3 with tools/board.sh m5stack-sticks3: it adds the board's drivers")
endif()
if(CONFIG_COMMA_BOARD_WAVESHARE_AMOLED_1_43C AND NOT comma_board STREQUAL "waveshare-amoled-1-43c")
    message(FATAL_ERROR "Build the Waveshare AMOLED 1.43C with tools/board.sh waveshare-amoled-1-43c: it adds the board's drivers")
endif()
if(CONFIG_COMMA_BOARD_ESP32_DEVKIT)
    set(board_dir "../boards/esp32_devkit")
elseif(CONFIG_COMMA_BOARD_WAVESHARE_AMOLED_1_32)
    set(board_dir "../boards/waveshare_amoled_1_32")
elseif(CONFIG_COMMA_BOARD_M5STACK_STICKS3)
    set(board_dir "../boards/m5stack_sticks3")
elseif(CONFIG_COMMA_BOARD_WAVESHARE_AMOLED_1_43C)
    set(board_dir "../boards/waveshare_amoled_1_43c")
else()
    set(board_dir "../boards/t_circle_s3")
endif()
# A board folder holds only that board's pins and drivers; all of its sources are built. Drivers for
# parts more than one board uses are in boards/common/.
file(GLOB board_srcs "${CMAKE_CURRENT_LIST_DIR}/${board_dir}/*.c")
if(CONFIG_COMMA_BOARD_T_CIRCLE_S3 OR CONFIG_COMMA_BOARD_WAVESHARE_AMOLED_1_32 OR CONFIG_COMMA_BOARD_WAVESHARE_AMOLED_1_43C)
    list(APPEND board_srcs "../boards/common/cst816_touch.c")
endif()
if(CONFIG_COMMA_BOARD_M5STACK_STICKS3 OR CONFIG_COMMA_BOARD_WAVESHARE_AMOLED_1_43C)
    list(APPEND board_srcs "../boards/common/mic_path.c")
endif()
set(srcs "main.c" "wifi.c" "connector.c" "connector_json.c" "net_watchdog.c" "lan_http.c"
         "lan_discover_core.c" "lan_dest.c" "lan_udp.c" "b64.c" "json_buf.c" "ws_frame.c"
         "device_config.c" "provisioning.c" ${board_srcs})
if(CONFIG_COMMA_LAN_DISCOVER)
    list(APPEND srcs "lan_discover.c")
endif()
if(CONFIG_COMMA_LAN_TUNNELS GREATER 0)
    list(APPEND srcs "lan_tunnel.c")
endif()
set(reqs esp_driver_gpio esp_timer heap freertos esp_wifi esp_event esp_netif
         nvs_flash bt esp_app_format esp_websocket_client esp-tls mbedtls cjson
         tcp_transport http_parser lwip)
# Requirements are collected before Kconfig is available, so they follow the target and the board
# name from tools/board.sh. The T-Circle, the StickS3 and the Waveshare AMOLED 1.43C have voice and
# display; the Waveshare AMOLED 1.32 display only (no speaker); the classic ESP32 target neither.
if(comma_board STREQUAL "waveshare-amoled-1-32")
    list(APPEND reqs esp_driver_i2c esp_lcd esp_driver_spi waveshare_deps)
elseif(comma_board STREQUAL "m5stack-sticks3")
    list(APPEND reqs esp_driver_i2s esp_driver_i2c esp-sr esp_lcd esp_driver_spi esp_driver_ledc
         esp_driver_tsens sticks3_deps)
elseif(comma_board STREQUAL "waveshare-amoled-1-43c")
    list(APPEND reqs esp_driver_i2s esp_driver_i2c esp-sr esp_lcd esp_driver_spi esp_driver_tsens
         esp_driver_usb_serial_jtag waveshare_1_43c_deps)
elseif(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND reqs esp_driver_i2s esp_driver_i2c esp-sr esp_lcd esp_lcd_gc9d01
         esp_driver_spi esp_driver_tsens)
endif()
# Shared voice (firmware/voice/) and screen (firmware/ui/) code, for boards that have them.
if(CONFIG_COMMA_HAS_VOICE)
    list(APPEND srcs "../voice/playback.c" "../voice/comma_voice.c" "../voice/voice_clean.c" "../voice/call.c"
         "../voice/resample.c")
    set(ldfragments "../voice/voice_clean.lf")
endif()
if(CONFIG_COMMA_HAS_DISPLAY)
    # ui/<size>/: a round screen's diameter, or a rectangular screen's WxH.
    if(CONFIG_COMMA_DISPLAY_WIDTH)
        set(ui_size "${CONFIG_COMMA_DISPLAY_WIDTH}x${CONFIG_COMMA_DISPLAY_HEIGHT}")
    else()
        set(ui_size "${CONFIG_COMMA_DISPLAY_SIZE}")
    endif()
    list(APPEND srcs "../ui/display.c" "../ui/panel_transfer.c" "../ui/ui_card.c" "../ui/${ui_size}/ui_font.c")
    set(assets "../ui/${ui_size}/comma_logo.bin")
    if(CONFIG_COMMA_SETTINGS_UI)
        list(APPEND srcs "wifi_switch.c" "../ui/settings.c" "../ui/ui_view.c" "../ui/${ui_size}/ui_font_menu.c")
    endif()
    if(CONFIG_COMMA_SERIAL_WIFI)
        list(APPEND srcs "serial_console.c")
    endif()
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "." "${board_dir}" "../boards/common" "../voice" "../ui"
                       PRIV_REQUIRES ${reqs} EMBED_FILES ${assets} LDFRAGMENTS ${ldfragments})

# One firmware version: firmware/version.txt is the PROJECT_VER that esp_app_desc reports over BLE,
# and the same string goes into Connector metadata, the voice client name and the User-Agent.
idf_build_get_property(project_ver PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_FIRMWARE_VERSION="${project_ver}")

# Dev only: `idf.py -B build-fake -DCOMMA_CONNECT_URL_OVERRIDE=ws://<mac-ip>:8765/v1/connect build` dials
# tools/fake_connector.py instead of the provisioned Connector, with a dummy token; the saved
# credentials are left alone (connector.c). Use a separate build directory: the cache keeps the value.
if(COMMA_CONNECT_URL_OVERRIDE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_CONNECT_URL_OVERRIDE="${COMMA_CONNECT_URL_OVERRIDE}")
endif()
# nsnet2 noise suppression before the VAD: `idf.py -B build-ns -DCOMMA_VOICE_NS_OFF=0 build`. The
# default is VAD only (voice_clean.h).
if(DEFINED COMMA_VOICE_NS_OFF)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COMMA_VOICE_NS_OFF=${COMMA_VOICE_NS_OFF})
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

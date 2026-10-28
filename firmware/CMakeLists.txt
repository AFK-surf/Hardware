# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Config lives next to the objects, never in the source tree.
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
# The no-audio target has its own lock and excludes the bundled LCD component.
# Never rewrite the T-Circle lock when changing chip targets.
if(IDF_TARGET STREQUAL "esp32")
    idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.esp32")
    set(EXCLUDE_COMPONENTS esp_lcd_gc9d01)
endif()
# Board-specific managed drivers: only the Waveshare AMOLED pulls the CO5300 panel component, with its
# own lock, so the T-Circle lock never changes. tools/board.sh passes COMMA_BOARD; main/CMakeLists.txt
# reads it as a build property, since requirements are collected before Kconfig.
if(COMMA_BOARD STREQUAL "waveshare-amoled-1-32")
    idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.waveshare-amoled-1-32")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards/waveshare_amoled_1_32/waveshare_deps")
    set(EXCLUDE_COMPONENTS esp_lcd_gc9d01)
endif()
# The StickS3 likewise: esp_codec_dev for its ES8311, its own lock. Its ST7789 driver is in IDF's esp_lcd.
if(COMMA_BOARD STREQUAL "m5stack-sticks3")
    idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.m5stack-sticks3")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards/m5stack_sticks3/sticks3_deps")
    set(EXCLUDE_COMPONENTS esp_lcd_gc9d01)
endif()
# The Waveshare AMOLED 1.43C: the CO5300 panel and esp_codec_dev (ES8311 speaker, ES7210 mics), its own lock.
if(COMMA_BOARD STREQUAL "waveshare-amoled-1-43c")
    idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.waveshare-amoled-1-43c")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards/waveshare_amoled_1_43c/waveshare_1_43c_deps")
    set(EXCLUDE_COMPONENTS esp_lcd_gc9d01)
endif()
idf_build_set_property(COMMA_BOARD "${COMMA_BOARD}")
project(comma)
include(cmake/idf_patches.cmake)

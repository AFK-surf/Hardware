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
project(cue_logo)
include(cmake/idf_patches.cmake)

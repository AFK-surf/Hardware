# SPDX-License-Identifier: Apache-2.0
# LAN scripts (include/lan_script.h) on Micro QuickJS (mquickjs/, MIT, bellard/mquickjs 6d4d7eb).
# Micro QuickJS keeps its standard library as ROM tables made by a host program from
# lan_script_stdlib.c: it is built with the host's compiler and run for this 32-bit target here.
set(q "${CMAKE_CURRENT_LIST_DIR}/mquickjs")
set(gen "${CMAKE_CURRENT_BINARY_DIR}/lan_script_gen")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    find_program(LAN_SCRIPT_HOST_CC NAMES cc clang gcc REQUIRED)
    file(MAKE_DIRECTORY "${gen}")
    execute_process(
        COMMAND "${LAN_SCRIPT_HOST_CC}" -O2 -D_GNU_SOURCE -I "${q}" -o "${gen}/stdlib_gen"
                "${CMAKE_CURRENT_LIST_DIR}/lan_script_stdlib.c" "${q}/mquickjs_build.c"
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "lan_script: building the stdlib generator with ${LAN_SCRIPT_HOST_CC} failed")
    endif()
    execute_process(COMMAND "${gen}/stdlib_gen" -a -m32 OUTPUT_FILE "${gen}/mquickjs_atom.h" RESULT_VARIABLE rc)
    execute_process(COMMAND "${gen}/stdlib_gen" -m32 OUTPUT_FILE "${gen}/lan_script_stdlib.h" RESULT_VARIABLE rc2)
    if(NOT rc EQUAL 0 OR NOT rc2 EQUAL 0)
        message(FATAL_ERROR "lan_script: generating the stdlib tables failed")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_LIST_DIR}/lan_script_stdlib.c" "${q}/mquickjs_build.c" "${q}/mquickjs_build.h")
endif()

idf_component_register(SRCS "lan_script.c" "lan_script_crypto_psa.c"
                            "mquickjs/mquickjs.c" "mquickjs/cutils.c" "mquickjs/dtoa.c" "mquickjs/libm.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "mquickjs" "${gen}"
                       PRIV_REQUIRES mbedtls)
# The engine's own style, as its Makefile builds it.
set_source_files_properties("mquickjs/mquickjs.c" "mquickjs/cutils.c" "mquickjs/dtoa.c" "mquickjs/libm.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-unused-function;-Wno-sign-compare;-Wno-maybe-uninitialized;-fno-math-errno;-fno-trapping-math")
target_compile_definitions(${COMPONENT_LIB} PRIVATE _GNU_SOURCE)

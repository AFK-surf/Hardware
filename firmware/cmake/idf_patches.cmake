# lan_discover (A5) sweeps the subnet with ARP and reads the replies out of lwIP's ARP table. With
# the default 10 entries, one batch of replies pushes out the next and the router's own entry.
# No Kconfig for it in IDF 6.0.2; PUBLIC so main's loop over the table sees the same size.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${lwip_lib} PUBLIC ARP_TABLE_SIZE=32)

# NimBLE gives up on pairing after 30 s without SMP progress (BLE_SM_TIMEOUT_MS, no Kconfig). The
# device shows a passkey that the person types on the phone, which often takes longer, so build a
# copy of ble_sm.c with 90 s instead. IDF itself is left untouched; if the line ever changes, the
# build stops here instead of silently keeping 30 s.
if(CONFIG_BT_NIMBLE_ENABLED AND CONFIG_BT_NIMBLE_SECURITY_ENABLE)
    set(sm_dir "${IDF_PATH}/components/bt/host/nimble/nimble/nimble/host/src")
    set(sm_src "${sm_dir}/ble_sm.c")
    set(sm_copy "${CMAKE_BINARY_DIR}/nimble_patch/ble_sm.c")
    file(READ "${sm_src}" sm_text)
    string(FIND "${sm_text}" "#define BLE_SM_TIMEOUT_MS             (30000)" sm_at)
    if(sm_at EQUAL -1)
        message(FATAL_ERROR "ble_sm.c no longer defines BLE_SM_TIMEOUT_MS as 30000; revisit the pairing timeout patch")
    endif()
    string(REPLACE "#define BLE_SM_TIMEOUT_MS             (30000)"
                   "#define BLE_SM_TIMEOUT_MS             (90000)  /* Comma: was 30000 */"
                   sm_text "${sm_text}")
    # Rewrite only on change, so a reconfigure does not rebuild it.
    set(sm_old "")
    if(EXISTS "${sm_copy}")
        file(READ "${sm_copy}" sm_old)
    endif()
    if(NOT sm_old STREQUAL sm_text)
        file(WRITE "${sm_copy}" "${sm_text}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sm_src}")

    idf_component_get_property(bt_lib bt COMPONENT_LIB)
    get_target_property(bt_srcs ${bt_lib} SOURCES)
    list(FIND bt_srcs "${sm_src}" sm_index)
    if(sm_index EQUAL -1)
        message(FATAL_ERROR "bt component does not build ${sm_src}; revisit the pairing timeout patch")
    endif()
    list(REMOVE_AT bt_srcs ${sm_index})
    list(APPEND bt_srcs "${sm_copy}")
    set_property(TARGET ${bt_lib} PROPERTY SOURCES "${bt_srcs}")
    # The copy includes NimBLE's private headers by relative path.
    target_include_directories(${bt_lib} PRIVATE "${sm_dir}")
endif()

file(GLOB_RECURSE MY_SRCS
    "apps/*.c"
    "apps/*.cc"
    "apps/*.cpp"
    "assets/*.c"
    "assets/*.cc"
    "assets/*.cpp"
    "hal/*.c"
    "hal/*.cc"
    "hal/*.cpp"
)

set(MY_INCS
    "."
)

idf_component_register(
    SRCS 
        "main.cpp"
        ${MY_SRCS}
    INCLUDE_DIRS 
        ${MY_INCS}
    EMBED_FILES
        "assets/sfx/boot_sfx.bin"
    EMBED_TXTFILES
        "hal/utils/config_ap/assets/badge_config_ap.html"
)

# Route only START dispatch and command ACKs through optional XZ negotiation.
idf_component_get_property(BLE_SERVICES_LIB espressif__ble_services COMPONENT_LIB)
target_compile_definitions(${BLE_SERVICES_LIB} PRIVATE
    esp_ble_ota_recv_cmd_data=ble_ota_xz_recv_cmd_data
)

idf_component_get_property(BLE_OTA_RAW_LIB espressif__ble_ota_raw COMPONENT_LIB)
target_compile_definitions(${BLE_OTA_RAW_LIB} PRIVATE
    esp_ble_ota_notify_command_raw=ble_ota_xz_notify_command_raw
)

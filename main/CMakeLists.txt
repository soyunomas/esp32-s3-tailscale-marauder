idf_component_register(
    SRCS "main.c" "wifi_manager.c" "web_server.c" "config_storage.c" "log_buffer.c" "tailscale_manager.c" "factory_reset.c" "scheduler_manager.c" "usb_hid_macro_store.c" "usb_hid_macro_parser.c" "usb_hid_device.c" "usb_hid_executor.c" "usb_hid_evaluator.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_server
        dns_server
        lwip
        app_update
        mbedtls
        wpa_supplicant
        microlink
        esp_driver_gpio
        espressif__cjson
        esp_tinyusb
        spiffs
    EMBED_FILES "embedded_files/index.html" "embedded_files/styles.css" "embedded_files/app.js" "embedded_files/usb_hid.js"
)

idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_options(${lwip_lib} PRIVATE "-I${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(${lwip_lib} PRIVATE "-DESP_IDF_LWIP_HOOK_FILENAME=\"wifi_lwip_hooks.h\""
                                                "-DLWIP_HOOK_IP4_INPUT=wifi_manager_lwip_hook_ip4_input")

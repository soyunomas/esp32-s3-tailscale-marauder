idf_component_register(
    SRCS "main.c" "wifi_manager.c" "web_server.c" "config_storage.c" "log_buffer.c" "tailscale_manager.c" "factory_reset.c" "scheduler_manager.c" "usb_hid_macro_store.c" "usb_hid_macro_parser.c" "usb_hid_device.c" "usb_hid_executor.c"
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
    EMBED_FILES "embedded_files/index.html" "embedded_files/styles.css" "embedded_files/app.js" "embedded_files/usb_hid.js"
)

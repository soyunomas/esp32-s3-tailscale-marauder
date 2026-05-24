#include "esp_log.h"
#include "config_storage.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "dns_server.h"
#include "log_buffer.h"
#include "tailscale_manager.h"
#include "factory_reset.h"
#include "scheduler_manager.h"
#include "usb_hid_executor.h"
#include "usb_hid_macro_store.h"

static const char *TAG = "main";
static repeater_config_t s_config;

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 WiFi Repeater + Tailscale ===");

    // Initialize log buffer (must be before any heavy logging)
    ESP_ERROR_CHECK(log_buffer_init());

    // Initialize NVS
    ESP_ERROR_CHECK(config_storage_init());

    esp_err_t macro_store_ret = usb_hid_macro_store_init();
    if (macro_store_ret != ESP_OK) {
        ESP_LOGE(TAG, "USB HID macro storage unavailable: %s", esp_err_to_name(macro_store_ret));
    }

    // Load configuration
    config_storage_load(&s_config);

    // Apply runtime log levels right after loading config so the rest of the
    // boot already follows user preferences (lighter logs => faster connect).
    config_storage_apply_log_levels(&s_config);

    // Arm physical factory reset via BOOT button (GPIO0). Short presses are
    // used later by Dormant Scheduled once the scheduler is initialized.
    ESP_ERROR_CHECK(factory_reset_init(&s_config));

    // Initialize Tailscale wrapper. This does not start MicroLink yet.
    ESP_ERROR_CHECK(tailscale_manager_init(&s_config));

    // Initialize and start WiFi
    ESP_ERROR_CHECK(wifi_manager_init(&s_config));
    if (s_config.sched_mode == SCHED_MODE_DORMANT_SCHEDULED) {
        ESP_LOGI(TAG, "Dormant Scheduled boot: keeping AP/STA/Tailscale silent until scheduler evaluates");
        ESP_ERROR_CHECK(wifi_manager_set_ap_enabled(false));
        ESP_ERROR_CHECK(wifi_manager_set_sta_scheduler_enabled(false));
        ESP_ERROR_CHECK(tailscale_manager_set_scheduler_enabled(false));
    }
    ESP_ERROR_CHECK(wifi_manager_start());

    // Start the isolated USB HID macro executor. This feature must never
    // prevent the existing web UI, captive portal, WiFi, or Tailscale paths
    // from booting.
    esp_err_t usb_hid_ret = usb_hid_executor_init();
    if (usb_hid_ret != ESP_OK) {
        ESP_LOGE(TAG, "USB HID executor disabled: %s", esp_err_to_name(usb_hid_ret));
    } else {
        esp_err_t keepalive_ret = usb_hid_executor_configure_keepalive(
            s_config.usb_hid_keepalive_enabled,
            s_config.usb_hid_keepalive_key,
            s_config.usb_hid_keepalive_interval_s);
        if (keepalive_ret != ESP_OK) {
            ESP_LOGE(TAG, "USB HID keep-awake config rejected: %s", esp_err_to_name(keepalive_ret));
        }
    }

    // Start time sync and scheduler after WiFi is up and after the optional
    // HID executor is ready, so scheduled macros cannot fire before their
    // executor queue exists.
    ESP_ERROR_CHECK(scheduler_manager_init(&s_config));

    // Start web server
    ESP_ERROR_CHECK(web_server_start(&s_config));

    // Start DNS server for captive portal (will be stopped when STA connects)
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns_server_handle_t dns_handle = start_dns_server(&dns_config);
    wifi_manager_set_dns_handle(dns_handle);

    ESP_LOGI(TAG, "System ready. AP='%s' Web UI at http://192.168.4.1", s_config.ap_ssid);
}

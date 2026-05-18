#include "usb_hid_device.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

static const char *TAG = "usb_hid_device";

#define USB_HID_REPORT_ID_KEYBOARD 1
#define USB_HID_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)
#define USB_HID_READY_TIMEOUT_MS 2000
#define USB_HID_RELEASE_POLL_MS 5

static bool s_installed;
static bool s_suspended;
static bool s_wakeup_host;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(USB_HID_REPORT_ID_KEYBOARD)),
};

static const char *s_hid_string_descriptor[] = {
    (char[]){0x09, 0x04},
    "ESP32",
    "WiFi Repeater HID",
    "000001",
    "Keyboard",
};

static const uint8_t s_hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_HID_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(s_hid_report_descriptor),
                       0x81, 16, 10),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    s_suspended = true;
    s_wakeup_host = remote_wakeup_en;
}

void tud_resume_cb(void)
{
    s_suspended = false;
    s_wakeup_host = false;
}

esp_err_t usb_hid_device_init(void)
{
    if (s_installed) return ESP_OK;

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    
    /* Manual task configuration to isolate USB from Network traffic */
    tusb_cfg.task.priority = 20;     /* High priority */
    tusb_cfg.task.xCoreID = 1;       /* Run on Core 1 (Network is on Core 0) */
    
    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = s_hid_configuration_descriptor;
    tusb_cfg.descriptor.string = s_hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_hid_string_descriptor) / sizeof(s_hid_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_hid_configuration_descriptor;
#endif

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG,
                        "TinyUSB driver install failed");
    s_installed = true;
    ESP_LOGI(TAG, "TinyUSB HID keyboard installed");
    return ESP_OK;
}

bool usb_hid_device_available(void)
{
    return s_installed;
}

bool usb_hid_device_ready(void)
{
    return s_installed && tud_mounted() && !s_suspended && tud_hid_ready();
}

static esp_err_t wait_hid_ready(void)
{
    if (!s_installed || !tud_mounted()) return ESP_ERR_INVALID_STATE;
    if (s_suspended) {
        if (s_wakeup_host) {
            tud_remote_wakeup();
            s_wakeup_host = false;
        }
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t waited = 0;
    while (!tud_hid_ready() && waited < USB_HID_READY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    
    if (!tud_hid_ready()) {
        ESP_LOGW(TAG, "HID timeout: mounted=%d, suspended=%d, ready=%d", 
                 tud_mounted(), s_suspended, tud_hid_ready());
    }
    return tud_hid_ready() ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t usb_hid_device_release_best_effort(uint32_t timeout_ms)
{
    if (!s_installed) return ESP_ERR_INVALID_STATE;

    uint32_t waited = 0;
    esp_err_t last = ESP_ERR_TIMEOUT;
    do {
        if (!tud_mounted()) {
            last = ESP_ERR_INVALID_STATE;
        } else if (s_suspended) {
            if (s_wakeup_host) {
                tud_remote_wakeup();
                s_wakeup_host = false;
            }
            last = ESP_ERR_INVALID_STATE;
        } else if (tud_hid_ready()) {
            if (tud_hid_keyboard_report(USB_HID_REPORT_ID_KEYBOARD, 0, NULL)) {
                return ESP_OK;
            }
            last = ESP_FAIL;
        }

        vTaskDelay(pdMS_TO_TICKS(USB_HID_RELEASE_POLL_MS));
        waited += USB_HID_RELEASE_POLL_MS;
    } while (waited <= timeout_ms);

    return last;
}

esp_err_t usb_hid_device_press(uint8_t modifier, uint8_t keycode)
{
    /* Use a shorter polling interval to check readiness more frequently without long blocking */
    uint32_t waited = 0;
    while (!tud_hid_ready() && waited < USB_HID_READY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(2)); 
        waited += 2;
    }

    if (!tud_hid_ready()) {
        ESP_LOGE(TAG, "HID press timeout (busy/suspended). Mounted=%d", tud_mounted());
        return ESP_ERR_TIMEOUT;
    }

    uint8_t keycodes[6] = {keycode, 0, 0, 0, 0, 0};
    if (!tud_hid_keyboard_report(USB_HID_REPORT_ID_KEYBOARD, modifier, keycodes)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t usb_hid_device_release(void)
{
    return usb_hid_device_release_best_effort(USB_HID_READY_TIMEOUT_MS);
}

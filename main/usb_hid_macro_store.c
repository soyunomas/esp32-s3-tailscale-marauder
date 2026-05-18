#include <stdio.h>
#include <string.h>
#include "usb_hid_macro_store.h"
#include "nvs.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "usb_hid_store";
static const char *NVS_NAMESPACE = "usb_hid";

static bool printable_multiline_ascii(const char *value)
{
    for (size_t i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '\r' || c == '\n' || c == '\t') continue;
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

bool usb_hid_macro_name_valid(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len == 0 || len >= USB_HID_MACRO_NAME_LEN) return false;
    return printable_multiline_ascii(name);
}

bool usb_hid_macro_layout_valid(const char *layout)
{
    static const char *const valid[] = {
        "es", "us", "uk", "latam", "fr", "de", "it", "pt", "br", "nordic", "be", "tr",
        "pl", "cz", "sk", "hu", "ro", "hr", "sr", "sl", "bg", "ru", "ua", "zh", "tw"
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (layout && strcmp(layout, valid[i]) == 0) return true;
    }
    return false;
}

bool usb_hid_macro_script_valid(const char *script)
{
    size_t len = script ? strlen(script) : 0;
    if (len == 0 || len >= USB_HID_MACRO_SCRIPT_LEN) return false;
    return printable_multiline_ascii(script);
}

static void key_for(char *out, size_t out_size, const char *prefix, int slot)
{
    snprintf(out, out_size, "%s%d", prefix, slot);
}

static esp_err_t read_slot(nvs_handle_t handle, int slot, usb_hid_macro_t *item)
{
    char key[24];
    uint32_t id = 0;
    key_for(key, sizeof(key), "id", slot);
    esp_err_t ret = nvs_get_u32(handle, key, &id);
    if (ret != ESP_OK || id == 0) return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret;

    memset(item, 0, sizeof(*item));
    item->id = id;

    size_t len = sizeof(item->name);
    key_for(key, sizeof(key), "name", slot);
    ret = nvs_get_str(handle, key, item->name, &len);
    if (ret != ESP_OK) return ret;

    len = sizeof(item->script);
    key_for(key, sizeof(key), "script", slot);
    ret = nvs_get_str(handle, key, item->script, &len);
    if (ret != ESP_OK) return ret;

    len = sizeof(item->layout);
    key_for(key, sizeof(key), "layout", slot);
    ret = nvs_get_str(handle, key, item->layout, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(item->layout, USB_HID_MACRO_DEFAULT_LAYOUT, sizeof(item->layout));
    } else if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

static esp_err_t write_slot(nvs_handle_t handle, int slot, const usb_hid_macro_t *item, uint32_t id)
{
    char key[24];
    key_for(key, sizeof(key), "id", slot);
    ESP_RETURN_ON_ERROR(nvs_set_u32(handle, key, id), TAG, "set id failed");
    key_for(key, sizeof(key), "name", slot);
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, key, item->name), TAG, "set name failed");
    key_for(key, sizeof(key), "script", slot);
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, key, item->script), TAG, "set script failed");
    key_for(key, sizeof(key), "layout", slot);
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, key, item->layout), TAG, "set layout failed");
    return ESP_OK;
}

static void erase_slot(nvs_handle_t handle, int slot)
{
    char key[24];
    key_for(key, sizeof(key), "id", slot);
    nvs_erase_key(handle, key);
    key_for(key, sizeof(key), "name", slot);
    nvs_erase_key(handle, key);
    key_for(key, sizeof(key), "script", slot);
    nvs_erase_key(handle, key);
    key_for(key, sizeof(key), "layout", slot);
    nvs_erase_key(handle, key);
}

esp_err_t usb_hid_macro_store_list(usb_hid_macro_t *items, size_t max_items, size_t *count)
{
    if (!items || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;

    for (int slot = 0; slot < USB_HID_MACRO_MAX && *count < max_items; slot++) {
        if (read_slot(handle, slot, &items[*count]) == ESP_OK) {
            (*count)++;
        }
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t usb_hid_macro_store_get(uint32_t id, usb_hid_macro_t *item)
{
    if (id == 0 || !item) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) return ret;

    for (int slot = 0; slot < USB_HID_MACRO_MAX; slot++) {
        if (read_slot(handle, slot, item) == ESP_OK && item->id == id) {
            nvs_close(handle);
            return ESP_OK;
        }
    }

    nvs_close(handle);
    memset(item, 0, sizeof(*item));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t usb_hid_macro_store_save(const usb_hid_macro_t *item, uint32_t *saved_id)
{
    if (!item || !saved_id ||
        !usb_hid_macro_name_valid(item->name) ||
        !usb_hid_macro_layout_valid(item->layout) ||
        !usb_hid_macro_script_valid(item->script)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    usb_hid_macro_t *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    int target_slot = -1;
    uint32_t id = item->id;
    for (int slot = 0; slot < USB_HID_MACRO_MAX; slot++) {
        if (read_slot(handle, slot, candidate) == ESP_OK) {
            if (id != 0 && candidate->id == id) {
                target_slot = slot;
                break;
            }
        } else if (target_slot < 0) {
            target_slot = slot;
        }
    }
    free(candidate);

    if (target_slot < 0) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    if (id == 0) {
        uint32_t next_id = 1;
        nvs_get_u32(handle, "next_id", &next_id);
        id = next_id == 0 ? 1 : next_id;
        nvs_set_u32(handle, "next_id", id + 1);
    }

    ret = write_slot(handle, target_slot, item, id);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret == ESP_OK) *saved_id = id;
    return ret;
}

esp_err_t usb_hid_macro_store_delete(uint32_t id)
{
    if (id == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    usb_hid_macro_t *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    for (int slot = 0; slot < USB_HID_MACRO_MAX; slot++) {
        if (read_slot(handle, slot, candidate) == ESP_OK && candidate->id == id) {
            erase_slot(handle, slot);
            ret = nvs_commit(handle);
            free(candidate);
            nvs_close(handle);
            return ret;
        }
    }

    free(candidate);
    nvs_close(handle);
    return ESP_ERR_NOT_FOUND;
}

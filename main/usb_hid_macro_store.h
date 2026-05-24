#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define USB_HID_MACRO_MAX_METADATA 512
#define USB_HID_MACRO_NAME_LEN 33
#define USB_HID_MACRO_SCRIPT_MAX_BYTES (512 * 1024)
#define USB_HID_MACRO_LAYOUT_LEN 16
#define USB_HID_MACRO_DEFAULT_LAYOUT "es"

typedef struct {
    uint32_t id;
    char name[USB_HID_MACRO_NAME_LEN];
    char layout[USB_HID_MACRO_LAYOUT_LEN];
    size_t script_len;
    char *script;
} usb_hid_macro_t;

typedef struct {
    bool available;
    size_t max_macros;
    size_t used_macros;
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
} usb_hid_macro_storage_stats_t;

esp_err_t usb_hid_macro_store_init(void);
esp_err_t usb_hid_macro_store_list(usb_hid_macro_t *items, size_t max_items, size_t *count);
esp_err_t usb_hid_macro_store_get(uint32_t id, usb_hid_macro_t *item);
esp_err_t usb_hid_macro_store_save(const usb_hid_macro_t *item, uint32_t *saved_id);
esp_err_t usb_hid_macro_store_delete(uint32_t id);
esp_err_t usb_hid_macro_store_stats(usb_hid_macro_storage_stats_t *stats);
void usb_hid_macro_free(usb_hid_macro_t *item);
bool usb_hid_macro_name_valid(const char *name);
bool usb_hid_macro_layout_valid(const char *layout);
bool usb_hid_macro_script_valid(const char *script);

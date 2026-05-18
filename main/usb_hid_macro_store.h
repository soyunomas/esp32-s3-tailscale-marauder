#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define USB_HID_MACRO_MAX 12
#define USB_HID_MACRO_NAME_LEN 33
#define USB_HID_MACRO_SCRIPT_LEN 4096
#define USB_HID_MACRO_LAYOUT_LEN 16
#define USB_HID_MACRO_DEFAULT_LAYOUT "es"

typedef struct {
    uint32_t id;
    char name[USB_HID_MACRO_NAME_LEN];
    char layout[USB_HID_MACRO_LAYOUT_LEN];
    char script[USB_HID_MACRO_SCRIPT_LEN];
} usb_hid_macro_t;

esp_err_t usb_hid_macro_store_list(usb_hid_macro_t *items, size_t max_items, size_t *count);
esp_err_t usb_hid_macro_store_get(uint32_t id, usb_hid_macro_t *item);
esp_err_t usb_hid_macro_store_save(const usb_hid_macro_t *item, uint32_t *saved_id);
esp_err_t usb_hid_macro_store_delete(uint32_t id);
bool usb_hid_macro_name_valid(const char *name);
bool usb_hid_macro_layout_valid(const char *layout);
bool usb_hid_macro_script_valid(const char *script);

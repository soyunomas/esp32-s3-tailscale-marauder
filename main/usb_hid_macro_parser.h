#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_HID_PARSE_ERROR_LEN 96

typedef struct {
    bool ok;
    uint32_t line;
    char message[USB_HID_PARSE_ERROR_LEN];
} usb_hid_parse_result_t;

usb_hid_parse_result_t usb_hid_macro_validate(const char *script);

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define USB_HID_MOD_CTRL  0x01
#define USB_HID_MOD_SHIFT 0x02
#define USB_HID_MOD_ALT   0x04
#define USB_HID_MOD_GUI   0x08
#define USB_HID_MOD_RIGHT_ALT 0x40

esp_err_t usb_hid_device_init(void);
bool usb_hid_device_available(void);
bool usb_hid_device_ready(void);
esp_err_t usb_hid_device_press(uint8_t modifier, uint8_t keycode);
esp_err_t usb_hid_device_release(void);
esp_err_t usb_hid_device_release_best_effort(uint32_t timeout_ms);

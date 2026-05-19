#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define USB_HID_MOD_CTRL  0x01
#define USB_HID_MOD_SHIFT 0x02
#define USB_HID_MOD_ALT   0x04
#define USB_HID_MOD_GUI   0x08
#define USB_HID_MOD_RIGHT_ALT 0x40

/* HID Output Report LED bitmask (mirrors TinyUSB KEYBOARD_LED_*) */
#define USB_HID_LED_NUMLOCK    0x01
#define USB_HID_LED_CAPSLOCK   0x02
#define USB_HID_LED_SCROLLLOCK 0x04
#define USB_HID_LED_COMPOSE    0x08
#define USB_HID_LED_KANA       0x10

esp_err_t usb_hid_device_init(void);
bool usb_hid_device_available(void);
bool usb_hid_device_ready(void);
esp_err_t usb_hid_device_press(uint8_t modifier, uint8_t keycode);
esp_err_t usb_hid_device_release(void);
esp_err_t usb_hid_device_release_best_effort(uint32_t timeout_ms);

/* LED feedback (host-driven). Safe to call from any FreeRTOS task. */
bool usb_hid_device_get_led_state(uint8_t led_mask);
uint8_t usb_hid_device_get_led_byte(void);

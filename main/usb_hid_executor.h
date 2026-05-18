#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb_hid_macro_store.h"

typedef enum {
    USB_HID_EXEC_IDLE = 0,
    USB_HID_EXEC_RUNNING,
    USB_HID_EXEC_STOPPING,
    USB_HID_EXEC_DONE,
    USB_HID_EXEC_ERROR
} usb_hid_exec_state_t;

typedef struct {
    usb_hid_exec_state_t state;
    bool dry_run;
    uint16_t current_line;
    uint16_t total_lines;
    char macro_name[USB_HID_MACRO_NAME_LEN];
    char message[96];
} usb_hid_exec_status_t;

esp_err_t usb_hid_executor_init(void);
esp_err_t usb_hid_executor_start(const usb_hid_macro_t *macro);
esp_err_t usb_hid_executor_stop(void);
esp_err_t usb_hid_executor_panic_stop(void);
void usb_hid_executor_get_status(usb_hid_exec_status_t *status);
const char *usb_hid_exec_state_name(usb_hid_exec_state_t state);

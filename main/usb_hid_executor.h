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

#define USB_HID_KEEPALIVE_KEY_LEN 16

typedef struct {
    bool enabled;
    bool paused;
    bool dry_run;
    bool ready;
    uint16_t interval_s;
    uint32_t sent_count;
    char key[USB_HID_KEEPALIVE_KEY_LEN];
    char message[96];
} usb_hid_keepalive_status_t;

esp_err_t usb_hid_executor_init(void);
esp_err_t usb_hid_executor_start(const usb_hid_macro_t *macro);
esp_err_t usb_hid_executor_stop(void);
esp_err_t usb_hid_executor_panic_stop(void);
void usb_hid_executor_get_status(usb_hid_exec_status_t *status);
const char *usb_hid_exec_state_name(usb_hid_exec_state_t state);
bool usb_hid_keepalive_key_valid(const char *key);
esp_err_t usb_hid_executor_configure_keepalive(bool enabled, const char *key, uint16_t interval_s);
void usb_hid_executor_get_keepalive_status(usb_hid_keepalive_status_t *status);

/* Resolve a DuckyScript internal variable (e.g. "$_CAPSLOCK_ON") to an integer.
 * Returns true if recognized, false otherwise. Designed to be called from the
 * Phase 3 expression evaluator. Safe to call from any FreeRTOS task. */
bool usb_hid_executor_resolve_internal_var(const char *name, int *value_out);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DuckyScript 3.0 integer expression evaluator.
 * Limits chosen to keep the whole context under ~1.3 KB so it fits in the
 * executor task stack budget (4 KB) alongside line_buf and the device state. */
#define USB_HID_EVAL_MAX_VARS         20
#define USB_HID_EVAL_MAX_DEFINES      16
#define USB_HID_EVAL_MAX_NAME         24    /* includes '$' or '#' prefix + NUL */
#define USB_HID_EVAL_MAX_DEFINE_VALUE 48
#define USB_HID_EVAL_MAX_STRING_VALUE 48
#define USB_HID_EVAL_MAX_DEPTH        4     /* max #define re-entrancy */

typedef struct {
    char name[USB_HID_EVAL_MAX_NAME];   /* "$X" */
    int  value;
    char string_value[USB_HID_EVAL_MAX_STRING_VALUE];
    bool is_string;
    bool used;
} usb_hid_eval_var_t;

typedef struct {
    char name[USB_HID_EVAL_MAX_NAME];                /* "#X" */
    char value[USB_HID_EVAL_MAX_DEFINE_VALUE];
    bool used;
} usb_hid_eval_define_t;

typedef struct {
    usb_hid_eval_var_t    vars[USB_HID_EVAL_MAX_VARS];
    usb_hid_eval_define_t defines[USB_HID_EVAL_MAX_DEFINES];
} usb_hid_eval_ctx_t;

void usb_hid_eval_init(usb_hid_eval_ctx_t *ctx);

bool usb_hid_eval_set_var(usb_hid_eval_ctx_t *ctx, const char *name, int value);
bool usb_hid_eval_get_var(const usb_hid_eval_ctx_t *ctx, const char *name, int *out);
bool usb_hid_eval_set_string_var(usb_hid_eval_ctx_t *ctx, const char *name, const char *value);
bool usb_hid_eval_get_var_text(const usb_hid_eval_ctx_t *ctx, const char *name,
                               char *out, size_t out_size);

bool usb_hid_eval_add_define(usb_hid_eval_ctx_t *ctx, const char *name, const char *value);
const char *usb_hid_eval_get_define(const usb_hid_eval_ctx_t *ctx, const char *name);

/* Evaluate an integer expression. Returns false on syntax error, unknown
 * identifier, unterminated expression or stack overflow.
 * Operators: + - * / %  &  |  ^  &&  ||  !  ~  == != < <= > >=
 * Primaries: integer literal, $USER, $_INTERNAL (via executor), #DEFINE,
 *            RANDOM_INT(a,b), TRUE, FALSE, parenthesized sub-expression. */
bool usb_hid_eval_expression(usb_hid_eval_ctx_t *ctx, const char *expr, int *result);

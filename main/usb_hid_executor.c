#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "usb_hid_executor.h"
#include "usb_hid_device.h"
#include "usb_hid_evaluator.h"
#include "usb_hid_macro_parser.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    uint32_t default_delay_ms; /* added to every per-command wait */
    uint8_t  jitter_percent;   /* 0..100 random variation applied to delays */
    usb_hid_eval_ctx_t eval;   /* user vars, defines, expression state */
} usb_hid_exec_ctx_t;

static uint32_t apply_jitter(uint32_t base_ms, uint8_t jitter_percent)
{
    if (jitter_percent == 0 || base_ms == 0) return base_ms;
    uint32_t max_var = (base_ms * jitter_percent) / 100U;
    if (max_var == 0) return base_ms;
    uint32_t variation = esp_random() % (max_var + 1U);
    return base_ms + variation;
}

#define USB_HID_EXEC_QUEUE_LEN 1
#define USB_HID_EXEC_TASK_STACK 8192   /* enlarged for eval ctx (~2 KB) */
#define USB_HID_EXEC_MAX_RUNTIME_MS 60000
#define USB_HID_EXEC_MAX_DELAY_MS 10000
#define USB_HID_EXEC_DEFAULT_STEP_MS 5
#define USB_HID_EXEC_KEY_PRESS_MS 30
#define USB_HID_EXEC_KEY_RELEASE_SETTLE_MS 20
#define USB_HID_EXEC_PANIC_RELEASE_MS 250
#define USB_HID_KEEPALIVE_TASK_STACK 3072

static const char *TAG = "usb_hid_exec";

typedef struct {
    usb_hid_macro_t macro;
} usb_hid_exec_request_t;

typedef struct {
    uint16_t offset;
    uint16_t len;
} usb_hid_line_ref_t;

#define USB_HID_EXEC_CALL_DEPTH 4

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_keepalive_lock;
static SemaphoreHandle_t s_hid_io_lock;
static volatile bool s_stop_requested;
static volatile bool s_executing;
static volatile bool s_macro_reserved;
static bool s_dry_run = true;
static usb_hid_exec_status_t s_status = {
    .state = USB_HID_EXEC_IDLE,
    .dry_run = true,
    .message = "Idle",
};
static bool s_keepalive_enabled;
static uint16_t s_keepalive_interval_s = 60;
static char s_keepalive_key[USB_HID_KEEPALIVE_KEY_LEN] = "SCROLLLOCK";
static uint32_t s_keepalive_sent_count;
static char s_keepalive_message[96] = "Disabled";

bool usb_hid_executor_resolve_internal_var(const char *name, int *value_out)
{
    if (!name || !value_out) return false;
    if (strcmp(name, "$_CAPSLOCK_ON") == 0) {
        *value_out = usb_hid_device_get_led_state(USB_HID_LED_CAPSLOCK) ? 1 : 0;
        return true;
    }
    if (strcmp(name, "$_NUMLOCK_ON") == 0) {
        *value_out = usb_hid_device_get_led_state(USB_HID_LED_NUMLOCK) ? 1 : 0;
        return true;
    }
    if (strcmp(name, "$_SCROLLLOCK_ON") == 0) {
        *value_out = usb_hid_device_get_led_state(USB_HID_LED_SCROLLLOCK) ? 1 : 0;
        return true;
    }
    if (strcmp(name, "$_BUTTON_ENABLED") == 0) {
        *value_out = 0; /* button hardware not wired yet -- placeholder */
        return true;
    }
    if (strcmp(name, "$_HOST_CONFIGURATION_REQUEST_COUNT") == 0) {
        *value_out = 0; /* TODO Phase 3: track via tud_mount_cb counter */
        return true;
    }
    return false;
}

const char *usb_hid_exec_state_name(usb_hid_exec_state_t state)
{
    switch (state) {
    case USB_HID_EXEC_IDLE: return "idle";
    case USB_HID_EXEC_RUNNING: return "running";
    case USB_HID_EXEC_STOPPING: return "stopping";
    case USB_HID_EXEC_DONE: return "done";
    case USB_HID_EXEC_ERROR: return "error";
    }
    return "unknown";
}

static void set_status(usb_hid_exec_state_t state, const usb_hid_macro_t *macro,
                       uint16_t current_line, uint16_t total_lines,
                       const char *message)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    s_status.dry_run = s_dry_run;
    s_status.current_line = current_line;
    s_status.total_lines = total_lines;
    if (macro) {
        strlcpy(s_status.macro_name, macro->name, sizeof(s_status.macro_name));
    }
    if (message) {
        strlcpy(s_status.message, message, sizeof(s_status.message));
    }
    if (s_lock) xSemaphoreGive(s_lock);
}

void usb_hid_executor_get_status(usb_hid_exec_status_t *status)
{
    if (!status) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    if (s_lock) xSemaphoreGive(s_lock);
}

static uint16_t count_lines(const char *script)
{
    uint16_t count = 0;
    bool has_content = false;
    for (size_t i = 0; script[i] != '\0'; i++) {
        if (script[i] != '\r' && script[i] != '\n') has_content = true;
        if (script[i] == '\n') count++;
    }
    return has_content ? count + 1 : 0;
}

static char *trim_left(char *value)
{
    while (*value && isspace((unsigned char)*value)) value++;
    return value;
}

static void trim_right(char *value)
{
    size_t len = strlen(value);
    while (len > 0 && isspace((unsigned char)value[len - 1])) {
        value[len - 1] = '\0';
        len--;
    }
}

static uint32_t delay_from_line(char *line)
{
    char *trimmed = trim_left(line);
    trim_right(trimmed);
    if (strncasecmp(trimmed, "DELAY", 5) != 0) return 1;
    char *arg = trim_left(trimmed + 5);
    if (*arg == '\0') return 1;
    long value = strtol(arg, NULL, 10);
    if (value < 0) return 1;
    if (value > USB_HID_EXEC_MAX_DELAY_MS) return USB_HID_EXEC_MAX_DELAY_MS;
    return (uint32_t)value;
}

/* Evaluator-aware version: accepts DELAY <expression> ($X * 100, RANDOM_INT(10,50), ...).
 * Falls back to the simple parser if evaluation fails (defensive). */
static uint32_t compute_command_delay_ms(char *line, usb_hid_eval_ctx_t *eval)
{
    char *trimmed = trim_left(line);
    trim_right(trimmed);
    if (strncasecmp(trimmed, "DELAY", 5) != 0) return 1;
    char *arg = trim_left(trimmed + 5);
    if (*arg == '\0') return 1;
    int value = 0;
    if (!usb_hid_eval_expression(eval, arg, &value)) {
        long lv = strtol(arg, NULL, 10);
        value = (int)lv;
    }
    if (value < 0) value = 1;
    if (value > USB_HID_EXEC_MAX_DELAY_MS) value = USB_HID_EXEC_MAX_DELAY_MS;
    return (uint32_t)value;
}

static uint32_t compute_delay_arg_ms(const char *arg, usb_hid_eval_ctx_t *eval)
{
    if (!arg) return 1;
    while (*arg && isspace((unsigned char)*arg)) arg++;
    if (*arg == '\0') return 1;

    int value = 0;
    if (!usb_hid_eval_expression(eval, arg, &value)) {
        long lv = strtol(arg, NULL, 10);
        value = (int)lv;
    }
    if (value < 0) value = 1;
    if (value > USB_HID_EXEC_MAX_DELAY_MS) value = USB_HID_EXEC_MAX_DELAY_MS;
    return (uint32_t)value;
}

static void normalize_command_alias_exec(char *command, size_t command_size)
{
    static const struct { const char *alias; const char *canonical; } aliases[] = {
        {"UP", "UPARROW"},
        {"DOWN", "DOWNARROW"},
        {"LEFT", "LEFTARROW"},
        {"RIGHT", "RIGHTARROW"},
        {"ESC", "ESCAPE"},
        {"CONTROL", "CTRL"},
        {"OPTION", "ALT"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (strcmp(command, aliases[i].alias) == 0) {
            strlcpy(command, aliases[i].canonical, command_size);
            return;
        }
    }
}

static bool command_from_line(char *line, char *command, size_t command_size, char **args)
{
    char *trimmed = trim_left(line);
    trim_right(trimmed);
    if (trimmed[0] == '\0') return false;
    /* `//` is an exact alias of REM: treat the whole line as a comment. */
    if (trimmed[0] == '/' && trimmed[1] == '/') {
        strlcpy(command, "REM", command_size);
        *args = trimmed + 2;
        trim_right(*args);
        return true;
    }

    size_t i = 0;
    while (trimmed[i] && !isspace((unsigned char)trimmed[i]) && i < command_size - 1) {
        command[i] = (char)toupper((unsigned char)trimmed[i]);
        i++;
    }
    command[i] = '\0';
    *args = trim_left(trimmed + i);
    trim_right(*args);
    normalize_command_alias_exec(command, command_size);
    return command[0] != '\0';
}

static void strip_trailing_then_exec(char *value)
{
    size_t len = strlen(value);
    while (len > 0 && isspace((unsigned char)value[len - 1])) len--;
    if (len < 4) return;

    const char *tail = value + len - 4;
    if (strncasecmp(tail, "THEN", 4) == 0 &&
        (len == 4 || isspace((unsigned char)value[len - 5]) || value[len - 5] == ')')) {
        value[len - 4] = '\0';
        trim_right(value);
    }
}

static size_t build_line_refs(const char *script, usb_hid_line_ref_t *lines, size_t max_lines)
{
    size_t count = 0;
    size_t pos = 0;
    while (script[pos] != '\0' && count < max_lines) {
        size_t start = pos;
        while (script[pos] != '\0' && script[pos] != '\n' && script[pos] != '\r') pos++;
        lines[count].offset = (uint16_t)start;
        lines[count].len = (uint16_t)(pos - start);
        count++;
        if (script[pos] == '\r') pos++;
        if (script[pos] == '\n') pos++;
    }
    return count;
}

static bool line_command_at(const char *script, const usb_hid_line_ref_t *lines,
                            size_t line_index, char *command, size_t command_size)
{
    char line_buf[192];
    if (lines[line_index].len >= sizeof(line_buf)) return false;
    memcpy(line_buf, script + lines[line_index].offset, lines[line_index].len);
    line_buf[lines[line_index].len] = '\0';

    char *args = NULL;
    return command_from_line(line_buf, command, command_size, &args);
}

static bool find_if_peer_forward(const char *script, const usb_hid_line_ref_t *lines,
                                 size_t line_count, size_t if_index,
                                 bool want_else, size_t *target_out)
{
    int depth = 0;
    for (size_t i = if_index + 1; i < line_count; i++) {
        char command[32] = {0};
        if (!line_command_at(script, lines, i, command, sizeof(command))) continue;

        if (strcmp(command, "IF") == 0) {
            depth++;
        } else if (strcmp(command, "END_IF") == 0) {
            if (depth == 0) {
                *target_out = i;
                return true;
            }
            depth--;
        } else if (want_else && strcmp(command, "ELSE") == 0 && depth == 0) {
            *target_out = i;
            return true;
        }
    }
    return false;
}

static bool find_matching_while_backward(const char *script, const usb_hid_line_ref_t *lines,
                                         size_t from_index, size_t *target_out)
{
    int depth = 0;
    for (size_t i = from_index; i > 0; i--) {
        size_t idx = i - 1;
        char command[32] = {0};
        if (!line_command_at(script, lines, idx, command, sizeof(command))) continue;

        if (strcmp(command, "END_WHILE") == 0) {
            depth++;
        } else if (strcmp(command, "WHILE") == 0) {
            if (depth == 0) {
                *target_out = idx;
                return true;
            }
            depth--;
        }
    }
    return false;
}

static bool find_matching_end_while_forward(const char *script, const usb_hid_line_ref_t *lines,
                                            size_t line_count, size_t from_index,
                                            size_t *target_out)
{
    int depth = 0;
    for (size_t i = from_index + 1; i < line_count; i++) {
        char command[32] = {0};
        if (!line_command_at(script, lines, i, command, sizeof(command))) continue;

        if (strcmp(command, "WHILE") == 0) {
            depth++;
        } else if (strcmp(command, "END_WHILE") == 0) {
            if (depth == 0) {
                *target_out = i;
                return true;
            }
            depth--;
        }
    }
    return false;
}

static bool find_end_function_forward(const char *script, const usb_hid_line_ref_t *lines,
                                      size_t line_count, size_t from_index,
                                      size_t *target_out)
{
    int depth = 0;
    for (size_t i = from_index + 1; i < line_count; i++) {
        char command[32] = {0};
        if (!line_command_at(script, lines, i, command, sizeof(command))) continue;

        if (strcmp(command, "FUNCTION") == 0) {
            depth++;
        } else if (strcmp(command, "END_FUNCTION") == 0) {
            if (depth == 0) {
                *target_out = i;
                return true;
            }
            depth--;
        }
    }
    return false;
}

static bool command_is_function_call(const char *command, char *name_out, size_t name_size)
{
    size_t len = command ? strlen(command) : 0;
    if (len < 3 || command[len - 2] != '(' || command[len - 1] != ')') return false;
    if (len - 2 >= name_size) return false;
    memcpy(name_out, command, len - 2);
    name_out[len - 2] = '\0';
    if (!isalpha((unsigned char)name_out[0]) && name_out[0] != '_') return false;
    for (size_t i = 1; name_out[i] != '\0'; i++) {
        if (!isalnum((unsigned char)name_out[i]) && name_out[i] != '_') return false;
    }
    return true;
}

static bool function_def_matches(const char *args, const char *name)
{
    char buf[64];
    strlcpy(buf, args, sizeof(buf));
    char *value = trim_left(buf);
    trim_right(value);
    size_t len = strlen(value);
    if (len < 3 || value[len - 2] != '(' || value[len - 1] != ')') return false;
    value[len - 2] = '\0';
    return strcasecmp(value, name) == 0;
}

static bool find_function_forward(const char *script, const usb_hid_line_ref_t *lines,
                                  size_t line_count, const char *name,
                                  size_t *target_out)
{
    for (size_t i = 0; i < line_count; i++) {
        char line_buf[192];
        if (lines[i].len >= sizeof(line_buf)) continue;
        memcpy(line_buf, script + lines[i].offset, lines[i].len);
        line_buf[lines[i].len] = '\0';
        char command[32] = {0};
        char *args = NULL;
        if (!command_from_line(line_buf, command, sizeof(command), &args)) continue;
        if (strcmp(command, "FUNCTION") == 0 && function_def_matches(args, name)) {
            *target_out = i;
            return true;
        }
    }
    return false;
}

static uint32_t dry_run_wait_for_command(const char *command, char *line)
{
    if (strcmp(command, "DELAY") == 0) return delay_from_line(line);
    if (strcmp(command, "WAIT_FOR_BUTTON_PRESS") == 0) return 50;
    return USB_HID_EXEC_DEFAULT_STEP_MS;
}

static bool dry_run_command_finishes_macro(const char *command, char *message, size_t message_size)
{
    if (strcmp(command, "STOP_PAYLOAD") == 0) {
        strlcpy(message, "Stopped by STOP_PAYLOAD", message_size);
        return true;
    }
    if (strcmp(command, "RESTART_PAYLOAD") == 0) {
        strlcpy(message, "Restart requested (dry run)", message_size);
        return true;
    }
    if (strcmp(command, "LOOP") == 0) {
        strlcpy(message, "LOOP reached (dry run)", message_size);
        return true;
    }
    return false;
}

#define HID_KEY_NONE          0x00
#define HID_KEY_A             0x04
#define HID_KEY_Q             0x14
#define HID_KEY_1             0x1e
#define HID_KEY_ENTER         0x28
#define HID_KEY_ESCAPE        0x29
#define HID_KEY_BACKSPACE     0x2a
#define HID_KEY_TAB           0x2b
#define HID_KEY_SPACE         0x2c
#define HID_KEY_MINUS         0x2d
#define HID_KEY_EQUAL         0x2e
#define HID_KEY_BRACKET_LEFT  0x2f
#define HID_KEY_BRACKET_RIGHT 0x30
#define HID_KEY_BACKSLASH     0x31
#define HID_KEY_SEMICOLON     0x33
#define HID_KEY_APOSTROPHE    0x34
#define HID_KEY_GRAVE         0x35
#define HID_KEY_COMMA         0x36
#define HID_KEY_PERIOD        0x37
#define HID_KEY_SLASH         0x38
#define HID_KEY_CAPS_LOCK     0x39
#define HID_KEY_F1            0x3a
#define HID_KEY_SCROLL_LOCK   0x47
#define HID_KEY_PRINT_SCREEN  0x46
#define HID_KEY_PAUSE         0x48
#define HID_KEY_INSERT        0x49
#define HID_KEY_HOME          0x4a
#define HID_KEY_PAGE_UP       0x4b
#define HID_KEY_DELETE        0x4c
#define HID_KEY_END           0x4d
#define HID_KEY_PAGE_DOWN     0x4e
#define HID_KEY_ARROW_RIGHT   0x4f
#define HID_KEY_ARROW_LEFT    0x50
#define HID_KEY_ARROW_DOWN    0x51
#define HID_KEY_ARROW_UP      0x52
#define HID_KEY_NUM_LOCK      0x53
#define HID_KEY_NON_US_BACKSLASH 0x64
#define HID_KEY_MENU          0x65

typedef struct {
    uint8_t modifier;
    uint8_t keycode;
} usb_hid_key_t;

typedef enum {
    USB_HID_LAYOUT_ES,
    USB_HID_LAYOUT_US,
    USB_HID_LAYOUT_UK,
    USB_HID_LAYOUT_LATAM,
    USB_HID_LAYOUT_FR,
    USB_HID_LAYOUT_DE,
    USB_HID_LAYOUT_IT,
    USB_HID_LAYOUT_PT,
    USB_HID_LAYOUT_BR,
    USB_HID_LAYOUT_NORDIC,
    USB_HID_LAYOUT_BE,
    USB_HID_LAYOUT_TR,
    USB_HID_LAYOUT_PL,
    USB_HID_LAYOUT_CZ,
    USB_HID_LAYOUT_SK,
    USB_HID_LAYOUT_HU,
    USB_HID_LAYOUT_RO,
    USB_HID_LAYOUT_HR,
    USB_HID_LAYOUT_SR,
    USB_HID_LAYOUT_SL,
    USB_HID_LAYOUT_BG,
    USB_HID_LAYOUT_RU,
    USB_HID_LAYOUT_UA,
    USB_HID_LAYOUT_ZH,
    USB_HID_LAYOUT_TW,
} usb_hid_layout_t;

static usb_hid_layout_t layout_from_name(const char *layout)
{
    if (!layout || layout[0] == '\0' || strcmp(layout, "es") == 0) return USB_HID_LAYOUT_ES;
    if (strcmp(layout, "us") == 0) return USB_HID_LAYOUT_US;
    if (strcmp(layout, "uk") == 0) return USB_HID_LAYOUT_UK;
    if (strcmp(layout, "latam") == 0) return USB_HID_LAYOUT_LATAM;
    if (strcmp(layout, "fr") == 0) return USB_HID_LAYOUT_FR;
    if (strcmp(layout, "de") == 0) return USB_HID_LAYOUT_DE;
    if (strcmp(layout, "it") == 0) return USB_HID_LAYOUT_IT;
    if (strcmp(layout, "pt") == 0) return USB_HID_LAYOUT_PT;
    if (strcmp(layout, "br") == 0) return USB_HID_LAYOUT_BR;
    if (strcmp(layout, "nordic") == 0) return USB_HID_LAYOUT_NORDIC;
    if (strcmp(layout, "be") == 0) return USB_HID_LAYOUT_BE;
    if (strcmp(layout, "tr") == 0) return USB_HID_LAYOUT_TR;
    if (strcmp(layout, "pl") == 0) return USB_HID_LAYOUT_PL;
    if (strcmp(layout, "cz") == 0) return USB_HID_LAYOUT_CZ;
    if (strcmp(layout, "sk") == 0) return USB_HID_LAYOUT_SK;
    if (strcmp(layout, "hu") == 0) return USB_HID_LAYOUT_HU;
    if (strcmp(layout, "ro") == 0) return USB_HID_LAYOUT_RO;
    if (strcmp(layout, "hr") == 0) return USB_HID_LAYOUT_HR;
    if (strcmp(layout, "sr") == 0) return USB_HID_LAYOUT_SR;
    if (strcmp(layout, "sl") == 0) return USB_HID_LAYOUT_SL;
    if (strcmp(layout, "bg") == 0) return USB_HID_LAYOUT_BG;
    if (strcmp(layout, "ru") == 0) return USB_HID_LAYOUT_RU;
    if (strcmp(layout, "ua") == 0) return USB_HID_LAYOUT_UA;
    if (strcmp(layout, "zh") == 0) return USB_HID_LAYOUT_ZH;
    if (strcmp(layout, "tw") == 0) return USB_HID_LAYOUT_TW;
    return USB_HID_LAYOUT_ES;
}

static bool layout_is_azerty(usb_hid_layout_t layout)
{
    return layout == USB_HID_LAYOUT_FR || layout == USB_HID_LAYOUT_BE;
}

static bool layout_is_qwertz(usb_hid_layout_t layout)
{
    return layout == USB_HID_LAYOUT_DE ||
           layout == USB_HID_LAYOUT_CZ ||
           layout == USB_HID_LAYOUT_SK ||
           layout == USB_HID_LAYOUT_HU ||
           layout == USB_HID_LAYOUT_HR ||
           layout == USB_HID_LAYOUT_SR ||
           layout == USB_HID_LAYOUT_SL;
}

static bool layout_uses_spanish_punctuation(usb_hid_layout_t layout)
{
    return layout == USB_HID_LAYOUT_ES || layout == USB_HID_LAYOUT_LATAM ||
           layout == USB_HID_LAYOUT_IT || layout == USB_HID_LAYOUT_PT;
}

static bool modifier_from_token(const char *token, uint8_t *modifier)
{
    if (strcasecmp(token, "CTRL") == 0 ||
        strcasecmp(token, "CONTROL") == 0) {
        *modifier = USB_HID_MOD_CTRL;
    } else if (strcasecmp(token, "SHIFT") == 0) {
        *modifier = USB_HID_MOD_SHIFT;
    } else if (strcasecmp(token, "ALT") == 0 ||
               strcasecmp(token, "OPTION") == 0) {
        *modifier = USB_HID_MOD_ALT;
    } else if (strcasecmp(token, "GUI") == 0 ||
               strcasecmp(token, "WINDOWS") == 0 ||
               strcasecmp(token, "COMMAND") == 0) {
        *modifier = USB_HID_MOD_GUI;
    } else {
        return false;
    }
    return true;
}

static bool key_from_ascii(char c, const char *layout_name, usb_hid_key_t *key)
{
    memset(key, 0, sizeof(*key));
    usb_hid_layout_t layout = layout_from_name(layout_name);

    if (c >= 'a' && c <= 'z') {
        char mapped = c;
        if (layout_is_azerty(layout)) {
            if (mapped == 'a') mapped = 'q';
            else if (mapped == 'q') mapped = 'a';
            else if (mapped == 'z') mapped = 'w';
            else if (mapped == 'w') mapped = 'z';
            else if (mapped == 'm') {
                key->keycode = HID_KEY_SEMICOLON;
                return true;
            }
        } else if (layout_is_qwertz(layout)) {
            if (mapped == 'y') mapped = 'z';
            else if (mapped == 'z') mapped = 'y';
        }
        key->keycode = HID_KEY_A + (uint8_t)(mapped - 'a');
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        key->modifier = USB_HID_MOD_SHIFT;
        char mapped = (char)tolower((unsigned char)c);
        if (layout_is_azerty(layout)) {
            if (mapped == 'a') mapped = 'q';
            else if (mapped == 'q') mapped = 'a';
            else if (mapped == 'z') mapped = 'w';
            else if (mapped == 'w') mapped = 'z';
            else if (mapped == 'm') {
                key->keycode = HID_KEY_SEMICOLON;
                return true;
            }
        } else if (layout_is_qwertz(layout)) {
            if (mapped == 'y') mapped = 'z';
            else if (mapped == 'z') mapped = 'y';
        }
        key->keycode = HID_KEY_A + (uint8_t)(mapped - 'a');
        return true;
    }
    if (c >= '1' && c <= '9') {
        if (layout_is_azerty(layout)) key->modifier = USB_HID_MOD_SHIFT;
        key->keycode = HID_KEY_1 + (uint8_t)(c - '1');
        return true;
    }
    if (c == '0') {
        if (layout_is_azerty(layout)) key->modifier = USB_HID_MOD_SHIFT;
        key->keycode = HID_KEY_1 + 9;
        return true;
    }

    if (layout_uses_spanish_punctuation(layout)) {
        switch (c) {
        case ' ': key->keycode = HID_KEY_SPACE; return true;
        case '\t': key->keycode = HID_KEY_TAB; return true;
        case '\n': key->keycode = HID_KEY_ENTER; return true;
        case '-': key->keycode = HID_KEY_SLASH; return true;
        case '_': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_SLASH; return true;
        case '=': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 9; return true;
        case '+': key->keycode = HID_KEY_BRACKET_RIGHT; return true;
        case '[': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_BRACKET_LEFT; return true;
        case '{': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_APOSTROPHE; return true;
        case ']': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_BRACKET_RIGHT; return true;
        case '}': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_BACKSLASH; return true;
        case '\\': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_GRAVE; return true;
        case '|': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_1; return true;
        case ';': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_COMMA; return true;
        case ':': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_PERIOD; return true;
        case '\'': key->keycode = HID_KEY_MINUS; return true;
        case '"': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 1; return true;
        case '`': key->keycode = HID_KEY_BRACKET_LEFT; return true;
        case '~': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_1 + 3; return true;
        case ',': key->keycode = HID_KEY_COMMA; return true;
        case '<': key->keycode = HID_KEY_NON_US_BACKSLASH; return true;
        case '.': key->keycode = HID_KEY_PERIOD; return true;
        case '>': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_NON_US_BACKSLASH; return true;
        case '/': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 6; return true;
        case '?': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_MINUS; return true;
        case '!': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1; return true;
        case '@':
            key->modifier = USB_HID_MOD_RIGHT_ALT;
            key->keycode = layout == USB_HID_LAYOUT_LATAM ? HID_KEY_Q : HID_KEY_1 + 1;
            return true;
        case '#': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_1 + 2; return true;
        case '$': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 3; return true;
        case '%': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 4; return true;
        case '&': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 5; return true;
        case '*': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_BRACKET_RIGHT; return true;
        case '(': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 7; return true;
        case ')': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 8; return true;
        default: return false;
        }
    }

    if (layout == USB_HID_LAYOUT_DE) {
        switch (c) {
        case '"': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 1; return true;
        case '&': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 5; return true;
        case '/': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 6; return true;
        case '(': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 7; return true;
        case ')': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 8; return true;
        case '=': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 9; return true;
        case '?': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_MINUS; return true;
        case ';': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_COMMA; return true;
        case ':': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_PERIOD; return true;
        case '@': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_Q; return true;
        case '[': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_1 + 7; return true;
        case ']': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_1 + 8; return true;
        case '{': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_1 + 6; return true;
        case '}': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_1 + 9; return true;
        case '\\': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_MINUS; return true;
        case '|': key->modifier = USB_HID_MOD_RIGHT_ALT; key->keycode = HID_KEY_NON_US_BACKSLASH; return true;
        }
    }

    switch (c) {
    case ' ': key->keycode = HID_KEY_SPACE; return true;
    case '\t': key->keycode = HID_KEY_TAB; return true;
    case '\n': key->keycode = HID_KEY_ENTER; return true;
    case '-': key->keycode = HID_KEY_MINUS; return true;
    case '_': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_MINUS; return true;
    case '=': key->keycode = HID_KEY_EQUAL; return true;
    case '+': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_EQUAL; return true;
    case '[': key->keycode = HID_KEY_BRACKET_LEFT; return true;
    case '{': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_BRACKET_LEFT; return true;
    case ']': key->keycode = HID_KEY_BRACKET_RIGHT; return true;
    case '}': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_BRACKET_RIGHT; return true;
    case '\\': key->keycode = HID_KEY_BACKSLASH; return true;
    case '|': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_BACKSLASH; return true;
    case ';': key->keycode = HID_KEY_SEMICOLON; return true;
    case ':': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_SEMICOLON; return true;
    case '\'': key->keycode = HID_KEY_APOSTROPHE; return true;
    case '"': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_APOSTROPHE; return true;
    case '`': key->keycode = HID_KEY_GRAVE; return true;
    case '~': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_GRAVE; return true;
    case ',': key->keycode = HID_KEY_COMMA; return true;
    case '<': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_COMMA; return true;
    case '.': key->keycode = HID_KEY_PERIOD; return true;
    case '>': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_PERIOD; return true;
    case '/': key->keycode = HID_KEY_SLASH; return true;
    case '?': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_SLASH; return true;
    case '!': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1; return true;
    case '@': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 1; return true;
    case '#': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 2; return true;
    case '$': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 3; return true;
    case '%': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 4; return true;
    case '^': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 5; return true;
    case '&': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 6; return true;
    case '*': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 7; return true;
    case '(': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 8; return true;
    case ')': key->modifier = USB_HID_MOD_SHIFT; key->keycode = HID_KEY_1 + 9; return true;
    default: return false;
    }
}

static bool key_from_token(const char *token, const char *layout, usb_hid_key_t *key)
{
    memset(key, 0, sizeof(*key));
    if (strlen(token) == 1) return key_from_ascii(token[0], layout, key);

    if (strcasecmp(token, "ENTER") == 0) key->keycode = HID_KEY_ENTER;
    else if (strcasecmp(token, "TAB") == 0) key->keycode = HID_KEY_TAB;
    else if (strcasecmp(token, "ESCAPE") == 0 || strcasecmp(token, "ESC") == 0) key->keycode = HID_KEY_ESCAPE;
    else if (strcasecmp(token, "SPACE") == 0) key->keycode = HID_KEY_SPACE;
    else if (strcasecmp(token, "BACKSPACE") == 0) key->keycode = HID_KEY_BACKSPACE;
    else if (strcasecmp(token, "DELETE") == 0) key->keycode = HID_KEY_DELETE;
    else if (strcasecmp(token, "INSERT") == 0) key->keycode = HID_KEY_INSERT;
    else if (strcasecmp(token, "HOME") == 0) key->keycode = HID_KEY_HOME;
    else if (strcasecmp(token, "END") == 0) key->keycode = HID_KEY_END;
    else if (strcasecmp(token, "PAGEUP") == 0) key->keycode = HID_KEY_PAGE_UP;
    else if (strcasecmp(token, "PAGEDOWN") == 0) key->keycode = HID_KEY_PAGE_DOWN;
    else if (strcasecmp(token, "UPARROW") == 0 ||
             strcasecmp(token, "UP") == 0) key->keycode = HID_KEY_ARROW_UP;
    else if (strcasecmp(token, "DOWNARROW") == 0 ||
             strcasecmp(token, "DOWN") == 0) key->keycode = HID_KEY_ARROW_DOWN;
    else if (strcasecmp(token, "LEFTARROW") == 0 ||
             strcasecmp(token, "LEFT") == 0) key->keycode = HID_KEY_ARROW_LEFT;
    else if (strcasecmp(token, "RIGHTARROW") == 0 ||
             strcasecmp(token, "RIGHT") == 0) key->keycode = HID_KEY_ARROW_RIGHT;
    else if (strcasecmp(token, "PRINTSCREEN") == 0) key->keycode = HID_KEY_PRINT_SCREEN;
    else if (strcasecmp(token, "PAUSE") == 0) key->keycode = HID_KEY_PAUSE;
    else if (strcasecmp(token, "CAPSLOCK") == 0) key->keycode = HID_KEY_CAPS_LOCK;
    else if (strcasecmp(token, "NUMLOCK") == 0) key->keycode = HID_KEY_NUM_LOCK;
    else if (strcasecmp(token, "SCROLLLOCK") == 0) key->keycode = HID_KEY_SCROLL_LOCK;
    else if (strcasecmp(token, "MENU") == 0) key->keycode = HID_KEY_MENU;
    else if ((token[0] == 'F' || token[0] == 'f') && token[1] != '\0') {
        char *end = NULL;
        long f = strtol(token + 1, &end, 10);
        if (*end == '\0' && f >= 1 && f <= 12) key->keycode = HID_KEY_F1 + (uint8_t)(f - 1);
    }

    return key->keycode != HID_KEY_NONE;
}

static bool keepalive_canonical_key(const char *input, char *out, size_t out_size)
{
    if (!input || !out || out_size == 0) return false;

    char normalized[USB_HID_KEEPALIVE_KEY_LEN] = {0};
    size_t pos = 0;
    for (size_t i = 0; input[i] != '\0' && pos + 1 < sizeof(normalized); i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '-' || c == '_' || c == '/' || isspace(c)) continue;
        if (!isalnum(c)) return false;
        normalized[pos++] = (char)toupper(c);
    }
    if (pos == 0) return false;

    if (strcmp(normalized, "SCROLLLOCK") == 0) {
        strlcpy(out, "SCROLLLOCK", out_size);
        return true;
    }
    if (strcmp(normalized, "PAUSE") == 0 ||
        strcmp(normalized, "BREAK") == 0 ||
        strcmp(normalized, "PAUSEBREAK") == 0) {
        strlcpy(out, "PAUSE", out_size);
        return true;
    }
    if (strcmp(normalized, "CAPSLOCK") == 0) {
        strlcpy(out, "CAPSLOCK", out_size);
        return true;
    }
    if (strcmp(normalized, "NUMLOCK") == 0) {
        strlcpy(out, "NUMLOCK", out_size);
        return true;
    }
    if (strcmp(normalized, "PRINTSCREEN") == 0) {
        strlcpy(out, "PRINTSCREEN", out_size);
        return true;
    }
    if (strcmp(normalized, "MENU") == 0) {
        strlcpy(out, "MENU", out_size);
        return true;
    }
    if (normalized[0] == 'F' && normalized[1] != '\0') {
        char *end = NULL;
        long f = strtol(normalized + 1, &end, 10);
        if (*end == '\0' && f >= 1 && f <= 12) {
            strlcpy(out, normalized, out_size);
            return true;
        }
    }

    return false;
}

static bool keepalive_key_from_name(const char *input, usb_hid_key_t *key)
{
    if (!key) return false;
    memset(key, 0, sizeof(*key));

    char canonical[USB_HID_KEEPALIVE_KEY_LEN];
    if (!keepalive_canonical_key(input, canonical, sizeof(canonical))) return false;

    if (strcmp(canonical, "SCROLLLOCK") == 0) key->keycode = HID_KEY_SCROLL_LOCK;
    else if (strcmp(canonical, "PAUSE") == 0) key->keycode = HID_KEY_PAUSE;
    else if (strcmp(canonical, "CAPSLOCK") == 0) key->keycode = HID_KEY_CAPS_LOCK;
    else if (strcmp(canonical, "NUMLOCK") == 0) key->keycode = HID_KEY_NUM_LOCK;
    else return key_from_token(canonical, USB_HID_MACRO_DEFAULT_LAYOUT, key) && key->modifier == 0;

    return key->keycode != HID_KEY_NONE;
}

bool usb_hid_keepalive_key_valid(const char *key)
{
    usb_hid_key_t parsed;
    return keepalive_key_from_name(key, &parsed);
}

esp_err_t usb_hid_executor_configure_keepalive(bool enabled, const char *key, uint16_t interval_s)
{
    if (interval_s < 5 || interval_s > 3600) return ESP_ERR_INVALID_ARG;

    char canonical[USB_HID_KEEPALIVE_KEY_LEN];
    if (!keepalive_canonical_key(key, canonical, sizeof(canonical))) return ESP_ERR_INVALID_ARG;

    if (s_keepalive_lock) xSemaphoreTake(s_keepalive_lock, portMAX_DELAY);
    s_keepalive_enabled = enabled;
    s_keepalive_interval_s = interval_s;
    strlcpy(s_keepalive_key, canonical, sizeof(s_keepalive_key));
    strlcpy(s_keepalive_message, enabled ? "Waiting for interval" : "Disabled",
            sizeof(s_keepalive_message));
    if (s_keepalive_lock) xSemaphoreGive(s_keepalive_lock);
    return ESP_OK;
}

void usb_hid_executor_get_keepalive_status(usb_hid_keepalive_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (s_keepalive_lock) xSemaphoreTake(s_keepalive_lock, portMAX_DELAY);
    status->enabled = s_keepalive_enabled;
    status->interval_s = s_keepalive_interval_s;
    status->sent_count = s_keepalive_sent_count;
    strlcpy(status->key, s_keepalive_key, sizeof(status->key));
    strlcpy(status->message, s_keepalive_message, sizeof(status->message));
    if (s_keepalive_lock) xSemaphoreGive(s_keepalive_lock);

    status->dry_run = s_dry_run;
    status->ready = usb_hid_device_ready();
    usb_hid_exec_status_t exec_status;
    usb_hid_executor_get_status(&exec_status);
    status->paused = s_macro_reserved || s_executing ||
                     exec_status.state == USB_HID_EXEC_RUNNING ||
                     exec_status.state == USB_HID_EXEC_STOPPING;
}

static bool command_supported_for_real(const char *command)
{
    static const char *const supported[] = {
        "REM", "END_REM", "STRING", "STRINGLN", "DELAY",
        "DEFAULT_DELAY", "DEFAULTDELAY", "JITTER",
        "VAR", "DEFINE",
        "IF", "ELSE", "END_IF", "WHILE", "END_WHILE", "BREAK", "CONTINUE",
        "ENTER", "TAB", "ESCAPE", "SPACE", "BACKSPACE", "DELETE", "INSERT",
        "HOME", "END", "PAGEUP", "PAGEDOWN",
        "PRINTSCREEN", "PAUSE", "CAPSLOCK", "NUMLOCK", "SCROLLLOCK", "MENU",
        "UPARROW", "DOWNARROW", "LEFTARROW", "RIGHTARROW",
        "CTRL", "ALT", "SHIFT", "GUI", "WINDOWS", "COMMAND",
        "HOLD", "RELEASE", "STOP_PAYLOAD",
        "LOOP", "FUNCTION", "END_FUNCTION", "RETURN",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    };
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (strcmp(command, supported[i]) == 0) return true;
    }
    char fn_name[32];
    return command_is_function_call(command, fn_name, sizeof(fn_name));
}

static bool modifier_combo_supported(const char *command, const char *args, const char *layout)
{
    uint8_t mod;
    if (!modifier_from_token(command, &mod)) return false;

    char buf[128];
    strlcpy(buf, args, sizeof(buf));
    char *save = NULL;
    char *token = strtok_r(buf, " \t", &save);
    bool saw_key = false;
    while (token) {
        if (saw_key) return false;
        if (!modifier_from_token(token, &mod)) {
            usb_hid_key_t key;
            if (!key_from_token(token, layout, &key)) return false;
            saw_key = true;
        }
        token = strtok_r(NULL, " \t", &save);
    }

    return saw_key;
}

static bool command_args_supported_for_real(const char *command, const char *args, const char *layout)
{
    char fn_name[32];
    if (command_is_function_call(command, fn_name, sizeof(fn_name))) {
        return args == NULL || args[0] == '\0';
    }

    if (strcmp(command, "REM") == 0 ||
        strcmp(command, "END_REM") == 0 ||
        strcmp(command, "DELAY") == 0 ||
        strcmp(command, "VAR") == 0 ||
        strcmp(command, "DEFINE") == 0 ||
        strcmp(command, "IF") == 0 ||
        strcmp(command, "ELSE") == 0 ||
        strcmp(command, "END_IF") == 0 ||
        strcmp(command, "WHILE") == 0 ||
        strcmp(command, "END_WHILE") == 0 ||
        strcmp(command, "BREAK") == 0 ||
        strcmp(command, "CONTINUE") == 0 ||
        strcmp(command, "LOOP") == 0 ||
        strcmp(command, "FUNCTION") == 0 ||
        strcmp(command, "END_FUNCTION") == 0 ||
        strcmp(command, "RETURN") == 0 ||
        strcmp(command, "DEFAULT_DELAY") == 0 ||
        strcmp(command, "DEFAULTDELAY") == 0 ||
        strcmp(command, "JITTER") == 0 ||
        strcmp(command, "STOP_PAYLOAD") == 0) {
        return true;
    }

    if (strcmp(command, "STRING") == 0 || strcmp(command, "STRINGLN") == 0) {
        for (size_t i = 0; args[i] != '\0'; i++) {
            usb_hid_key_t key;
            if (!key_from_ascii(args[i], layout, &key)) return false;
        }
        return true;
    }

    uint8_t mod;
    if (modifier_from_token(command, &mod)) {
        return modifier_combo_supported(command, args, layout);
    }

    if (strcmp(command, "HOLD") == 0) {
        return modifier_from_token(args, &mod);
    }
    if (strcmp(command, "RELEASE") == 0) {
        return modifier_from_token(args, &mod) || strcasecmp(args, "ALL") == 0;
    }

    usb_hid_key_t key;
    return key_from_token(command, layout, &key);
}

static void dry_run_status_for_command(const char *command, const char *args,
                                       char *message, size_t message_size)
{
    if (strcmp(command, "WAIT_FOR_BUTTON_PRESS") == 0) {
        strlcpy(message, "Button wait skipped in dry run", message_size);
    } else if (strcmp(command, "EXFIL") == 0) {
        snprintf(message, message_size, "Local EXFIL skipped: %.56s", args);
    } else if (strcmp(command, "ATTACKMODE") == 0 ||
               strcmp(command, "SAVE_ATTACKMODE") == 0 ||
               strcmp(command, "RESTORE_ATTACKMODE") == 0) {
        strlcpy(message, "USB mode command skipped in dry run", message_size);
    } else if (strcmp(command, "JITTER") == 0 ||
               strcmp(command, "DEFAULT_DELAY") == 0 ||
               strcmp(command, "DEFAULTDELAY") == 0) {
        snprintf(message, message_size, "%s applied (dry run)", command);
    } else if (strcmp(command, "DEFINE") == 0 ||
               strcmp(command, "VAR") == 0 ||
               strcmp(command, "IF") == 0 ||
               strcmp(command, "ELSE") == 0 ||
               strcmp(command, "END_IF") == 0 ||
               strcmp(command, "WHILE") == 0 ||
               strcmp(command, "END_WHILE") == 0 ||
               strcmp(command, "FUNCTION") == 0 ||
               strcmp(command, "END_FUNCTION") == 0 ||
               strcmp(command, "RETURN") == 0 ||
               strcmp(command, "BREAK") == 0 ||
               strcmp(command, "CONTINUE") == 0) {
        strlcpy(message, "Control command checked in dry run", message_size);
    } else {
        strlcpy(message, "Dry run", message_size);
    }
}

static bool preflight_real_script(const char *script, const char *layout, uint16_t *line_out,
                                  char *message, size_t message_size)
{
    const char *cursor = script;
    uint16_t line_no = 1;
    char line_buf[192];

    while (*cursor) {
        size_t len = 0;
        while (cursor[len] && cursor[len] != '\n' && cursor[len] != '\r') len++;
        if (len >= sizeof(line_buf)) {
            *line_out = line_no;
            strlcpy(message, "Line is too long", message_size);
            return false;
        }

        memcpy(line_buf, cursor, len);
        line_buf[len] = '\0';

        char command[32] = {0};
        char *args = NULL;
        if (command_from_line(line_buf, command, sizeof(command), &args)) {
            if (!command_supported_for_real(command)) {
                *line_out = line_no;
                snprintf(message, message_size, "%s requires dry-run/runtime support", command);
                return false;
            }
            if (!command_args_supported_for_real(command, args, layout)) {
                *line_out = line_no;
                snprintf(message, message_size, "%s arguments are not supported for real HID output", command);
                return false;
            }
        }

        cursor += len;
        if (*cursor == '\r') cursor++;
        if (*cursor == '\n') cursor++;
        line_no++;
    }

    return true;
}

static esp_err_t send_key(uint8_t held_mods, const usb_hid_key_t *key)
{
    esp_err_t ret = usb_hid_device_press(held_mods | key->modifier, key->keycode);
    if (ret != ESP_OK) return ret;
    
    vTaskDelay(pdMS_TO_TICKS(USB_HID_EXEC_KEY_PRESS_MS));
    
    /* Mandatory release: try harder to release the key to avoid stuck keys */
    int retries = 3;
    while (retries-- > 0) {
        ret = usb_hid_device_release();
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
    vTaskDelay(pdMS_TO_TICKS(USB_HID_EXEC_KEY_RELEASE_SETTLE_MS));
    return ret;
}

static esp_err_t send_text(uint8_t held_mods, const char *text, const char *layout)
{
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (s_stop_requested) return ESP_ERR_INVALID_STATE;
        usb_hid_key_t key;
        if (!key_from_ascii(text[i], layout, &key)) return ESP_ERR_NOT_SUPPORTED;
        esp_err_t ret = send_key(held_mods, &key);
        if (ret != ESP_OK) return ret;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static void unquote_string_value(char *value)
{
    trim_right(value);
    size_t len = strlen(value);
    if (len >= 2 && (value[0] == '"' || value[0] == '\'') && value[len - 1] == value[0]) {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
    }
}

static bool append_text(char *out, size_t out_size, size_t *pos, const char *text)
{
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (*pos + 1 >= out_size) return false;
        out[(*pos)++] = text[i];
    }
    out[*pos] = '\0';
    return true;
}

static bool expand_runtime_text(const char *src, usb_hid_exec_ctx_t *ctx,
                                char *out, size_t out_size)
{
    size_t pos = 0;
    out[0] = '\0';
    for (size_t i = 0; src[i] != '\0'; i++) {
        if ((src[i] == '$' || src[i] == '#') && src[i + 1] == src[i]) {
            if (pos + 1 >= out_size) return false;
            out[pos++] = src[i++];
            out[pos] = '\0';
            continue;
        }
        if ((src[i] == '$' || src[i] == '#') &&
            (isalpha((unsigned char)src[i + 1]) || src[i + 1] == '_')) {
            char name[USB_HID_EVAL_MAX_NAME];
            size_t n = 0;
            name[n++] = src[i++];
            while ((isalnum((unsigned char)src[i]) || src[i] == '_') && n + 1 < sizeof(name)) {
                name[n++] = src[i++];
            }
            name[n] = '\0';
            i--;

            char value[USB_HID_EVAL_MAX_STRING_VALUE];
            if (name[0] == '$') {
                int internal = 0;
                if (usb_hid_executor_resolve_internal_var(name, &internal)) {
                    snprintf(value, sizeof(value), "%d", internal);
                } else if (!usb_hid_eval_get_var_text(&ctx->eval, name, value, sizeof(value))) {
                    value[0] = '\0';
                }
            } else {
                const char *define_value = usb_hid_eval_get_define(&ctx->eval, name);
                strlcpy(value, define_value ? define_value : "", sizeof(value));
            }
            if (!append_text(out, out_size, &pos, value)) return false;
            continue;
        }
        if (pos + 1 >= out_size) return false;
        out[pos++] = src[i];
        out[pos] = '\0';
    }
    return true;
}

static esp_err_t send_modifier_combo(uint8_t held_mods, const char *command, const char *args, const char *layout)
{
    uint8_t mods = held_mods;
    uint8_t mod;
    if (!modifier_from_token(command, &mod)) return ESP_ERR_NOT_SUPPORTED;
    mods |= mod;

    char buf[128];
    strlcpy(buf, args, sizeof(buf));
    char *save = NULL;
    char *token = strtok_r(buf, " \t", &save);
    while (token) {
        if (modifier_from_token(token, &mod)) {
            mods |= mod;
            token = strtok_r(NULL, " \t", &save);
            continue;
        }

        usb_hid_key_t key;
        if (!key_from_token(token, layout, &key)) return ESP_ERR_NOT_SUPPORTED;
        if (strtok_r(NULL, " \t", &save) != NULL) return ESP_ERR_NOT_SUPPORTED;
        return send_key(mods, &key);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t execute_real_command(const char *command, const char *args,
                                      const char *layout,
                                      uint8_t *held_mods, bool *finish,
                                      usb_hid_exec_ctx_t *ctx,
                                      char *message, size_t message_size)
{
    *finish = false;
    if (strcmp(command, "REM") == 0 || strcmp(command, "END_REM") == 0) {
        strlcpy(message, "Comment skipped", message_size);
        return ESP_OK;
    }
    if (strcmp(command, "DELAY") == 0) {
        /* Body of DELAY is evaluated in the wait loop via compute_command_delay_ms;
         * here we just acknowledge so the status message is meaningful. */
        strlcpy(message, "Delay", message_size);
        return ESP_OK;
    }
    if (strcmp(command, "VAR") == 0) {
        /* "VAR $NAME = expr" -- parse name, expect '=' and evaluate the RHS. */
        char buf[160];
        strlcpy(buf, args, sizeof(buf));
        char *name = buf;
        while (*name && isspace((unsigned char)*name)) name++;
        char *p = name;
        while (*p && !isspace((unsigned char)*p) && *p != '=') p++;
        char sep = *p;
        if (*p) *p++ = '\0';
        if (sep != '=') {
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p != '=') { strlcpy(message, "VAR missing '='", message_size); return ESP_ERR_INVALID_ARG; }
            p++;
        }
        while (*p && isspace((unsigned char)*p)) p++;
        int value = 0;
        if (!usb_hid_eval_expression(&ctx->eval, p, &value)) {
            char string_value[USB_HID_EVAL_MAX_STRING_VALUE];
            strlcpy(string_value, p, sizeof(string_value));
            unquote_string_value(string_value);
            if (!usb_hid_eval_set_string_var(&ctx->eval, name, string_value)) {
                strlcpy(message, "VAR string table full or too long", message_size);
                return ESP_ERR_NO_MEM;
            }
            snprintf(message, message_size, "%.40s = %.40s", name, string_value);
            return ESP_OK;
        }
        if (!usb_hid_eval_set_var(&ctx->eval, name, value)) {
            strlcpy(message, "VAR table full or bad name", message_size);
            return ESP_ERR_NO_MEM;
        }
        snprintf(message, message_size, "%.40s = %d", name, value);
        return ESP_OK;
    }
    if (strcmp(command, "DEFINE") == 0) {
        /* "DEFINE #NAME value..." -- value is stored verbatim, evaluated lazily. */
        char buf[160];
        strlcpy(buf, args, sizeof(buf));
        char *name = buf;
        while (*name && isspace((unsigned char)*name)) name++;
        char *p = name;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') { strlcpy(message, "DEFINE missing value", message_size); return ESP_ERR_INVALID_ARG; }
        if (!usb_hid_eval_add_define(&ctx->eval, name, p)) {
            strlcpy(message, "DEFINE table full or too long", message_size);
            return ESP_ERR_NO_MEM;
        }
        snprintf(message, message_size, "%.40s defined", name);
        return ESP_OK;
    }
    if (strcmp(command, "DEFAULT_DELAY") == 0 || strcmp(command, "DEFAULTDELAY") == 0) {
        long value = (args && args[0]) ? strtol(args, NULL, 10) : 0;
        if (value < 0) value = 0;
        if (value > USB_HID_EXEC_MAX_DELAY_MS) value = USB_HID_EXEC_MAX_DELAY_MS;
        ctx->default_delay_ms = (uint32_t)value;
        snprintf(message, message_size, "Default delay set to %lu ms", (unsigned long)ctx->default_delay_ms);
        return ESP_OK;
    }
    if (strcmp(command, "JITTER") == 0) {
        long value = (args && args[0]) ? strtol(args, NULL, 10) : 0;
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        ctx->jitter_percent = (uint8_t)value;
        snprintf(message, message_size, "Jitter set to %u%%", (unsigned)ctx->jitter_percent);
        return ESP_OK;
    }
    if (strcmp(command, "STOP_PAYLOAD") == 0) {
        *finish = true;
        strlcpy(message, "Stopped by STOP_PAYLOAD", message_size);
        return ESP_OK;
    }

    if (strcmp(command, "STRING") == 0 || strcmp(command, "STRINGLN") == 0) {
        char expanded[256];
        if (!expand_runtime_text(args, ctx, expanded, sizeof(expanded))) {
            strlcpy(message, "Text expansion failed", message_size);
            return ESP_ERR_NO_MEM;
        }
        esp_err_t ret = send_text(*held_mods, expanded, layout);
        if (ret == ESP_OK && strcmp(command, "STRINGLN") == 0) {
            usb_hid_key_t enter = {.keycode = HID_KEY_ENTER};
            vTaskDelay(pdMS_TO_TICKS(10));
            ret = send_key(*held_mods, &enter);
        }
        strlcpy(message, ret == ESP_OK ? "Text sent" : "Text send failed", message_size);
        return ret;
    }

    uint8_t mod;
    if (modifier_from_token(command, &mod)) {
        esp_err_t ret = send_modifier_combo(*held_mods, command, args, layout);
        strlcpy(message, ret == ESP_OK ? "Key combo sent" : "Key combo failed", message_size);
        return ret;
    }

    if (strcmp(command, "HOLD") == 0) {
        if (!modifier_from_token(args, &mod)) return ESP_ERR_NOT_SUPPORTED;
        *held_mods |= mod;
        strlcpy(message, "Modifier held", message_size);
        return ESP_OK;
    }
    if (strcmp(command, "RELEASE") == 0) {
        if (modifier_from_token(args, &mod)) {
            *held_mods &= (uint8_t)~mod;
        } else if (strcasecmp(args, "ALL") == 0) {
            *held_mods = 0;
        } else {
            return ESP_ERR_NOT_SUPPORTED;
        }
        usb_hid_device_release();
        strlcpy(message, "Released", message_size);
        return ESP_OK;
    }

    usb_hid_key_t key;
    if (key_from_token(command, layout, &key)) {
        esp_err_t ret = send_key(*held_mods, &key);
        strlcpy(message, ret == ESP_OK ? "Key sent" : "Key send failed", message_size);
        return ret;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static void run_macro_dry(const usb_hid_macro_t *macro)
{
    usb_hid_parse_result_t parsed = usb_hid_macro_validate(macro->script);
    if (!parsed.ok) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Line %u: %.80s", parsed.line, parsed.message);
        set_status(USB_HID_EXEC_ERROR, macro, parsed.line, count_lines(macro->script), msg);
        return;
    }

    uint16_t total = count_lines(macro->script);
    uint32_t started = xTaskGetTickCount() * portTICK_PERIOD_MS;
    const char *cursor = macro->script;
    uint16_t line_no = 1;
    char line_buf[192];

    while (*cursor) {
        if (s_stop_requested) {
            set_status(USB_HID_EXEC_DONE, macro, line_no, total, "Stopped");
            return;
        }

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - started > USB_HID_EXEC_MAX_RUNTIME_MS) {
            set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "Runtime limit reached");
            return;
        }

        size_t len = 0;
        while (cursor[len] && cursor[len] != '\n' && cursor[len] != '\r') len++;
        if (len >= sizeof(line_buf)) {
            set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "Line is too long");
            return;
        }

        memcpy(line_buf, cursor, len);
        line_buf[len] = '\0';

        char command[32] = {0};
        char *args = NULL;
        char message[96];
        if (command_from_line(line_buf, command, sizeof(command), &args)) {
            if (dry_run_command_finishes_macro(command, message, sizeof(message))) {
                set_status(USB_HID_EXEC_DONE, macro, line_no, total, message);
                return;
            }
            dry_run_status_for_command(command, args, message, sizeof(message));
        } else {
            strlcpy(message, "Dry run", sizeof(message));
        }

        set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, message);

        uint32_t wait_ms = command[0] ? dry_run_wait_for_command(command, line_buf) : USB_HID_EXEC_DEFAULT_STEP_MS;
        uint32_t waited = 0;
        while (waited < wait_ms) {
            if (s_stop_requested) {
                set_status(USB_HID_EXEC_DONE, macro, line_no, total, "Stopped");
                return;
            }
            uint32_t slice = (wait_ms - waited) > 50 ? 50 : (wait_ms - waited);
            vTaskDelay(pdMS_TO_TICKS(slice));
            waited += slice;
        }

        cursor += len;
        if (*cursor == '\r') cursor++;
        if (*cursor == '\n') cursor++;
        line_no++;
    }

    set_status(USB_HID_EXEC_DONE, macro, total, total, "Dry run complete");
}

static void run_macro_real(const usb_hid_macro_t *macro)
{
    usb_hid_parse_result_t parsed = usb_hid_macro_validate(macro->script);
    uint16_t total = count_lines(macro->script);
    if (!parsed.ok) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Line %u: %.80s", parsed.line, parsed.message);
        set_status(USB_HID_EXEC_ERROR, macro, parsed.line, total, msg);
        return;
    }

    uint16_t bad_line = 0;
    char preflight_msg[96];
    const char *layout = usb_hid_macro_layout_valid(macro->layout) ?
                         macro->layout : USB_HID_MACRO_DEFAULT_LAYOUT;
    if (!preflight_real_script(macro->script, layout, &bad_line, preflight_msg, sizeof(preflight_msg))) {
        set_status(USB_HID_EXEC_ERROR, macro, bad_line, total, preflight_msg);
        return;
    }

    if (!usb_hid_device_ready()) {
        set_status(USB_HID_EXEC_ERROR, macro, 0, total, "USB HID host is not ready");
        return;
    }

    size_t max_lines = total ? total : 1;
    usb_hid_line_ref_t *lines = calloc(max_lines, sizeof(*lines));
    if (!lines) {
        set_status(USB_HID_EXEC_ERROR, macro, 0, total, "Line index allocation failed");
        return;
    }
    size_t line_count = build_line_refs(macro->script, lines, max_lines);
    total = (uint16_t)line_count;
    uint16_t *loop_counts = calloc(max_lines, sizeof(*loop_counts));
    if (!loop_counts) {
        free(lines);
        set_status(USB_HID_EXEC_ERROR, macro, 0, total, "Loop counter allocation failed");
        return;
    }

    uint32_t started = xTaskGetTickCount() * portTICK_PERIOD_MS;
    size_t pc = 0;
    size_t call_stack[USB_HID_EXEC_CALL_DEPTH];
    size_t call_depth = 0;
    uint8_t held_mods = 0;
    usb_hid_exec_ctx_t ctx = { .default_delay_ms = 0, .jitter_percent = 0 };
    usb_hid_eval_init(&ctx.eval);
    char line_buf[192];

    while (pc < line_count) {
        uint16_t line_no = (uint16_t)(pc + 1);
        if (s_stop_requested) {
            usb_hid_device_release();
            set_status(USB_HID_EXEC_DONE, macro, line_no, total, "Stopped");
            free(loop_counts);
            free(lines);
            return;
        }

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - started > USB_HID_EXEC_MAX_RUNTIME_MS) {
            usb_hid_device_release();
            set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "Runtime limit reached");
            free(loop_counts);
            free(lines);
            return;
        }

        if (lines[pc].len >= sizeof(line_buf)) {
            usb_hid_device_release();
            set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "Line is too long");
            free(loop_counts);
            free(lines);
            return;
        }

        memcpy(line_buf, macro->script + lines[pc].offset, lines[pc].len);
        line_buf[lines[pc].len] = '\0';

        char command[32] = {0};
        char *args = NULL;
        char message[96] = "USB HID";
        bool finish = false;
        esp_err_t ret = ESP_OK;
        if (command_from_line(line_buf, command, sizeof(command), &args)) {
            if (strcmp(command, "DELAY") == 0) {
                set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "Delay");
                uint32_t wait_ms = compute_delay_arg_ms(args, &ctx.eval);
                TickType_t delay_ticks = pdMS_TO_TICKS(wait_ms);
                if (delay_ticks == 0) delay_ticks = 1;
                TickType_t end_tick = xTaskGetTickCount() + delay_ticks;
                while ((int32_t)(end_tick - xTaskGetTickCount()) > 0) {
                    if (s_stop_requested) {
                        usb_hid_device_release();
                        set_status(USB_HID_EXEC_DONE, macro, line_no, total, "Stopped");
                        free(loop_counts);
                        free(lines);
                        return;
                    }
                    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    if (now - started > USB_HID_EXEC_MAX_RUNTIME_MS) {
                        usb_hid_device_release();
                        set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "Runtime limit reached");
                        free(loop_counts);
                        free(lines);
                        return;
                    }
                    TickType_t remaining = end_tick - xTaskGetTickCount();
                    TickType_t slice = pdMS_TO_TICKS(50);
                    if (slice == 0) slice = 1;
                    if (remaining < slice) slice = remaining;
                    vTaskDelay(slice);
                }
                pc++;
                continue;
            }
            if (strcmp(command, "IF") == 0) {
                char expr[192];
                strlcpy(expr, args, sizeof(expr));
                strip_trailing_then_exec(expr);
                int value = 0;
                if (!usb_hid_eval_expression(&ctx.eval, expr, &value)) {
                    usb_hid_device_release();
                    set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "IF expression failed");
                    free(loop_counts);
                    free(lines);
                    return;
                }
                if (value) {
                    set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "IF true");
                    pc++;
                } else {
                    size_t target = pc;
                    if (!find_if_peer_forward(macro->script, lines, line_count, pc, true, &target)) {
                        usb_hid_device_release();
                        set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "IF target not found");
                        free(loop_counts);
                        free(lines);
                        return;
                    }
                    set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "IF false");
                    pc = target + 1;
                }
                continue;
            }
            if (strcmp(command, "ELSE") == 0) {
                size_t target = pc;
                if (!find_if_peer_forward(macro->script, lines, line_count, pc, false, &target)) {
                    usb_hid_device_release();
                    set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "ELSE target not found");
                    free(loop_counts);
                    free(lines);
                    return;
                }
                set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "ELSE skipped");
                pc = target + 1;
                continue;
            }
            if (strcmp(command, "END_IF") == 0) {
                set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "END_IF");
                pc++;
                continue;
            }
            if (strcmp(command, "WHILE") == 0) {
                int value = 0;
                if (!usb_hid_eval_expression(&ctx.eval, args, &value)) {
                    usb_hid_device_release();
                    set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "WHILE expression failed");
                    free(loop_counts);
                    free(lines);
                    return;
                }
                if (value) {
                    set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "WHILE true");
                    pc++;
                } else {
                    size_t target = pc;
                    if (!find_matching_end_while_forward(macro->script, lines, line_count, pc, &target)) {
                        usb_hid_device_release();
                        set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "WHILE target not found");
                        free(loop_counts);
                        free(lines);
                        return;
                    }
                    set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "WHILE false");
                    pc = target + 1;
                }
                continue;
            }
            if (strcmp(command, "END_WHILE") == 0) {
                size_t target = pc;
                if (!find_matching_while_backward(macro->script, lines, pc, &target)) {
                    usb_hid_device_release();
                    set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "END_WHILE target not found");
                    free(loop_counts);
                    free(lines);
                    return;
                }
                set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "WHILE repeat");
                pc = target;
                continue;
            }
            if (strcmp(command, "BREAK") == 0) {
                size_t target = pc;
                if (!find_matching_end_while_forward(macro->script, lines, line_count, pc, &target)) {
                    usb_hid_device_release();
                    set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "BREAK target not found");
                    free(loop_counts);
                    free(lines);
                    return;
                }
                set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "BREAK");
                pc = target + 1;
                continue;
            }
            if (strcmp(command, "CONTINUE") == 0) {
                size_t target = pc;
                if (!find_matching_while_backward(macro->script, lines, pc, &target)) {
                    usb_hid_device_release();
                    set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "CONTINUE target not found");
                    free(loop_counts);
                    free(lines);
                    return;
                }
                set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "CONTINUE");
                pc = target;
                continue;
            }
            if (strcmp(command, "LOOP") == 0) {
                if (args && args[0] != '\0') {
                    long count = strtol(args, NULL, 10);
                    if (count <= 0) {
                        pc++;
                    } else if (loop_counts[pc] < (uint16_t)count) {
                        loop_counts[pc]++;
                        set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "LOOP repeat");
                        pc = 0;
                    } else {
                        loop_counts[pc] = 0;
                        set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "LOOP done");
                        pc++;
                    }
                } else {
                    set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "LOOP repeat");
                    pc = 0;
                }
                continue;
            }
            if (strcmp(command, "FUNCTION") == 0) {
                size_t target = pc;
                if (!find_end_function_forward(macro->script, lines, line_count, pc, &target)) {
                    usb_hid_device_release();
                    set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "FUNCTION target not found");
                    free(loop_counts);
                    free(lines);
                    return;
                }
                set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "FUNCTION skipped");
                pc = target + 1;
                continue;
            }
            if (strcmp(command, "END_FUNCTION") == 0 || strcmp(command, "RETURN") == 0) {
                if (call_depth == 0) {
                    pc++;
                } else {
                    pc = call_stack[--call_depth];
                }
                set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "FUNCTION return");
                continue;
            }
            char fn_name[32];
            if (command_is_function_call(command, fn_name, sizeof(fn_name))) {
                size_t target = 0;
                if (!find_function_forward(macro->script, lines, line_count, fn_name, &target)) {
                    usb_hid_device_release();
                    set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "FUNCTION not found");
                    free(loop_counts);
                    free(lines);
                    return;
                }
                if (call_depth >= USB_HID_EXEC_CALL_DEPTH) {
                    usb_hid_device_release();
                    set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "FUNCTION call stack full");
                    free(loop_counts);
                    free(lines);
                    return;
                }
                call_stack[call_depth++] = pc + 1;
                set_status(USB_HID_EXEC_RUNNING, macro, line_no, total, "FUNCTION call");
                pc = target + 1;
                continue;
            }
            ret = execute_real_command(command, args, layout, &held_mods, &finish, &ctx, message, sizeof(message));
        }
        if (ret != ESP_OK) {
            usb_hid_device_release();
            snprintf(message, sizeof(message), "Line %u failed: %s", line_no, esp_err_to_name(ret));
            set_status(USB_HID_EXEC_ERROR, macro, line_no, total, message);
            free(loop_counts);
            free(lines);
            return;
        }

        set_status(finish ? USB_HID_EXEC_DONE : USB_HID_EXEC_RUNNING,
                   macro, line_no, total, message);
        if (finish) {
            usb_hid_device_release();
            free(loop_counts);
            free(lines);
            return;
        }

        uint32_t base_wait = command[0] ? compute_command_delay_ms(line_buf, &ctx.eval)
                                        : USB_HID_EXEC_DEFAULT_STEP_MS;
        /* Meta-commands set runtime state; they shouldn't introduce extra waits. */
        bool is_control_meta = (strcmp(command, "DEFAULT_DELAY") == 0 ||
                                strcmp(command, "DEFAULTDELAY") == 0 ||
                                strcmp(command, "JITTER") == 0 ||
                                strcmp(command, "VAR") == 0 ||
                                strcmp(command, "DEFINE") == 0);
        if (!is_control_meta) {
            base_wait += ctx.default_delay_ms;
        }
        uint32_t wait_ms = apply_jitter(base_wait, ctx.jitter_percent);
        if (wait_ms > USB_HID_EXEC_MAX_DELAY_MS) wait_ms = USB_HID_EXEC_MAX_DELAY_MS;
        uint32_t waited = 0;
        while (waited < wait_ms) {
            if (s_stop_requested) {
                usb_hid_device_release();
                set_status(USB_HID_EXEC_DONE, macro, line_no, total, "Stopped");
                free(loop_counts);
                free(lines);
                return;
            }
            uint32_t slice = (wait_ms - waited) > 50 ? 50 : (wait_ms - waited);
            vTaskDelay(pdMS_TO_TICKS(slice));
            waited += slice;
        }

        pc++;
    }

    usb_hid_device_release();
    set_status(USB_HID_EXEC_DONE, macro, total, total, "USB HID complete");
    free(loop_counts);
    free(lines);
}

static void keepalive_set_message(const char *message)
{
    if (!message) return;
    if (s_keepalive_lock) xSemaphoreTake(s_keepalive_lock, portMAX_DELAY);
    strlcpy(s_keepalive_message, message, sizeof(s_keepalive_message));
    if (s_keepalive_lock) xSemaphoreGive(s_keepalive_lock);
}

static void keepalive_get_config(bool *enabled, uint16_t *interval_s,
                                 char *key, size_t key_size)
{
    if (s_keepalive_lock) xSemaphoreTake(s_keepalive_lock, portMAX_DELAY);
    if (enabled) *enabled = s_keepalive_enabled;
    if (interval_s) *interval_s = s_keepalive_interval_s;
    if (key && key_size > 0) strlcpy(key, s_keepalive_key, key_size);
    if (s_keepalive_lock) xSemaphoreGive(s_keepalive_lock);
}

static bool keepalive_is_macro_active(void)
{
    usb_hid_exec_status_t status;
    usb_hid_executor_get_status(&status);
    return s_macro_reserved || s_executing ||
           status.state == USB_HID_EXEC_RUNNING ||
           status.state == USB_HID_EXEC_STOPPING;
}

static void keepalive_task(void *arg)
{
    (void)arg;
    TickType_t next_due = 0;
    uint16_t last_interval_s = 0;
    char last_key[USB_HID_KEEPALIVE_KEY_LEN] = {0};
    while (1) {
        bool enabled = false;
        uint16_t interval_s = 60;
        char key_name[USB_HID_KEEPALIVE_KEY_LEN];
        keepalive_get_config(&enabled, &interval_s, key_name, sizeof(key_name));

        if (!enabled) {
            next_due = 0;
            last_interval_s = 0;
            last_key[0] = '\0';
            keepalive_set_message("Disabled");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        TickType_t interval_ticks = pdMS_TO_TICKS((uint32_t)interval_s * 1000U);
        if (interval_ticks == 0) interval_ticks = pdMS_TO_TICKS(1000);
        if (next_due == 0 || last_interval_s != interval_s || strcmp(last_key, key_name) != 0) {
            next_due = now + interval_ticks;
            last_interval_s = interval_s;
            strlcpy(last_key, key_name, sizeof(last_key));
        }

        int32_t ticks_until_due = (int32_t)(next_due - now);
        if (ticks_until_due > 0) {
            TickType_t sleep_ticks = ticks_until_due > pdMS_TO_TICKS(1000) ?
                                     pdMS_TO_TICKS(1000) : (TickType_t)ticks_until_due;
            vTaskDelay(sleep_ticks);
            continue;
        }
        next_due = now + interval_ticks;

        if (s_dry_run) {
            keepalive_set_message("Dry run: periodic key not sent");
            continue;
        }
        if (keepalive_is_macro_active()) {
            keepalive_set_message("Paused while macro is running");
            continue;
        }
        if (!usb_hid_device_ready()) {
            keepalive_set_message("USB HID host is not ready");
            continue;
        }

        usb_hid_key_t key;
        if (!keepalive_key_from_name(key_name, &key)) {
            keepalive_set_message("Invalid keep-awake key");
            continue;
        }

        if (!s_hid_io_lock || xSemaphoreTake(s_hid_io_lock, 0) != pdTRUE) {
            keepalive_set_message("Paused while HID output is busy");
            continue;
        }
        if (keepalive_is_macro_active()) {
            xSemaphoreGive(s_hid_io_lock);
            keepalive_set_message("Paused while macro is running");
            continue;
        }

        esp_err_t ret = send_key(0, &key);
        xSemaphoreGive(s_hid_io_lock);
        if (ret == ESP_OK) {
            if (s_keepalive_lock) xSemaphoreTake(s_keepalive_lock, portMAX_DELAY);
            s_keepalive_sent_count++;
            snprintf(s_keepalive_message, sizeof(s_keepalive_message),
                     "Sent %s (%lu)", key_name, (unsigned long)s_keepalive_sent_count);
            if (s_keepalive_lock) xSemaphoreGive(s_keepalive_lock);
        } else {
            char message[96];
            snprintf(message, sizeof(message), "Keep-awake send failed: %s", esp_err_to_name(ret));
            keepalive_set_message(message);
        }
    }
}

static void executor_task(void *arg)
{
    (void)arg;
    usb_hid_exec_request_t *req = NULL;
    while (1) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            s_macro_reserved = false;
            s_executing = true;
            if (s_dry_run) {
                run_macro_dry(&req->macro);
            } else {
                if (s_hid_io_lock) xSemaphoreTake(s_hid_io_lock, portMAX_DELAY);
                run_macro_real(&req->macro);
                if (s_hid_io_lock) xSemaphoreGive(s_hid_io_lock);
            }
            s_executing = false;
            free(req);
            req = NULL;
        }
    }
}

esp_err_t usb_hid_executor_init(void)
{
    if (s_queue) return ESP_OK;

    esp_err_t hid_ret = usb_hid_device_init();
    if (hid_ret == ESP_OK) {
        s_dry_run = false;
    } else {
        s_dry_run = true;
        ESP_LOGW(TAG, "USB HID unavailable, keeping dry run: %s", esp_err_to_name(hid_ret));
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "status lock allocation failed");
    s_keepalive_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_keepalive_lock != NULL, ESP_ERR_NO_MEM, TAG, "keepalive lock allocation failed");
    s_hid_io_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_hid_io_lock != NULL, ESP_ERR_NO_MEM, TAG, "HID I/O lock allocation failed");

    s_queue = xQueueCreate(USB_HID_EXEC_QUEUE_LEN, sizeof(usb_hid_exec_request_t *));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "queue allocation failed");

    BaseType_t ok = xTaskCreatePinnedToCore(executor_task, "usb_hid_exec", USB_HID_EXEC_TASK_STACK,
                                            NULL, 20, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task allocation failed");
    ok = xTaskCreatePinnedToCore(keepalive_task, "usb_hid_keepalive", USB_HID_KEEPALIVE_TASK_STACK,
                                 NULL, 10, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "keepalive task allocation failed");

    set_status(USB_HID_EXEC_IDLE, NULL, 0, 0, s_dry_run ? "Idle (dry run)" : "Idle");
    return ESP_OK;
}

esp_err_t usb_hid_executor_start(const usb_hid_macro_t *macro)
{
    if (!macro) return ESP_ERR_INVALID_ARG;

    usb_hid_exec_status_t status;
    usb_hid_executor_get_status(&status);
    if (status.state == USB_HID_EXEC_RUNNING || status.state == USB_HID_EXEC_STOPPING ||
        s_macro_reserved || s_executing) {
        return ESP_ERR_INVALID_STATE;
    }

    usb_hid_exec_request_t *req = calloc(1, sizeof(*req));
    if (!req) return ESP_ERR_NO_MEM;
    req->macro = *macro;
    s_stop_requested = false;
    s_macro_reserved = true;
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        s_macro_reserved = false;
        free(req);
        return ESP_ERR_INVALID_STATE;
    }

    set_status(USB_HID_EXEC_RUNNING, macro, 0, count_lines(macro->script), "Queued");
    return ESP_OK;
}

esp_err_t usb_hid_executor_stop(void)
{
    usb_hid_exec_status_t status;
    usb_hid_executor_get_status(&status);
    if (status.state != USB_HID_EXEC_RUNNING) return ESP_ERR_INVALID_STATE;

    s_stop_requested = true;
    usb_hid_device_release_best_effort(USB_HID_EXEC_PANIC_RELEASE_MS);
    set_status(USB_HID_EXEC_STOPPING, NULL, status.current_line, status.total_lines, "Stopping");
    return ESP_OK;
}

esp_err_t usb_hid_executor_panic_stop(void)
{
    s_stop_requested = true;
    if (s_queue) {
        usb_hid_exec_request_t *queued = NULL;
        while (xQueueReceive(s_queue, &queued, 0) == pdTRUE) {
            free(queued);
            queued = NULL;
        }
        xQueueReset(s_queue);
    }
    s_macro_reserved = false;

    esp_err_t release_ret = usb_hid_device_release_best_effort(USB_HID_EXEC_PANIC_RELEASE_MS);

    usb_hid_exec_status_t status;
    usb_hid_executor_get_status(&status);
    const char *message = release_ret == ESP_OK ? "Emergency stop: keys released" :
                          "Emergency stop: release attempted";
    if ((status.state == USB_HID_EXEC_RUNNING || status.state == USB_HID_EXEC_STOPPING) && s_executing) {
        set_status(USB_HID_EXEC_STOPPING, NULL, status.current_line, status.total_lines, message);
    } else {
        set_status(USB_HID_EXEC_DONE, NULL, status.current_line, status.total_lines, message);
    }
    (void)release_ret;
    return ESP_OK;
}

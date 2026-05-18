#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "usb_hid_executor.h"
#include "usb_hid_device.h"
#include "usb_hid_macro_parser.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define USB_HID_EXEC_QUEUE_LEN 1
#define USB_HID_EXEC_TASK_STACK 4096
#define USB_HID_EXEC_MAX_RUNTIME_MS 60000
#define USB_HID_EXEC_MAX_DELAY_MS 10000
#define USB_HID_EXEC_DEFAULT_STEP_MS 1
#define USB_HID_EXEC_KEY_PRESS_MS 20
#define USB_HID_EXEC_KEY_RELEASE_SETTLE_MS 10
#define USB_HID_EXEC_PANIC_RELEASE_MS 250

static const char *TAG = "usb_hid_exec";

typedef struct {
    usb_hid_macro_t macro;
} usb_hid_exec_request_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static volatile bool s_stop_requested;
static volatile bool s_executing;
static bool s_dry_run = true;
static usb_hid_exec_status_t s_status = {
    .state = USB_HID_EXEC_IDLE,
    .dry_run = true,
    .message = "Idle",
};

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

static bool command_from_line(char *line, char *command, size_t command_size, char **args)
{
    char *trimmed = trim_left(line);
    trim_right(trimmed);
    if (trimmed[0] == '\0') return false;

    size_t i = 0;
    while (trimmed[i] && !isspace((unsigned char)trimmed[i]) && i < command_size - 1) {
        command[i] = (char)toupper((unsigned char)trimmed[i]);
        i++;
    }
    command[i] = '\0';
    *args = trim_left(trimmed + i);
    trim_right(*args);
    return command[0] != '\0';
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
#define HID_KEY_F1            0x3a
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
#define HID_KEY_NON_US_BACKSLASH 0x64

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
    if (strcasecmp(token, "CTRL") == 0) {
        *modifier = USB_HID_MOD_CTRL;
    } else if (strcasecmp(token, "SHIFT") == 0) {
        *modifier = USB_HID_MOD_SHIFT;
    } else if (strcasecmp(token, "ALT") == 0) {
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
    else if (strcasecmp(token, "UPARROW") == 0) key->keycode = HID_KEY_ARROW_UP;
    else if (strcasecmp(token, "DOWNARROW") == 0) key->keycode = HID_KEY_ARROW_DOWN;
    else if (strcasecmp(token, "LEFTARROW") == 0) key->keycode = HID_KEY_ARROW_LEFT;
    else if (strcasecmp(token, "RIGHTARROW") == 0) key->keycode = HID_KEY_ARROW_RIGHT;
    else if ((token[0] == 'F' || token[0] == 'f') && token[1] != '\0') {
        char *end = NULL;
        long f = strtol(token + 1, &end, 10);
        if (*end == '\0' && f >= 1 && f <= 12) key->keycode = HID_KEY_F1 + (uint8_t)(f - 1);
    }

    return key->keycode != HID_KEY_NONE;
}

static bool command_supported_for_real(const char *command)
{
    static const char *const supported[] = {
        "REM", "END_REM", "STRING", "STRINGLN", "DELAY",
        "ENTER", "TAB", "ESCAPE", "SPACE", "BACKSPACE", "DELETE", "INSERT",
        "HOME", "END", "PAGEUP", "PAGEDOWN",
        "UPARROW", "DOWNARROW", "LEFTARROW", "RIGHTARROW",
        "CTRL", "ALT", "SHIFT", "GUI", "WINDOWS", "COMMAND",
        "HOLD", "RELEASE", "STOP_PAYLOAD",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    };
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (strcmp(command, supported[i]) == 0) return true;
    }
    return false;
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
    if (strcmp(command, "REM") == 0 ||
        strcmp(command, "END_REM") == 0 ||
        strcmp(command, "DELAY") == 0 ||
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
               strcmp(command, "DEFINE") == 0 ||
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
    ret = usb_hid_device_release();
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
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    return ESP_OK;
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
                                      char *message, size_t message_size)
{
    *finish = false;
    if (strcmp(command, "REM") == 0 || strcmp(command, "END_REM") == 0) {
        strlcpy(message, "Comment skipped", message_size);
        return ESP_OK;
    }
    if (strcmp(command, "DELAY") == 0) {
        strlcpy(message, "Delay", message_size);
        return ESP_OK;
    }
    if (strcmp(command, "STOP_PAYLOAD") == 0) {
        *finish = true;
        strlcpy(message, "Stopped by STOP_PAYLOAD", message_size);
        return ESP_OK;
    }

    if (strcmp(command, "STRING") == 0 || strcmp(command, "STRINGLN") == 0) {
        esp_err_t ret = send_text(*held_mods, args, layout);
        if (ret == ESP_OK && strcmp(command, "STRINGLN") == 0) {
            usb_hid_key_t enter = {.keycode = HID_KEY_ENTER};
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

    uint32_t started = xTaskGetTickCount() * portTICK_PERIOD_MS;
    const char *cursor = macro->script;
    uint16_t line_no = 1;
    uint8_t held_mods = 0;
    char line_buf[192];

    while (*cursor) {
        if (s_stop_requested) {
            usb_hid_device_release();
            set_status(USB_HID_EXEC_DONE, macro, line_no, total, "Stopped");
            return;
        }

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - started > USB_HID_EXEC_MAX_RUNTIME_MS) {
            usb_hid_device_release();
            set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "Runtime limit reached");
            return;
        }

        size_t len = 0;
        while (cursor[len] && cursor[len] != '\n' && cursor[len] != '\r') len++;
        if (len >= sizeof(line_buf)) {
            usb_hid_device_release();
            set_status(USB_HID_EXEC_ERROR, macro, line_no, total, "Line is too long");
            return;
        }

        memcpy(line_buf, cursor, len);
        line_buf[len] = '\0';

        char command[32] = {0};
        char *args = NULL;
        char message[96] = "USB HID";
        bool finish = false;
        esp_err_t ret = ESP_OK;
        if (command_from_line(line_buf, command, sizeof(command), &args)) {
            ret = execute_real_command(command, args, layout, &held_mods, &finish, message, sizeof(message));
        }
        if (ret != ESP_OK) {
            usb_hid_device_release();
            snprintf(message, sizeof(message), "Line %u failed: %s", line_no, esp_err_to_name(ret));
            set_status(USB_HID_EXEC_ERROR, macro, line_no, total, message);
            return;
        }

        set_status(finish ? USB_HID_EXEC_DONE : USB_HID_EXEC_RUNNING,
                   macro, line_no, total, message);
        if (finish) {
            usb_hid_device_release();
            return;
        }

        uint32_t wait_ms = command[0] ? delay_from_line(line_buf) : USB_HID_EXEC_DEFAULT_STEP_MS;
        uint32_t waited = 0;
        while (waited < wait_ms) {
            if (s_stop_requested) {
                usb_hid_device_release();
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

    usb_hid_device_release();
    set_status(USB_HID_EXEC_DONE, macro, total, total, "USB HID complete");
}

static void executor_task(void *arg)
{
    (void)arg;
    usb_hid_exec_request_t *req = NULL;
    while (1) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            s_executing = true;
            if (s_dry_run) {
                run_macro_dry(&req->macro);
            } else {
                run_macro_real(&req->macro);
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

    s_queue = xQueueCreate(USB_HID_EXEC_QUEUE_LEN, sizeof(usb_hid_exec_request_t *));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "queue allocation failed");

    BaseType_t ok = xTaskCreate(executor_task, "usb_hid_exec", USB_HID_EXEC_TASK_STACK,
                                NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task allocation failed");

    set_status(USB_HID_EXEC_IDLE, NULL, 0, 0, s_dry_run ? "Idle (dry run)" : "Idle");
    return ESP_OK;
}

esp_err_t usb_hid_executor_start(const usb_hid_macro_t *macro)
{
    if (!macro) return ESP_ERR_INVALID_ARG;

    usb_hid_exec_status_t status;
    usb_hid_executor_get_status(&status);
    if (status.state == USB_HID_EXEC_RUNNING || status.state == USB_HID_EXEC_STOPPING) {
        return ESP_ERR_INVALID_STATE;
    }

    usb_hid_exec_request_t *req = calloc(1, sizeof(*req));
    if (!req) return ESP_ERR_NO_MEM;
    req->macro = *macro;
    s_stop_requested = false;
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
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

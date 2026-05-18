#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "usb_hid_macro_parser.h"
#include "usb_hid_macro_store.h"

#define USB_HID_PARSE_MAX_BLOCK_DEPTH 8
#define USB_HID_PARSE_MAX_COUNTED_LOOP 100
#define USB_HID_PARSE_MAX_JITTER_PERCENT 100

typedef enum {
    ARG_NONE,
    ARG_TEXT,
    ARG_NUMBER,
    ARG_ONE_TOKEN,
    ARG_REST_OPTIONAL,
    ARG_EXPRESSION,
    ARG_DEFINE,
    ARG_VAR,
    ARG_FUNCTION,
    ARG_LOOP,
    ARG_ATTACKMODE,
    ARG_LED,
    ARG_INJECT_MOD,
    ARG_EXFIL,
    ARG_RANDOM_INT
} usb_hid_arg_kind_t;

typedef struct {
    const char *name;
    usb_hid_arg_kind_t args;
} usb_hid_command_spec_t;

typedef enum {
    BLOCK_IF,
    BLOCK_WHILE,
    BLOCK_FUNCTION
} usb_hid_block_type_t;

typedef struct {
    usb_hid_block_type_t type;
    uint16_t line;
    bool seen_else;
} usb_hid_block_t;

static const usb_hid_command_spec_t s_commands[] = {
    {"REM", ARG_REST_OPTIONAL},
    {"END_REM", ARG_NONE},
    {"STRING", ARG_TEXT},
    {"STRINGLN", ARG_TEXT},
    {"DELAY", ARG_NUMBER},
    {"ENTER", ARG_NONE},
    {"TAB", ARG_NONE},
    {"ESCAPE", ARG_NONE},
    {"SPACE", ARG_NONE},
    {"BACKSPACE", ARG_NONE},
    {"DELETE", ARG_NONE},
    {"INSERT", ARG_NONE},
    {"HOME", ARG_NONE},
    {"END", ARG_NONE},
    {"PAGEUP", ARG_NONE},
    {"PAGEDOWN", ARG_NONE},
    {"UPARROW", ARG_NONE},
    {"DOWNARROW", ARG_NONE},
    {"LEFTARROW", ARG_NONE},
    {"RIGHTARROW", ARG_NONE},
    {"CTRL", ARG_REST_OPTIONAL},
    {"ALT", ARG_REST_OPTIONAL},
    {"SHIFT", ARG_REST_OPTIONAL},
    {"GUI", ARG_REST_OPTIONAL},
    {"WINDOWS", ARG_REST_OPTIONAL},
    {"COMMAND", ARG_REST_OPTIONAL},
    {"HOLD", ARG_ONE_TOKEN},
    {"RELEASE", ARG_ONE_TOKEN},
    {"ATTACKMODE", ARG_ATTACKMODE},
    {"SAVE_ATTACKMODE", ARG_NONE},
    {"RESTORE_ATTACKMODE", ARG_NONE},
    {"DEFINE", ARG_DEFINE},
    {"VAR", ARG_VAR},
    {"IF", ARG_EXPRESSION},
    {"ELSE", ARG_NONE},
    {"END_IF", ARG_NONE},
    {"WHILE", ARG_EXPRESSION},
    {"END_WHILE", ARG_NONE},
    {"LOOP", ARG_LOOP},
    {"BREAK", ARG_NONE},
    {"CONTINUE", ARG_NONE},
    {"FUNCTION", ARG_FUNCTION},
    {"END_FUNCTION", ARG_NONE},
    {"RETURN", ARG_EXPRESSION},
    {"RANDOM_INT", ARG_RANDOM_INT},
    {"JITTER", ARG_NUMBER},
    {"WAIT_FOR_BUTTON_PRESS", ARG_NONE},
    {"LED", ARG_LED},
    {"CAPSLOCK", ARG_NONE},
    {"NUMLOCK", ARG_NONE},
    {"SCROLLLOCK", ARG_NONE},
    {"EXFIL", ARG_EXFIL},
    {"INJECT_MOD", ARG_INJECT_MOD},
    {"STOP_PAYLOAD", ARG_NONE},
    {"RESTART_PAYLOAD", ARG_NONE},
};

static const char *s_internal_vars[] = {
    "$_CAPSLOCK_ON",
    "$_NUMLOCK_ON",
    "$_CURRENT_VID",
    "$_CURRENT_PID",
    "$_BUTTON_ENABLED",
    "$_HOST_CONFIGURATION_REQUEST_COUNT",
};

static usb_hid_parse_result_t ok_result(void)
{
    usb_hid_parse_result_t result = {.ok = true, .line = 0};
    result.message[0] = '\0';
    return result;
}

static usb_hid_parse_result_t error_result(uint16_t line, const char *message)
{
    usb_hid_parse_result_t result = {.ok = false, .line = line};
    strlcpy(result.message, message, sizeof(result.message));
    return result;
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

static void uppercase(char *value)
{
    for (size_t i = 0; value[i] != '\0'; i++) {
        value[i] = (char)toupper((unsigned char)value[i]);
    }
}

static bool is_number_arg(const char *value)
{
    if (!value || *value == '\0') return false;
    for (size_t i = 0; value[i] != '\0'; i++) {
        if (!isdigit((unsigned char)value[i])) return false;
    }
    return true;
}

static bool is_identifier_body_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static bool is_identifier(const char *value)
{
    if (!value || *value == '\0') return false;
    if (!isalpha((unsigned char)value[0]) && value[0] != '_') return false;
    for (size_t i = 1; value[i] != '\0'; i++) {
        if (!is_identifier_body_char(value[i])) return false;
    }
    return true;
}

static bool is_user_var_name(const char *value)
{
    return value && value[0] == '$' && value[1] != '_' && is_identifier(value + 1);
}

static bool is_internal_var_name(const char *value)
{
    for (size_t i = 0; i < sizeof(s_internal_vars) / sizeof(s_internal_vars[0]); i++) {
        if (strcmp(value, s_internal_vars[i]) == 0) return true;
    }
    return false;
}

static bool is_one_token(const char *value)
{
    if (!value || *value == '\0') return false;
    for (size_t i = 0; value[i] != '\0'; i++) {
        if (isspace((unsigned char)value[i])) return false;
    }
    return true;
}

static bool token_is_one_of(const char *value, const char *const *allowed, size_t allowed_count)
{
    for (size_t i = 0; i < allowed_count; i++) {
        if (strcasecmp(value, allowed[i]) == 0) return true;
    }
    return false;
}

static bool is_function_key(const char *command)
{
    if (command[0] != 'F') return false;
    char *end = NULL;
    long number = strtol(command + 1, &end, 10);
    return end && *end == '\0' && number >= 1 && number <= 12;
}

static const usb_hid_command_spec_t *find_command(const char *command)
{
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        if (strcmp(command, s_commands[i].name) == 0) return &s_commands[i];
    }
    return NULL;
}

static bool expression_balanced(const char *value)
{
    int depth = 0;
    for (size_t i = 0; value[i] != '\0'; i++) {
        if (value[i] == '(') depth++;
        if (value[i] == ')') depth--;
        if (depth < 0) return false;
    }
    return depth == 0;
}

static void strip_trailing_then(char *value)
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

static bool expression_internal_vars_known(const char *value)
{
    for (size_t i = 0; value[i] != '\0'; i++) {
        if (value[i] != '$' || value[i + 1] != '_') continue;
        size_t start = i;
        i += 2;
        while (is_identifier_body_char(value[i])) i++;

        char token[64];
        size_t len = i - start;
        if (len >= sizeof(token)) return false;
        memcpy(token, value + start, len);
        token[len] = '\0';
        if (!is_internal_var_name(token)) return false;
        if (value[i] == '\0') break;
    }
    return true;
}

static bool expression_valid(const char *value)
{
    if (!value || value[0] == '\0') return false;
    return expression_balanced(value) && expression_internal_vars_known(value);
}

static bool parse_number_in_range(const char *value, long min_value, long max_value, long *out)
{
    if (!is_number_arg(value)) return false;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < min_value || parsed > max_value) return false;
    if (out) *out = parsed;
    return true;
}

static usb_hid_parse_result_t validate_define(uint16_t line, const char *args)
{
    char buf[192];
    strlcpy(buf, args, sizeof(buf));
    char *name = trim_left(buf);
    char *cursor = name;
    while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
    if (*cursor == '\0') return error_result(line, "DEFINE requires a name and value");
    *cursor++ = '\0';

    char *value = trim_left(cursor);
    trim_right(value);
    if (name[0] != '#' || !is_identifier(name + 1)) {
        return error_result(line, "DEFINE name must start with # and use identifier characters");
    }
    if (value[0] == '\0') return error_result(line, "DEFINE requires a value");
    return ok_result();
}

static usb_hid_parse_result_t validate_var(uint16_t line, const char *args)
{
    char buf[192];
    strlcpy(buf, args, sizeof(buf));
    char *name = trim_left(buf);
    char *cursor = name;
    while (*cursor && !isspace((unsigned char)*cursor) && *cursor != '=') cursor++;
    char separator = *cursor;
    *cursor++ = '\0';

    if (!is_user_var_name(name)) {
        return error_result(line, "VAR name must start with $ and must not be an internal variable");
    }
    if (separator != '=') {
        cursor = trim_left(cursor);
        if (*cursor != '=') return error_result(line, "VAR requires =");
        cursor++;
    }

    char *expr = trim_left(cursor);
    trim_right(expr);
    if (!expression_valid(expr)) return error_result(line, "VAR requires a valid expression");
    return ok_result();
}

static usb_hid_parse_result_t validate_function(uint16_t line, const char *args)
{
    char buf[96];
    strlcpy(buf, args, sizeof(buf));
    char *value = trim_left(buf);
    trim_right(value);
    size_t len = strlen(value);
    if (len < 3 || value[len - 2] != '(' || value[len - 1] != ')') {
        return error_result(line, "FUNCTION requires NAME()");
    }
    value[len - 2] = '\0';
    if (!is_identifier(value)) return error_result(line, "FUNCTION name must be an identifier");
    return ok_result();
}

static usb_hid_parse_result_t validate_attackmode(uint16_t line, const char *args)
{
    static const char *const allowed[] = {"HID", "STORAGE", "SERIAL", "MSC"};
    char buf[96];
    strlcpy(buf, args, sizeof(buf));
    char *token = strtok(buf, " \t");
    if (!token) return error_result(line, "ATTACKMODE requires at least one mode");
    while (token) {
        if (!token_is_one_of(token, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
            return error_result(line, "ATTACKMODE mode is not supported");
        }
        token = strtok(NULL, " \t");
    }
    return ok_result();
}

static usb_hid_parse_result_t validate_led(uint16_t line, const char *args)
{
    static const char *const allowed[] = {"R", "G", "B", "OFF", "ON"};
    if (!is_one_token(args) || !token_is_one_of(args, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        return error_result(line, "LED requires R, G, B, ON, or OFF");
    }
    return ok_result();
}

static usb_hid_parse_result_t validate_inject_mod(uint16_t line, const char *args)
{
    static const char *const allowed[] = {"CTRL", "ALT", "SHIFT", "GUI", "WINDOWS", "COMMAND"};
    if (!is_one_token(args) || !token_is_one_of(args, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        return error_result(line, "INJECT_MOD requires a supported modifier");
    }
    return ok_result();
}

static usb_hid_parse_result_t validate_exfil(uint16_t line, const char *args)
{
    if (!args || args[0] == '\0') return error_result(line, "EXFIL requires a local path");
    if (strstr(args, "://") || args[0] != '/') {
        return error_result(line, "EXFIL only accepts local absolute paths");
    }
    return ok_result();
}

static usb_hid_parse_result_t validate_random_int(uint16_t line, const char *args)
{
    if (!args || args[0] == '\0' || strcmp(args, "()") == 0) return ok_result();
    size_t len = strlen(args);
    if (len < 2 || args[0] != '(' || args[len - 1] != ')') {
        return error_result(line, "RANDOM_INT requires () or (min,max)");
    }
    return ok_result();
}

static usb_hid_parse_result_t validate_args(uint16_t line, const char *command,
                                            const char *args,
                                            usb_hid_arg_kind_t kind)
{
    bool has_args = args && args[0] != '\0';
    switch (kind) {
    case ARG_NONE:
        if (has_args) return error_result(line, "Command does not accept arguments");
        return ok_result();
    case ARG_TEXT:
        if (!has_args) return error_result(line, "Command requires text");
        return ok_result();
    case ARG_NUMBER:
        if (!is_number_arg(args)) return error_result(line, "Command requires a numeric argument");
        if (strcmp(command, "JITTER") == 0 &&
            !parse_number_in_range(args, 0, USB_HID_PARSE_MAX_JITTER_PERCENT, NULL)) {
            return error_result(line, "JITTER must be between 0 and 100");
        }
        return ok_result();
    case ARG_ONE_TOKEN:
        if (!is_one_token(args)) return error_result(line, "Command requires one argument");
        return ok_result();
    case ARG_REST_OPTIONAL:
        return ok_result();
    case ARG_EXPRESSION:
        if (!has_args && strcmp(command, "RETURN") != 0) return error_result(line, "Command requires an expression");
        if (has_args) {
            char expr[192];
            strlcpy(expr, args, sizeof(expr));
            if (strcmp(command, "IF") == 0) strip_trailing_then(expr);
            if (!expression_valid(expr)) return error_result(line, "Expression is not valid");
        }
        return ok_result();
    case ARG_DEFINE:
        if (!has_args) return error_result(line, "DEFINE requires arguments");
        return validate_define(line, args);
    case ARG_VAR:
        if (!has_args) return error_result(line, "VAR requires arguments");
        return validate_var(line, args);
    case ARG_FUNCTION:
        if (!has_args) return error_result(line, "FUNCTION requires arguments");
        return validate_function(line, args);
    case ARG_LOOP:
        if (!has_args) return ok_result();
        if (!parse_number_in_range(args, 1, USB_HID_PARSE_MAX_COUNTED_LOOP, NULL)) {
            return error_result(line, "LOOP count must be between 1 and 100");
        }
        return ok_result();
    case ARG_ATTACKMODE:
        if (!has_args) return error_result(line, "ATTACKMODE requires arguments");
        return validate_attackmode(line, args);
    case ARG_LED:
        return validate_led(line, args);
    case ARG_INJECT_MOD:
        return validate_inject_mod(line, args);
    case ARG_EXFIL:
        return validate_exfil(line, args);
    case ARG_RANDOM_INT:
        return validate_random_int(line, args);
    }

    (void)command;
    return ok_result();
}

static bool stack_contains(const usb_hid_block_t *stack, size_t depth, usb_hid_block_type_t type)
{
    for (size_t i = 0; i < depth; i++) {
        if (stack[i].type == type) return true;
    }
    return false;
}

static usb_hid_parse_result_t push_block(usb_hid_block_t *stack, size_t *depth,
                                         usb_hid_block_type_t type, uint16_t line)
{
    if (*depth >= USB_HID_PARSE_MAX_BLOCK_DEPTH) {
        return error_result(line, "Block nesting is too deep");
    }
    stack[*depth] = (usb_hid_block_t){.type = type, .line = line, .seen_else = false};
    (*depth)++;
    return ok_result();
}

static usb_hid_parse_result_t pop_block(usb_hid_block_t *stack, size_t *depth,
                                        usb_hid_block_type_t type, uint16_t line,
                                        const char *message)
{
    if (*depth == 0 || stack[*depth - 1].type != type) {
        return error_result(line, message);
    }
    (*depth)--;
    return ok_result();
}

static usb_hid_parse_result_t validate_block_transition(const char *command, uint16_t line,
                                                        usb_hid_block_t *stack, size_t *depth)
{
    if (strcmp(command, "IF") == 0) return push_block(stack, depth, BLOCK_IF, line);
    if (strcmp(command, "WHILE") == 0) return push_block(stack, depth, BLOCK_WHILE, line);
    if (strcmp(command, "FUNCTION") == 0) return push_block(stack, depth, BLOCK_FUNCTION, line);
    if (strcmp(command, "END_IF") == 0) return pop_block(stack, depth, BLOCK_IF, line, "END_IF without matching IF");
    if (strcmp(command, "END_WHILE") == 0) return pop_block(stack, depth, BLOCK_WHILE, line, "END_WHILE without matching WHILE");
    if (strcmp(command, "END_FUNCTION") == 0) return pop_block(stack, depth, BLOCK_FUNCTION, line, "END_FUNCTION without matching FUNCTION");
    if (strcmp(command, "ELSE") == 0) {
        if (*depth == 0 || stack[*depth - 1].type != BLOCK_IF) {
            return error_result(line, "ELSE without matching IF");
        }
        if (stack[*depth - 1].seen_else) return error_result(line, "IF block already has ELSE");
        stack[*depth - 1].seen_else = true;
    }
    if ((strcmp(command, "BREAK") == 0 || strcmp(command, "CONTINUE") == 0) &&
        !stack_contains(stack, *depth, BLOCK_WHILE)) {
        return error_result(line, "BREAK and CONTINUE require a WHILE block");
    }
    if (strcmp(command, "RETURN") == 0 && !stack_contains(stack, *depth, BLOCK_FUNCTION)) {
        return error_result(line, "RETURN requires a FUNCTION block");
    }
    return ok_result();
}

usb_hid_parse_result_t usb_hid_macro_validate(const char *script)
{
    if (!usb_hid_macro_script_valid(script)) {
        return error_result(0, "Macro script is empty or contains invalid characters");
    }

    char line_buf[192];
    usb_hid_block_t block_stack[USB_HID_PARSE_MAX_BLOCK_DEPTH];
    size_t block_depth = 0;
    uint16_t line_no = 1;
    const char *cursor = script;
    while (*cursor) {
        size_t len = 0;
        while (cursor[len] && cursor[len] != '\n' && cursor[len] != '\r') len++;
        if (len >= sizeof(line_buf)) {
            return error_result(line_no, "Line is too long");
        }
        memcpy(line_buf, cursor, len);
        line_buf[len] = '\0';

        char *line = trim_left(line_buf);
        trim_right(line);
        if (line[0] != '\0') {
            char command[32];
            size_t i = 0;
            while (line[i] && !isspace((unsigned char)line[i]) && i < sizeof(command) - 1) {
                command[i] = line[i];
                i++;
            }
            if (line[i] && !isspace((unsigned char)line[i])) {
                return error_result(line_no, "Command is too long");
            }
            command[i] = '\0';
            uppercase(command);

            char *args = trim_left(line + i);
            trim_right(args);

            const usb_hid_command_spec_t *spec = find_command(command);
            usb_hid_command_spec_t function_key_spec = {command, ARG_NONE};
            if (!spec && is_function_key(command)) {
                spec = &function_key_spec;
            }
            if (!spec) {
                return error_result(line_no, "Unknown command");
            }

            usb_hid_parse_result_t result = validate_args(line_no, command, args, spec->args);
            if (!result.ok) return result;
            result = validate_block_transition(command, line_no, block_stack, &block_depth);
            if (!result.ok) return result;
        }

        cursor += len;
        if (*cursor == '\r') cursor++;
        if (*cursor == '\n') cursor++;
        line_no++;
    }

    if (block_depth > 0) {
        switch (block_stack[block_depth - 1].type) {
        case BLOCK_IF:
            return error_result(block_stack[block_depth - 1].line, "IF block is not closed");
        case BLOCK_WHILE:
            return error_result(block_stack[block_depth - 1].line, "WHILE block is not closed");
        case BLOCK_FUNCTION:
            return error_result(block_stack[block_depth - 1].line, "FUNCTION block is not closed");
        }
    }

    return ok_result();
}

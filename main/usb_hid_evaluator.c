#include "usb_hid_evaluator.h"
#include "usb_hid_executor.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_random.h"

/* ------------------------------------------------------------------------- */
/* Context management                                                        */
/* ------------------------------------------------------------------------- */

void usb_hid_eval_init(usb_hid_eval_ctx_t *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
}

bool usb_hid_eval_set_var(usb_hid_eval_ctx_t *ctx, const char *name, int value)
{
    if (!ctx || !name || name[0] == '\0') return false;
    if (strlen(name) >= USB_HID_EVAL_MAX_NAME) return false;

    size_t free_slot = USB_HID_EVAL_MAX_VARS;
    for (size_t i = 0; i < USB_HID_EVAL_MAX_VARS; i++) {
        if (ctx->vars[i].used) {
            if (strcmp(ctx->vars[i].name, name) == 0) {
                ctx->vars[i].value = value;
                ctx->vars[i].string_value[0] = '\0';
                ctx->vars[i].is_string = false;
                return true;
            }
        } else if (free_slot == USB_HID_EVAL_MAX_VARS) {
            free_slot = i;
        }
    }
    if (free_slot >= USB_HID_EVAL_MAX_VARS) return false;
    ctx->vars[free_slot].used = true;
    strlcpy(ctx->vars[free_slot].name, name, USB_HID_EVAL_MAX_NAME);
    ctx->vars[free_slot].value = value;
    ctx->vars[free_slot].string_value[0] = '\0';
    ctx->vars[free_slot].is_string = false;
    return true;
}

bool usb_hid_eval_get_var(const usb_hid_eval_ctx_t *ctx, const char *name, int *out)
{
    if (!ctx || !name || !out) return false;
    for (size_t i = 0; i < USB_HID_EVAL_MAX_VARS; i++) {
        if (ctx->vars[i].used && strcmp(ctx->vars[i].name, name) == 0) {
            if (ctx->vars[i].is_string) return false;
            *out = ctx->vars[i].value;
            return true;
        }
    }
    return false;
}

bool usb_hid_eval_set_string_var(usb_hid_eval_ctx_t *ctx, const char *name, const char *value)
{
    if (!ctx || !name || !value || name[0] == '\0') return false;
    if (strlen(name) >= USB_HID_EVAL_MAX_NAME) return false;
    if (strlen(value) >= USB_HID_EVAL_MAX_STRING_VALUE) return false;

    size_t free_slot = USB_HID_EVAL_MAX_VARS;
    for (size_t i = 0; i < USB_HID_EVAL_MAX_VARS; i++) {
        if (ctx->vars[i].used) {
            if (strcmp(ctx->vars[i].name, name) == 0) {
                strlcpy(ctx->vars[i].string_value, value, sizeof(ctx->vars[i].string_value));
                ctx->vars[i].value = 0;
                ctx->vars[i].is_string = true;
                return true;
            }
        } else if (free_slot == USB_HID_EVAL_MAX_VARS) {
            free_slot = i;
        }
    }
    if (free_slot >= USB_HID_EVAL_MAX_VARS) return false;
    ctx->vars[free_slot].used = true;
    strlcpy(ctx->vars[free_slot].name, name, USB_HID_EVAL_MAX_NAME);
    strlcpy(ctx->vars[free_slot].string_value, value, sizeof(ctx->vars[free_slot].string_value));
    ctx->vars[free_slot].value = 0;
    ctx->vars[free_slot].is_string = true;
    return true;
}

bool usb_hid_eval_get_var_text(const usb_hid_eval_ctx_t *ctx, const char *name,
                               char *out, size_t out_size)
{
    if (!ctx || !name || !out || out_size == 0) return false;
    for (size_t i = 0; i < USB_HID_EVAL_MAX_VARS; i++) {
        if (ctx->vars[i].used && strcmp(ctx->vars[i].name, name) == 0) {
            if (ctx->vars[i].is_string) {
                strlcpy(out, ctx->vars[i].string_value, out_size);
            } else {
                snprintf(out, out_size, "%d", ctx->vars[i].value);
            }
            return true;
        }
    }
    return false;
}

bool usb_hid_eval_add_define(usb_hid_eval_ctx_t *ctx, const char *name, const char *value)
{
    if (!ctx || !name || !value || name[0] == '\0') return false;
    if (strlen(name) >= USB_HID_EVAL_MAX_NAME) return false;
    if (strlen(value) >= USB_HID_EVAL_MAX_DEFINE_VALUE) return false;

    size_t free_slot = USB_HID_EVAL_MAX_DEFINES;
    for (size_t i = 0; i < USB_HID_EVAL_MAX_DEFINES; i++) {
        if (ctx->defines[i].used) {
            if (strcmp(ctx->defines[i].name, name) == 0) {
                strlcpy(ctx->defines[i].value, value, USB_HID_EVAL_MAX_DEFINE_VALUE);
                return true;
            }
        } else if (free_slot == USB_HID_EVAL_MAX_DEFINES) {
            free_slot = i;
        }
    }
    if (free_slot >= USB_HID_EVAL_MAX_DEFINES) return false;
    ctx->defines[free_slot].used = true;
    strlcpy(ctx->defines[free_slot].name, name, USB_HID_EVAL_MAX_NAME);
    strlcpy(ctx->defines[free_slot].value, value, USB_HID_EVAL_MAX_DEFINE_VALUE);
    return true;
}

const char *usb_hid_eval_get_define(const usb_hid_eval_ctx_t *ctx, const char *name)
{
    if (!ctx || !name) return NULL;
    for (size_t i = 0; i < USB_HID_EVAL_MAX_DEFINES; i++) {
        if (ctx->defines[i].used && strcmp(ctx->defines[i].name, name) == 0) {
            return ctx->defines[i].value;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Recursive-descent parser                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *src;
    size_t pos;
    usb_hid_eval_ctx_t *ctx;
    int  define_depth;
    bool error;
} parser_state_t;

static int parse_or(parser_state_t *st);

static void skip_ws(parser_state_t *st)
{
    while (st->src[st->pos] && isspace((unsigned char)st->src[st->pos])) st->pos++;
}

static bool accept_str2(parser_state_t *st, const char *s)
{
    if (st->src[st->pos] == s[0] && st->src[st->pos + 1] == s[1]) {
        st->pos += 2;
        return true;
    }
    return false;
}

static int parse_number(parser_state_t *st)
{
    skip_ws(st);
    /* Support 0x.. hex literals too. */
    if (st->src[st->pos] == '0' &&
        (st->src[st->pos + 1] == 'x' || st->src[st->pos + 1] == 'X')) {
        st->pos += 2;
        int v = 0;
        bool any = false;
        while (isxdigit((unsigned char)st->src[st->pos])) {
            char c = st->src[st->pos++];
            int d = (c >= '0' && c <= '9') ? c - '0' :
                    (c >= 'a' && c <= 'f') ? 10 + c - 'a' :
                                              10 + c - 'A';
            v = (v << 4) | d;
            any = true;
        }
        if (!any) st->error = true;
        return v;
    }

    int v = 0;
    bool any = false;
    while (isdigit((unsigned char)st->src[st->pos])) {
        v = v * 10 + (st->src[st->pos] - '0');
        st->pos++;
        any = true;
    }
    if (!any) st->error = true;
    return v;
}

static void parse_ident_into(parser_state_t *st, char *buf, size_t bufsize)
{
    size_t n = 0;
    while ((isalnum((unsigned char)st->src[st->pos]) || st->src[st->pos] == '_') &&
           n + 1 < bufsize) {
        buf[n++] = st->src[st->pos++];
    }
    buf[n] = '\0';
}

static int parse_random_int_call(parser_state_t *st)
{
    skip_ws(st);
    if (st->src[st->pos] != '(') { st->error = true; return 0; }
    st->pos++;
    int a = parse_or(st);
    skip_ws(st);
    if (st->src[st->pos] != ',') { st->error = true; return 0; }
    st->pos++;
    int b = parse_or(st);
    skip_ws(st);
    if (st->src[st->pos] != ')') { st->error = true; return 0; }
    st->pos++;

    if (st->error) return 0;
    if (a > b) { int t = a; a = b; b = t; }
    uint32_t range = (uint32_t)(b - a + 1);
    if (range == 0) return a;
    return a + (int)(esp_random() % range);
}

static int eval_define_value(parser_state_t *st, const char *value)
{
    if (st->define_depth >= USB_HID_EVAL_MAX_DEPTH) {
        st->error = true;
        return 0;
    }
    parser_state_t sub = {
        .src = value,
        .pos = 0,
        .ctx = st->ctx,
        .define_depth = st->define_depth + 1,
        .error = false,
    };
    int v = parse_or(&sub);
    skip_ws(&sub);
    if (sub.error || sub.src[sub.pos] != '\0') st->error = true;
    return v;
}

static int parse_unary(parser_state_t *st);

static int parse_primary(parser_state_t *st)
{
    skip_ws(st);
    char c = st->src[st->pos];
    if (c == '\0') { st->error = true; return 0; }

    if (c == '(') {
        st->pos++;
        int v = parse_or(st);
        skip_ws(st);
        if (st->src[st->pos] != ')') { st->error = true; return v; }
        st->pos++;
        return v;
    }

    if (isdigit((unsigned char)c)) return parse_number(st);

    if (c == '$') {
        char name[USB_HID_EVAL_MAX_NAME];
        name[0] = '$';
        size_t n = 1;
        st->pos++;
        while ((isalnum((unsigned char)st->src[st->pos]) || st->src[st->pos] == '_') &&
               n + 1 < sizeof(name)) {
            name[n++] = st->src[st->pos++];
        }
        name[n] = '\0';
        if (n == 1) { st->error = true; return 0; }

        int v = 0;
        if (usb_hid_executor_resolve_internal_var(name, &v)) return v;
        if (usb_hid_eval_get_var(st->ctx, name, &v)) return v;
        st->error = true;
        return 0;
    }

    if (c == '#') {
        char name[USB_HID_EVAL_MAX_NAME];
        name[0] = '#';
        size_t n = 1;
        st->pos++;
        while ((isalnum((unsigned char)st->src[st->pos]) || st->src[st->pos] == '_') &&
               n + 1 < sizeof(name)) {
            name[n++] = st->src[st->pos++];
        }
        name[n] = '\0';
        const char *val = usb_hid_eval_get_define(st->ctx, name);
        if (!val) { st->error = true; return 0; }
        return eval_define_value(st, val);
    }

    if (isalpha((unsigned char)c) || c == '_') {
        char ident[24];
        parse_ident_into(st, ident, sizeof(ident));
        if (strcasecmp(ident, "TRUE") == 0) return 1;
        if (strcasecmp(ident, "FALSE") == 0) return 0;
        if (strcasecmp(ident, "RANDOM_INT") == 0) return parse_random_int_call(st);
        st->error = true;
        return 0;
    }

    st->error = true;
    return 0;
}

static int parse_unary(parser_state_t *st)
{
    skip_ws(st);
    char c = st->src[st->pos];
    if (c == '-') { st->pos++; return -parse_unary(st); }
    if (c == '+') { st->pos++; return  parse_unary(st); }
    if (c == '!') { st->pos++; return !parse_unary(st); }
    if (c == '~') { st->pos++; return ~parse_unary(st); }
    return parse_primary(st);
}

static int parse_mul(parser_state_t *st)
{
    int v = parse_unary(st);
    while (!st->error) {
        skip_ws(st);
        char c = st->src[st->pos];
        if (c == '*') { st->pos++; v = v * parse_unary(st); }
        else if (c == '/') {
            st->pos++;
            int r = parse_unary(st);
            v = (r != 0) ? (v / r) : 0;     /* fail-safe: divide-by-zero -> 0 */
        }
        else if (c == '%') {
            st->pos++;
            int r = parse_unary(st);
            v = (r != 0) ? (v % r) : 0;
        }
        else break;
    }
    return v;
}

static int parse_add(parser_state_t *st)
{
    int v = parse_mul(st);
    while (!st->error) {
        skip_ws(st);
        char c = st->src[st->pos];
        if (c == '+') { st->pos++; v = v + parse_mul(st); }
        else if (c == '-') { st->pos++; v = v - parse_mul(st); }
        else break;
    }
    return v;
}

static int parse_band(parser_state_t *st)
{
    int v = parse_add(st);
    while (!st->error) {
        skip_ws(st);
        if (st->src[st->pos] == '&' && st->src[st->pos + 1] != '&') {
            st->pos++;
            v = v & parse_add(st);
        } else break;
    }
    return v;
}

static int parse_bxor(parser_state_t *st)
{
    int v = parse_band(st);
    while (!st->error) {
        skip_ws(st);
        if (st->src[st->pos] == '^') {
            st->pos++;
            v = v ^ parse_band(st);
        } else break;
    }
    return v;
}

static int parse_bor(parser_state_t *st)
{
    int v = parse_bxor(st);
    while (!st->error) {
        skip_ws(st);
        if (st->src[st->pos] == '|' && st->src[st->pos + 1] != '|') {
            st->pos++;
            v = v | parse_bxor(st);
        } else break;
    }
    return v;
}

static int parse_cmp(parser_state_t *st)
{
    int v = parse_bor(st);
    skip_ws(st);
    if (accept_str2(st, "==")) { int r = parse_bor(st); v = (v == r); }
    else if (accept_str2(st, "!=")) { int r = parse_bor(st); v = (v != r); }
    else if (accept_str2(st, "<=")) { int r = parse_bor(st); v = (v <= r); }
    else if (accept_str2(st, ">=")) { int r = parse_bor(st); v = (v >= r); }
    else if (st->src[st->pos] == '<' && st->src[st->pos + 1] != '<') {
        st->pos++;
        int r = parse_bor(st);
        v = (v < r);
    }
    else if (st->src[st->pos] == '>' && st->src[st->pos + 1] != '>') {
        st->pos++;
        int r = parse_bor(st);
        v = (v > r);
    }
    return v;
}

static int parse_and(parser_state_t *st)
{
    int v = parse_cmp(st);
    while (!st->error) {
        skip_ws(st);
        if (accept_str2(st, "&&")) {
            int r = parse_cmp(st);
            v = (v && r) ? 1 : 0;
        } else break;
    }
    return v;
}

static int parse_or(parser_state_t *st)
{
    int v = parse_and(st);
    while (!st->error) {
        skip_ws(st);
        if (accept_str2(st, "||")) {
            int r = parse_and(st);
            v = (v || r) ? 1 : 0;
        } else break;
    }
    return v;
}

bool usb_hid_eval_expression(usb_hid_eval_ctx_t *ctx, const char *expr, int *result)
{
    if (!ctx || !expr || !result) return false;
    parser_state_t st = {
        .src = expr,
        .pos = 0,
        .ctx = ctx,
        .define_depth = 0,
        .error = false,
    };
    int v = parse_or(&st);
    skip_ws(&st);
    if (st.error) return false;
    if (st.src[st.pos] != '\0') return false; /* trailing garbage */
    *result = v;
    return true;
}

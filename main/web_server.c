#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "web_server.h"
#include "wifi_manager.h"
#include "config_storage.h"
#include "tailscale_manager.h"
#include "scheduler_manager.h"
#include "usb_hid_macro_store.h"
#include "usb_hid_macro_parser.h"
#include "usb_hid_executor.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "lwip/inet.h"
#include "mbedtls/base64.h"
#include "log_buffer.h"
#include "cJSON.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;
static repeater_config_t *s_config = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[]   asm("_binary_styles_css_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");
extern const uint8_t usb_hid_js_start[] asm("_binary_usb_hid_js_start");
extern const uint8_t usb_hid_js_end[]   asm("_binary_usb_hid_js_end");

// --- Utility: escape a string for JSON ---
static int json_escape(char *dst, size_t dst_size, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
    return (int)j;
}

// --- Basic Auth helper ---
static bool require_auth(httpd_req_t *req)
{
    char auth_hdr[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"Authentication required\"}");
        return false;
    }

    if (strncmp(auth_hdr, "Basic ", 6) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid auth method\"}");
        return false;
    }

    unsigned char decoded[128];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                               (const unsigned char *)(auth_hdr + 6),
                               strlen(auth_hdr + 6)) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
        return false;
    }
    decoded[decoded_len] = '\0';

    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
        return false;
    }
    *colon = '\0';
    const char *user = (const char *)decoded;
    const char *pass = colon + 1;

    if (strcmp(user, s_config->web_user) != 0 || strcmp(pass, s_config->web_pass) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
        return false;
    }

    return true;
}

// --- Static file handlers ---

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)styles_css_start,
                    styles_css_end - styles_css_start);
    return ESP_OK;
}

static esp_err_t js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)app_js_start,
                    app_js_end - app_js_start);
    return ESP_OK;
}

static esp_err_t usb_hid_js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)usb_hid_js_start,
                    usb_hid_js_end - usb_hid_js_start);
    return ESP_OK;
}

// --- API handlers ---

static esp_err_t api_status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    wifi_status_t status;
    wifi_manager_get_status(&status);

    char ip_str[16];
    esp_ip4_addr_t ip = { .addr = status.sta_ip };
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));

    esp_netif_ip_info_t ap_ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ap_ip_info);
    char ap_ip_str[16];
    snprintf(ap_ip_str, sizeof(ap_ip_str), IPSTR, IP2STR(&ap_ip_info.ip));

    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t uptime_sec = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    usb_hid_macro_storage_stats_t macro_stats;
    memset(&macro_stats, 0, sizeof(macro_stats));
    if (usb_hid_macro_store_stats(&macro_stats) != ESP_OK) {
        macro_stats.max_macros = USB_HID_MACRO_MAX_METADATA;
    }

    char esc_ssid[68];
    json_escape(esc_ssid, sizeof(esc_ssid), status.sta_ssid);

    char sta_mac[18];
    char ap_mac[18];
    snprintf(sta_mac, sizeof(sta_mac), MACSTR, MAC2STR(status.sta_mac));
    snprintf(ap_mac, sizeof(ap_mac), MACSTR, MAC2STR(status.ap_mac));

    char buf[768];
    snprintf(buf, sizeof(buf),
        "{\"sta_connected\":%s,\"sta_ssid\":\"%s\",\"sta_rssi\":%d,"
        "\"sta_ip\":\"%s\",\"ap_clients\":%d,\"ap_ip\":\"%s\","
        "\"sta_mac\":\"%s\",\"ap_mac\":\"%s\","
        "\"sta_retry_count\":%u,\"sta_recovery\":%s,\"sta_paused\":%s,"
        "\"sta_scheduler_enabled\":%s,\"ap_enabled\":%s,"
        "\"sta_next_retry_s\":%u,\"free_heap\":%lu,\"uptime\":%lu,"
        "\"macro_storage_available\":%s,"
        "\"macro_slots_used\":%lu,\"macro_slots_free\":%lu,\"macro_slots_max\":%lu,"
        "\"macro_storage_used\":%lu,\"macro_storage_free\":%lu,\"macro_storage_capacity\":%lu}",
        status.sta_connected ? "true" : "false",
        esc_ssid, status.sta_rssi, ip_str,
        status.ap_client_count, ap_ip_str,
        sta_mac, ap_mac,
        status.sta_retry_count,
        status.sta_recovery ? "true" : "false",
        status.sta_paused ? "true" : "false",
        status.sta_scheduler_enabled ? "true" : "false",
        status.ap_enabled ? "true" : "false",
        status.sta_next_retry_s,
        (unsigned long)free_heap, (unsigned long)uptime_sec,
        macro_stats.available ? "true" : "false",
        (unsigned long)macro_stats.used_macros,
        (unsigned long)(macro_stats.max_macros > macro_stats.used_macros
                        ? macro_stats.max_macros - macro_stats.used_macros : 0),
        (unsigned long)macro_stats.max_macros,
        (unsigned long)macro_stats.used_bytes,
        (unsigned long)macro_stats.free_bytes,
        (unsigned long)macro_stats.total_bytes);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_wifi_state_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    wifi_status_t status;
    wifi_manager_get_status(&status);

    char ip_str[16];
    esp_ip4_addr_t ip = { .addr = status.sta_ip };
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));

    char esc_ssid[68];
    json_escape(esc_ssid, sizeof(esc_ssid), status.sta_ssid);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
        "\"retry_count\":%u,\"recovery\":%s,\"paused\":%s,"
        "\"next_retry_s\":%u}",
        status.sta_connected ? "true" : "false",
        esc_ssid,
        ip_str,
        status.sta_retry_count,
        status.sta_recovery ? "true" : "false",
        status.sta_paused ? "true" : "false",
        status.sta_next_retry_s);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_wifi_pause_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"STA paused\"}");

    /* Pausing STA can drop this HTTP connection when the UI is accessed via
     * upstream IP. Let the response leave before disconnecting. */
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_pause_sta();
    return ESP_OK;
}

static esp_err_t api_wifi_resume_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    esp_err_t ret = wifi_manager_resume_sta();
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi resume failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"STA resumed\"}");
    return ESP_OK;
}

static esp_err_t api_scan_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    wifi_ap_record_t ap_records[WIFI_SCAN_MAX_AP];
    uint16_t ap_count = 0;

    esp_err_t ret = wifi_manager_scan(ap_records, &ap_count);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    // Build JSON array manually. Each entry ~100 bytes, max 20 entries
    char *buf = malloc(ap_count * 120 + 16);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int pos = 0;
    pos += snprintf(buf + pos, 2, "[");
    for (int i = 0; i < ap_count; i++) {
        char esc_ssid[68];
        json_escape(esc_ssid, sizeof(esc_ssid), (const char *)ap_records[i].ssid);
        pos += snprintf(buf + pos, 120,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"auth\":%d}",
            i > 0 ? "," : "",
            esc_ssid, ap_records[i].rssi, ap_records[i].primary, ap_records[i].authmode);
    }
    pos += snprintf(buf + pos, 2, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

static bool json_is_object_like(const char *json);
static bool json_get_string_strict(const char *json, const char *key,
                                   char *out, size_t out_size, bool *present);
static bool json_get_int_strict(const char *json, const char *key, int *out, bool *present);
static bool runtime_string_valid_http(const char *value, size_t max_len);

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char esc_sta_ssid[68], esc_ap_ssid[68], esc_hostname[68];
    json_escape(esc_sta_ssid, sizeof(esc_sta_ssid), s_config->sta_ssid);
    json_escape(esc_ap_ssid, sizeof(esc_ap_ssid), s_config->ap_ssid);
    json_escape(esc_hostname, sizeof(esc_hostname), s_config->hostname);

    char esc_eap_id[130], esc_eap_user[68];
    json_escape(esc_eap_id, sizeof(esc_eap_id), s_config->sta_eap_identity);
    json_escape(esc_eap_user, sizeof(esc_eap_user), s_config->sta_eap_username);

    char sta_mac[18];
    char ap_mac[18];
    if (s_config->sta_mac_custom) {
        snprintf(sta_mac, sizeof(sta_mac), MACSTR, MAC2STR(s_config->sta_mac));
    } else {
        sta_mac[0] = '\0';
    }
    if (s_config->ap_mac_custom) {
        snprintf(ap_mac, sizeof(ap_mac), MACSTR, MAC2STR(s_config->ap_mac));
    } else {
        ap_mac[0] = '\0';
    }

    httpd_resp_set_type(req, "application/json");

    char buf[384];
    int len;

    len = snprintf(buf, sizeof(buf),
        "{\"sta_ssid\":\"%s\",\"sta_pass\":\"\","
        "\"ap_ssid\":\"%s\",\"ap_pass\":\"\","
        "\"ap_channel\":%d,\"ap_max_conn\":%d,\"ap_hide_ssid\":%s,",
        esc_sta_ssid, esc_ap_ssid,
        s_config->ap_channel, s_config->ap_max_conn,
        s_config->ap_hide_ssid ? "true" : "false");
    httpd_resp_send_chunk(req, buf, len);

    len = snprintf(buf, sizeof(buf),
        "\"sta_eap_enabled\":%s,\"sta_eap_identity\":\"%s\","
        "\"sta_eap_username\":\"%s\",\"sta_eap_password\":\"\","
        "\"sta_retry_max\":%u,\"sta_backoff_s\":%u,"
        "\"hostname\":\"%s\",\"sta_mac_custom\":%s,\"sta_mac\":\"%s\","
        "\"ap_mac_custom\":%s,\"ap_mac\":\"%s\",\"port_fwd\":[",
        s_config->sta_eap_enabled ? "true" : "false",
        esc_eap_id, esc_eap_user,
        s_config->sta_retry_max,
        s_config->sta_backoff_s,
        esc_hostname,
        s_config->sta_mac_custom ? "true" : "false",
        sta_mac,
        s_config->ap_mac_custom ? "true" : "false",
        ap_mac);
    httpd_resp_send_chunk(req, buf, len);

    for (int i = 0; i < CFG_PORT_FWD_MAX; i++) {
        esp_ip4_addr_t ip = { .addr = s_config->port_fwd[i].int_ip };
        char entry[128];
        snprintf(entry, sizeof(entry),
            "%s{\"enabled\":%s,\"proto\":%d,\"ext_port\":%d,"
            "\"int_ip\":\"" IPSTR "\",\"int_port\":%d}",
            i > 0 ? "," : "",
            s_config->port_fwd[i].enabled ? "true" : "false",
            s_config->port_fwd[i].proto,
            s_config->port_fwd[i].ext_port,
            IP2STR(&ip),
            s_config->port_fwd[i].int_port);
        httpd_resp_send_chunk(req, entry, strlen(entry));
    }

    esp_ip4_addr_t sip   = { .addr = s_config->sta_static_ip };
    esp_ip4_addr_t sgw   = { .addr = s_config->sta_static_gw };
    esp_ip4_addr_t smask = { .addr = s_config->sta_static_netmask };
    esp_ip4_addr_t sdns1 = { .addr = s_config->sta_static_dns1 };
    esp_ip4_addr_t sdns2 = { .addr = s_config->sta_static_dns2 };
    len = snprintf(buf, sizeof(buf),
        "],\"log_level_global\":%u,\"log_level_microlink\":%u,"
        "\"sta_static_ip_enabled\":%s,"
        "\"sta_static_ip\":\"" IPSTR "\","
        "\"sta_static_gw\":\"" IPSTR "\","
        "\"sta_static_netmask\":\"" IPSTR "\","
        "\"sta_static_dns1\":\"" IPSTR "\","
        "\"sta_static_dns2\":\"" IPSTR "\"}",
        s_config->log_level_global,
        s_config->log_level_microlink,
        s_config->sta_static_ip_enabled ? "true" : "false",
        IP2STR(&sip), IP2STR(&sgw), IP2STR(&smask),
        IP2STR(&sdns1), IP2STR(&sdns2));
    httpd_resp_send_chunk(req, buf, len);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static bool runtime_get_u16_range(httpd_req_t *req, const char *json,
                                  const char *key, uint16_t min, uint16_t max,
                                  uint16_t *out, bool *changed)
{
    int ival;
    bool present = false;
    if (!json_get_int_strict(json, key, &ival, &present)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Invalid %s", key);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return false;
    }
    if (!present) return true;
    if (ival < (int)min || ival > (int)max) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s out of range (%u-%u)", key, min, max);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return false;
    }
    *out = (uint16_t)ival;
    if (changed) *changed = true;
    return true;
}

static bool scheduler_get_u32_range(httpd_req_t *req, const char *json,
                                    const char *key, uint32_t min, uint32_t max,
                                    uint32_t *out, bool *changed)
{
    int ival;
    bool present = false;
    if (!json_get_int_strict(json, key, &ival, &present)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Invalid %s", key);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return false;
    }
    if (!present) return true;
    if (ival < (int)min || (uint32_t)ival > max) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s out of range (%lu-%lu)", key,
                 (unsigned long)min, (unsigned long)max);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return false;
    }
    *out = (uint32_t)ival;
    if (changed) *changed = true;
    return true;
}

static bool runtime_get_u8_range(httpd_req_t *req, const char *json,
                                 const char *key, uint8_t min, uint8_t max,
                                 uint8_t *out, bool *changed)
{
    uint16_t tmp = *out;
    if (!runtime_get_u16_range(req, json, key, min, max, &tmp, changed)) {
        return false;
    }
    *out = (uint8_t)tmp;
    return true;
}

static esp_err_t api_runtime_config_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char ntp1[CFG_NTP_SERVER_LEN * 2];
    char ntp2[CFG_NTP_SERVER_LEN * 2];
    json_escape(ntp1, sizeof(ntp1), s_config->ntp_server_1);
    json_escape(ntp2, sizeof(ntp2), s_config->ntp_server_2);

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"tailscale_boot_delay_s\":%u,"
        "\"wifi_recovery_cooldown_s\":%u,"
        "\"scheduler_poll_interval_s\":%u,"
        "\"ntp_server_1\":\"%s\",\"ntp_server_2\":\"%s\","
        "\"proxy_buf_size\":%u,"
        "\"proxy_socket_timeout_s\":%u,"
        "\"proxy_idle_timeout_s\":%u,"
        "\"proxy_accept_retry_ms\":%u,"
        "\"proxy_listen_backlog\":%u,"
        "\"scan_active_min_ms\":%u,"
        "\"scan_active_max_ms\":%u,"
        "\"scan_timeout_s\":%u,"
        "\"ping_count\":%u,"
        "\"ping_interval_ms\":%u,"
        "\"ping_timeout_ms\":%u,"
        "\"ping_total_wait_s\":%u}",
        s_config->tailscale_boot_delay_s,
        s_config->wifi_recovery_cooldown_s,
        s_config->scheduler_poll_interval_s,
        ntp1, ntp2,
        s_config->proxy_buf_size,
        s_config->proxy_socket_timeout_s,
        s_config->proxy_idle_timeout_s,
        s_config->proxy_accept_retry_ms,
        s_config->proxy_listen_backlog,
        s_config->scan_active_min_ms,
        s_config->scan_active_max_ms,
        s_config->scan_timeout_s,
        s_config->ping_count,
        s_config->ping_interval_ms,
        s_config->ping_timeout_ms,
        s_config->ping_total_wait_s);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_runtime_config_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char buf[1024];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload size");
        return ESP_FAIL;
    }

    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, buf + total, req->content_len - total);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        total += received;
    }
    buf[total] = '\0';

    if (!json_is_object_like(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    repeater_config_t next = *s_config;
    bool changed = false;

    if (!runtime_get_u16_range(req, buf, "tailscale_boot_delay_s",
                               CFG_TS_BOOT_DELAY_MIN_S, CFG_TS_BOOT_DELAY_MAX_S,
                               &next.tailscale_boot_delay_s, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "wifi_recovery_cooldown_s",
                               CFG_WIFI_RECOVERY_COOLDOWN_MIN_S, CFG_WIFI_RECOVERY_COOLDOWN_MAX_S,
                               &next.wifi_recovery_cooldown_s, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "scheduler_poll_interval_s",
                               CFG_SCHED_POLL_INTERVAL_MIN_S, CFG_SCHED_POLL_INTERVAL_MAX_S,
                               &next.scheduler_poll_interval_s, &changed)) return ESP_FAIL;

    bool present = false;
    char sval[CFG_NTP_SERVER_LEN];
    if (!json_get_string_strict(buf, "ntp_server_1", sval, sizeof(sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ntp_server_1");
        return ESP_FAIL;
    }
    if (present) {
        if (!runtime_string_valid_http(sval, sizeof(next.ntp_server_1))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "ntp_server_1 must be 1-63 printable non-space chars");
            return ESP_FAIL;
        }
        strlcpy(next.ntp_server_1, sval, sizeof(next.ntp_server_1));
        changed = true;
    }
    if (!json_get_string_strict(buf, "ntp_server_2", sval, sizeof(sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ntp_server_2");
        return ESP_FAIL;
    }
    if (present) {
        if (!runtime_string_valid_http(sval, sizeof(next.ntp_server_2))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "ntp_server_2 must be 1-63 printable non-space chars");
            return ESP_FAIL;
        }
        strlcpy(next.ntp_server_2, sval, sizeof(next.ntp_server_2));
        changed = true;
    }

    if (!runtime_get_u16_range(req, buf, "proxy_buf_size",
                               CFG_PROXY_BUF_SIZE_MIN, CFG_PROXY_BUF_SIZE_MAX,
                               &next.proxy_buf_size, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "proxy_socket_timeout_s",
                               CFG_PROXY_SOCKET_TIMEOUT_MIN_S, CFG_PROXY_SOCKET_TIMEOUT_MAX_S,
                               &next.proxy_socket_timeout_s, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "proxy_idle_timeout_s",
                               CFG_PROXY_IDLE_TIMEOUT_MIN_S, CFG_PROXY_IDLE_TIMEOUT_MAX_S,
                               &next.proxy_idle_timeout_s, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "proxy_accept_retry_ms",
                               CFG_PROXY_ACCEPT_RETRY_MIN_MS, CFG_PROXY_ACCEPT_RETRY_MAX_MS,
                               &next.proxy_accept_retry_ms, &changed)) return ESP_FAIL;
    if (!runtime_get_u8_range(req, buf, "proxy_listen_backlog",
                              CFG_PROXY_LISTEN_BACKLOG_MIN, CFG_PROXY_LISTEN_BACKLOG_MAX,
                              &next.proxy_listen_backlog, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "scan_active_min_ms",
                               CFG_SCAN_ACTIVE_MIN_MIN_MS, CFG_SCAN_ACTIVE_MIN_MAX_MS,
                               &next.scan_active_min_ms, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "scan_active_max_ms",
                               CFG_SCAN_ACTIVE_MAX_MIN_MS, CFG_SCAN_ACTIVE_MAX_MAX_MS,
                               &next.scan_active_max_ms, &changed)) return ESP_FAIL;
    if (next.scan_active_max_ms < next.scan_active_min_ms) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "scan_active_max_ms must be >= scan_active_min_ms");
        return ESP_FAIL;
    }
    if (!runtime_get_u16_range(req, buf, "scan_timeout_s",
                               CFG_SCAN_TIMEOUT_MIN_S, CFG_SCAN_TIMEOUT_MAX_S,
                               &next.scan_timeout_s, &changed)) return ESP_FAIL;
    if (!runtime_get_u8_range(req, buf, "ping_count",
                              CFG_PING_COUNT_MIN, CFG_PING_COUNT_MAX,
                              &next.ping_count, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "ping_interval_ms",
                               CFG_PING_INTERVAL_MIN_MS, CFG_PING_INTERVAL_MAX_MS,
                               &next.ping_interval_ms, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "ping_timeout_ms",
                               CFG_PING_TIMEOUT_MIN_MS, CFG_PING_TIMEOUT_MAX_MS,
                               &next.ping_timeout_ms, &changed)) return ESP_FAIL;
    if (!runtime_get_u16_range(req, buf, "ping_total_wait_s",
                               CFG_PING_TOTAL_WAIT_MIN_S, CFG_PING_TOTAL_WAIT_MAX_S,
                               &next.ping_total_wait_s, &changed)) return ESP_FAIL;

    if (!changed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No runtime tuning fields");
        return ESP_FAIL;
    }

    *s_config = next;
    esp_err_t ret = config_storage_save(s_config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Runtime config save failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Runtime tuning saved: ts_boot=%us wifi_rec=%us sched_poll=%us proxy_buf=%u",
             s_config->tailscale_boot_delay_s,
             s_config->wifi_recovery_cooldown_s,
             s_config->scheduler_poll_interval_s,
             s_config->proxy_buf_size);

    tailscale_manager_apply_runtime_config(s_config);
    scheduler_manager_apply_config(s_config);
    wifi_manager_apply_port_forwarding();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Runtime tuning saved\",\"requires_reboot\":false}");
    return ESP_OK;
}

// Simple JSON string value extractor (no full parser needed for our small payloads)
static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *start = strstr(json, search);
    if (!start) return false;
    start += strlen(search);
    const char *end = strchr(start, '"');
    if (!end) return false;
    size_t len = end - start;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *start = strstr(json, search);
    if (!start) return false;
    start += strlen(search);
    *out = atoi(start);
    return true;
}

static bool json_is_object_like(const char *json)
{
    while (*json == ' ' || *json == '\t' || *json == '\r' || *json == '\n') json++;
    if (*json != '{') return false;

    const char *end = json + strlen(json);
    while (end > json && (end[-1] == ' ' || end[-1] == '\t' ||
           end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    return end > json && end[-1] == '}';
}

static const char *json_find_value(const char *json, const char *key)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *start = strstr(json, search);
    if (!start) return NULL;

    start += strlen(search);
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (*start != ':') return NULL;
    start++;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    return start;
}

static bool json_get_string_strict(const char *json, const char *key,
                                   char *out, size_t out_size, bool *present)
{
    const char *start = json_find_value(json, key);
    if (present) *present = false;
    if (!start) return true;
    if (present) *present = true;

    if (*start != '"') return false;
    start++;
    const char *end = strchr(start, '"');
    if (!end) return false;

    size_t len = end - start;
    if (len >= out_size) return false;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool json_get_bool_strict(const char *json, const char *key, bool *out, bool *present)
{
    const char *start = json_find_value(json, key);
    if (present) *present = false;
    if (!start) return true;
    if (present) *present = true;

    if (strncmp(start, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    if (*start == '1') {
        *out = true;
        return true;
    }
    if (*start == '0') {
        *out = false;
        return true;
    }
    return false;
}

static bool json_get_int_strict(const char *json, const char *key, int *out, bool *present)
{
    const char *start = json_find_value(json, key);
    if (present) *present = false;
    if (!start) return true;
    if (present) *present = true;

    char *end = NULL;
    long value = strtol(start, &end, 10);
    if (end == start) return false;
    *out = (int)value;
    return true;
}

static bool tailscale_device_name_valid(const char *value)
{
    if (value[0] == '\0') return false;
    for (size_t i = 0; value[i] != '\0'; i++) {
        char c = value[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            continue;
        }
        return false;
    }
    return true;
}

static bool tailscale_control_host_valid(const char *value)
{
    for (size_t i = 0; value[i] != '\0'; i++) {
        char c = value[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':') {
            continue;
        }
        return false;
    }
    return true;
}

static bool hostname_valid(const char *value)
{
    size_t len = strlen(value);
    if (len == 0 || len >= CFG_HOSTNAME_LEN) return false;
    if (value[0] == '-' || value[len - 1] == '-') return false;

    for (size_t i = 0; i < len; i++) {
        char c = value[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-') {
            continue;
        }
        return false;
    }
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool custom_mac_valid(const uint8_t mac[6])
{
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = false;
        if (mac[i] != 0xff) all_ff = false;
    }
    if (all_zero || all_ff) return false;
    if (mac[0] & 0x01) return false;
    return true;
}

static bool parse_custom_mac(const char *value, uint8_t mac[6])
{
    if (strlen(value) != 17) return false;
    for (int i = 0; i < 6; i++) {
        int hi = hex_value(value[i * 3]);
        int lo = hex_value(value[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        mac[i] = (uint8_t)((hi << 4) | lo);
        if (i < 5 && value[i * 3 + 2] != ':') return false;
    }
    return custom_mac_valid(mac);
}

static bool mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static uint32_t cidr_mask_from_prefix(int prefix)
{
    return prefix == 32 ? 0xffffffffU : (0xffffffffU << (32 - prefix));
}

static bool cidr_range_overlaps(uint32_t network, int prefix,
                                uint32_t blocked_network, int blocked_prefix)
{
    uint32_t mask = cidr_mask_from_prefix(prefix);
    uint32_t blocked_mask = cidr_mask_from_prefix(blocked_prefix);
    uint32_t start = network & mask;
    uint32_t end = start | ~mask;
    uint32_t blocked_start = blocked_network & blocked_mask;
    uint32_t blocked_end = blocked_start | ~blocked_mask;

    return start <= blocked_end && blocked_start <= end;
}

static bool parse_ipv4_cidr(const char *value, uint32_t *network_out, int *prefix_out)
{
    if (!value || value[0] == '\0') return false;

    const char *slash = strchr(value, '/');
    if (!slash || strchr(slash + 1, '/')) return false;

    int prefix = 0;
    if (slash[1] == '\0') return false;
    for (const char *p = slash + 1; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return false;
        prefix = prefix * 10 + (*p - '0');
        if (prefix > 32) return false;
    }
    if (prefix < 1 || prefix > 32) return false;

    uint32_t octets[4] = {0};
    const char *p = value;
    for (int i = 0; i < 4; i++) {
        int digits = 0;
        uint32_t octet = 0;
        while (p < slash && *p >= '0' && *p <= '9') {
            octet = (octet * 10) + (uint32_t)(*p - '0');
            if (octet > 255 || ++digits > 3) return false;
            p++;
        }
        if (digits == 0) return false;
        octets[i] = octet;

        if (i < 3) {
            if (p >= slash || *p != '.') return false;
            p++;
        } else if (p != slash) {
            return false;
        }
    }

    uint32_t addr = (octets[0] << 24) | (octets[1] << 16) |
                    (octets[2] << 8) | octets[3];
    uint32_t mask = cidr_mask_from_prefix(prefix);
    uint32_t network = addr & mask;
    if (addr != network) return false;

    if (network_out) *network_out = network;
    if (prefix_out) *prefix_out = prefix;
    return true;
}

static bool tailscale_cidr4_valid(const char *value)
{
    uint32_t network = 0;
    int prefix = 0;
    if (!parse_ipv4_cidr(value, &network, &prefix)) return false;

    if (cidr_range_overlaps(network, prefix, 0x00000000U, 8)) return false;   /* 0.0.0.0/8 */
    if (cidr_range_overlaps(network, prefix, 0x64400000U, 10)) return false;  /* 100.64.0.0/10 */
    if (cidr_range_overlaps(network, prefix, 0x7f000000U, 8)) return false;   /* 127.0.0.0/8 */
    if (cidr_range_overlaps(network, prefix, 0xa9fe0000U, 16)) return false;  /* 169.254.0.0/16 */
    if (cidr_range_overlaps(network, prefix, 0xe0000000U, 4)) return false;   /* 224.0.0.0/4 */

    return true;
}

static bool scheduler_tz_valid_http(const char *value)
{
    size_t len = strlen(value);
    if (len == 0 || len >= CFG_SCHED_TZ_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

static bool runtime_string_valid_http(const char *value, size_t max_len)
{
    size_t len = strlen(value);
    if (len == 0 || len >= max_len) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

static bool scheduler_note_valid(const char *value)
{
    for (size_t i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static bool scheduler_rule_valid_http(const scheduler_rule_t *rule)
{
    if (!rule->enabled) return true;
    if ((rule->days & 0x7f) == 0) return false;
    if (rule->start_min >= 1440 || rule->end_min > 1440) return false;
    if (rule->start_min >= rule->end_min) return false;
    return true;
}

static bool scheduler_macro_id_valid_http(uint32_t macro_id)
{
    if (macro_id == 0) return true;
    usb_hid_macro_t *macro = calloc(1, sizeof(*macro));
    if (!macro) return false;
    esp_err_t ret = usb_hid_macro_store_get(macro_id, macro);
    usb_hid_macro_free(macro);
    free(macro);
    return ret == ESP_OK;
}

static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char buf[2048];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char val[65];
    int ival;
    bool present = false;
    bool bval = false;
    char sval[CFG_HOSTNAME_LEN];
    uint8_t new_sta_retry_max = s_config->sta_retry_max;
    uint16_t new_sta_backoff_s = s_config->sta_backoff_s;
    char new_hostname[CFG_HOSTNAME_LEN];
    uint8_t new_sta_mac[6];
    uint8_t new_ap_mac[6];
    bool new_sta_mac_custom = s_config->sta_mac_custom;
    bool new_ap_mac_custom = s_config->ap_mac_custom;

    strlcpy(new_hostname, s_config->hostname, sizeof(new_hostname));
    memcpy(new_sta_mac, s_config->sta_mac, sizeof(new_sta_mac));
    memcpy(new_ap_mac, s_config->ap_mac, sizeof(new_ap_mac));

    if (!json_get_int_strict(buf, "sta_retry_max", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_retry_max");
        return ESP_FAIL;
    }
    if (present) {
        if (ival < CFG_STA_RETRY_MIN || ival > CFG_STA_RETRY_MAX_LIMIT) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sta_retry_max out of range");
            return ESP_FAIL;
        }
        new_sta_retry_max = (uint8_t)ival;
    }

    if (!json_get_int_strict(buf, "sta_backoff_s", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_backoff_s");
        return ESP_FAIL;
    }
    if (present) {
        if (ival < CFG_STA_BACKOFF_MIN_S || ival > CFG_STA_BACKOFF_MAX_S) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sta_backoff_s out of range");
            return ESP_FAIL;
        }
        new_sta_backoff_s = (uint16_t)ival;
    }

    if (!json_get_string_strict(buf, "hostname", sval, sizeof(sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid hostname");
        return ESP_FAIL;
    }
    if (present) {
        if (!hostname_valid(sval)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "hostname must be 1-31 chars: A-Z, a-z, 0-9 or hyphen");
            return ESP_FAIL;
        }
        strlcpy(new_hostname, sval, sizeof(new_hostname));
    }

    if (!json_get_bool_strict(buf, "sta_mac_custom", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_mac_custom");
        return ESP_FAIL;
    }
    if (present) new_sta_mac_custom = bval;

    char mac_sval[18];
    if (!json_get_string_strict(buf, "sta_mac", mac_sval, sizeof(mac_sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_mac");
        return ESP_FAIL;
    }
    if (present && new_sta_mac_custom && !parse_custom_mac(mac_sval, new_sta_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "STA MAC must be unicast and not all-zero/all-FF");
        return ESP_FAIL;
    }

    if (!json_get_bool_strict(buf, "ap_mac_custom", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ap_mac_custom");
        return ESP_FAIL;
    }
    if (present) new_ap_mac_custom = bval;

    if (!json_get_string_strict(buf, "ap_mac", mac_sval, sizeof(mac_sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ap_mac");
        return ESP_FAIL;
    }
    if (present && new_ap_mac_custom && !parse_custom_mac(mac_sval, new_ap_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP MAC must be unicast and not all-zero/all-FF");
        return ESP_FAIL;
    }

    if (new_sta_mac_custom && !custom_mac_valid(new_sta_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "STA custom MAC is enabled but invalid");
        return ESP_FAIL;
    }
    if (new_ap_mac_custom && !custom_mac_valid(new_ap_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP custom MAC is enabled but invalid");
        return ESP_FAIL;
    }
    if (new_sta_mac_custom && new_ap_mac_custom && mac_equal(new_sta_mac, new_ap_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "STA and AP custom MAC must differ");
        return ESP_FAIL;
    }

    if (json_get_string(buf, "sta_ssid", val, sizeof(val))) {
        strlcpy(s_config->sta_ssid, val, sizeof(s_config->sta_ssid));
    }
    if (json_get_string(buf, "sta_pass", val, sizeof(val)) && strlen(val) > 0) {
        strlcpy(s_config->sta_pass, val, sizeof(s_config->sta_pass));
    }
    if (json_get_string(buf, "ap_ssid", val, sizeof(val)) && strlen(val) > 0) {
        strlcpy(s_config->ap_ssid, val, sizeof(s_config->ap_ssid));
    }
    if (json_get_string(buf, "ap_pass", val, sizeof(val)) && strlen(val) > 0) {
        strlcpy(s_config->ap_pass, val, sizeof(s_config->ap_pass));
    }
    if (json_get_int(buf, "ap_channel", &ival)) {
        s_config->ap_channel = (uint8_t)ival;
    }
    if (json_get_int(buf, "ap_max_conn", &ival)) {
        s_config->ap_max_conn = (uint8_t)ival;
    }
    if (json_get_int(buf, "ap_hide_ssid", &ival)) {
        s_config->ap_hide_ssid = ival ? true : false;
    }

    s_config->sta_retry_max = new_sta_retry_max;
    s_config->sta_backoff_s = new_sta_backoff_s;
    strlcpy(s_config->hostname, new_hostname, sizeof(s_config->hostname));
    memcpy(s_config->sta_mac, new_sta_mac, sizeof(s_config->sta_mac));
    s_config->sta_mac_custom = new_sta_mac_custom;
    memcpy(s_config->ap_mac, new_ap_mac, sizeof(s_config->ap_mac));
    s_config->ap_mac_custom = new_ap_mac_custom;

    // EAP fields
    int eap_en;
    if (json_get_int(buf, "sta_eap_enabled", &eap_en)) {
        s_config->sta_eap_enabled = eap_en ? true : false;
    }
    if (json_get_string(buf, "sta_eap_identity", val, sizeof(val))) {
        strlcpy(s_config->sta_eap_identity, val, sizeof(s_config->sta_eap_identity));
    }
    if (json_get_string(buf, "sta_eap_username", val, sizeof(val))) {
        strlcpy(s_config->sta_eap_username, val, sizeof(s_config->sta_eap_username));
    }
    if (json_get_string(buf, "sta_eap_password", val, sizeof(val)) && strlen(val) > 0) {
        strlcpy(s_config->sta_eap_password, val, sizeof(s_config->sta_eap_password));
    }

    // Port forwarding rules (flat keys: pf0_enabled .. pf4_int_port)
    for (int i = 0; i < CFG_PORT_FWD_MAX; i++) {
        char key[16];
        int v;
        snprintf(key, sizeof(key), "pf%d_enabled", i);
        if (json_get_int(buf, key, &v)) s_config->port_fwd[i].enabled = v ? true : false;
        snprintf(key, sizeof(key), "pf%d_proto", i);
        if (json_get_int(buf, key, &v)) s_config->port_fwd[i].proto = (uint8_t)v;
        snprintf(key, sizeof(key), "pf%d_ext_port", i);
        if (json_get_int(buf, key, &v)) s_config->port_fwd[i].ext_port = (uint16_t)v;
        snprintf(key, sizeof(key), "pf%d_int_port", i);
        if (json_get_int(buf, key, &v)) s_config->port_fwd[i].int_port = (uint16_t)v;
        char ip_str[16];
        snprintf(key, sizeof(key), "pf%d_int_ip", i);
        if (json_get_string(buf, key, ip_str, sizeof(ip_str))) {
            uint32_t parsed = esp_ip4addr_aton(ip_str);
            if (parsed != IPADDR_NONE) {
                s_config->port_fwd[i].int_ip = parsed;
            }
        }
    }

    /* Runtime log levels — accept and clamp to 0..5. */
    if (json_get_int(buf, "log_level_global", &ival)) {
        if (ival < 0) ival = 0;
        if (ival > CFG_LOG_LEVEL_VERBOSE) ival = CFG_LOG_LEVEL_VERBOSE;
        s_config->log_level_global = (uint8_t)ival;
    }
    if (json_get_int(buf, "log_level_microlink", &ival)) {
        if (ival < 0) ival = 0;
        if (ival > CFG_LOG_LEVEL_VERBOSE) ival = CFG_LOG_LEVEL_VERBOSE;
        s_config->log_level_microlink = (uint8_t)ival;
    }

    /* Static IP for STA. Strings empty/missing leave the previous value;
     * "0.0.0.0" clears the field. Validation only when enabled. */
    bool new_sta_static_enabled = s_config->sta_static_ip_enabled;
    uint32_t new_sta_static_ip      = s_config->sta_static_ip;
    uint32_t new_sta_static_gw      = s_config->sta_static_gw;
    uint32_t new_sta_static_netmask = s_config->sta_static_netmask;
    uint32_t new_sta_static_dns1    = s_config->sta_static_dns1;
    uint32_t new_sta_static_dns2    = s_config->sta_static_dns2;

    if (json_get_int(buf, "sta_static_ip_enabled", &ival)) {
        new_sta_static_enabled = ival ? true : false;
    }
    char ip_buf[16];
    if (json_get_string(buf, "sta_static_ip", ip_buf, sizeof(ip_buf))) {
        if (ip_buf[0] == '\0') {
            new_sta_static_ip = 0;
        } else {
            uint32_t parsed = esp_ip4addr_aton(ip_buf);
            if (parsed == IPADDR_NONE) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_static_ip");
                return ESP_FAIL;
            }
            new_sta_static_ip = parsed;
        }
    }
    if (json_get_string(buf, "sta_static_gw", ip_buf, sizeof(ip_buf))) {
        if (ip_buf[0] == '\0') {
            new_sta_static_gw = 0;
        } else {
            uint32_t parsed = esp_ip4addr_aton(ip_buf);
            if (parsed == IPADDR_NONE) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_static_gw");
                return ESP_FAIL;
            }
            new_sta_static_gw = parsed;
        }
    }
    if (json_get_string(buf, "sta_static_netmask", ip_buf, sizeof(ip_buf))) {
        if (ip_buf[0] == '\0') {
            new_sta_static_netmask = 0;
        } else {
            uint32_t parsed = esp_ip4addr_aton(ip_buf);
            if (parsed == IPADDR_NONE) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_static_netmask");
                return ESP_FAIL;
            }
            new_sta_static_netmask = parsed;
        }
    }
    if (json_get_string(buf, "sta_static_dns1", ip_buf, sizeof(ip_buf))) {
        if (ip_buf[0] == '\0') {
            new_sta_static_dns1 = 0;
        } else {
            uint32_t parsed = esp_ip4addr_aton(ip_buf);
            if (parsed == IPADDR_NONE) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_static_dns1");
                return ESP_FAIL;
            }
            new_sta_static_dns1 = parsed;
        }
    }
    if (json_get_string(buf, "sta_static_dns2", ip_buf, sizeof(ip_buf))) {
        if (ip_buf[0] == '\0') {
            new_sta_static_dns2 = 0;
        } else {
            uint32_t parsed = esp_ip4addr_aton(ip_buf);
            if (parsed == IPADDR_NONE) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_static_dns2");
                return ESP_FAIL;
            }
            new_sta_static_dns2 = parsed;
        }
    }
    if (new_sta_static_enabled) {
        if (new_sta_static_ip == 0 || new_sta_static_netmask == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                "Static IP requires sta_static_ip and sta_static_netmask");
            return ESP_FAIL;
        }
    }
    s_config->sta_static_ip_enabled = new_sta_static_enabled;
    s_config->sta_static_ip         = new_sta_static_ip;
    s_config->sta_static_gw         = new_sta_static_gw;
    s_config->sta_static_netmask    = new_sta_static_netmask;
    s_config->sta_static_dns1       = new_sta_static_dns1;
    s_config->sta_static_dns2       = new_sta_static_dns2;

    esp_err_t save_ret = config_storage_save(s_config);
    if (save_ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Config save failed");
        return ESP_FAIL;
    }

    /* Apply log levels immediately, no reboot needed. */
    config_storage_apply_log_levels(s_config);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Config saved. WiFi is restarting...\"}");

    /* Reconfiguring WiFi can drop the HTTP transport, especially when the AP
     * or STA MAC changes. Give the response a short window to leave first. */
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_reconfigure(s_config);
    return ESP_OK;
}

static esp_err_t api_loglevel_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    int ival;
    bool changed = false;
    if (json_get_int(buf, "log_level_global", &ival)) {
        if (ival < 0) ival = 0;
        if (ival > CFG_LOG_LEVEL_VERBOSE) ival = CFG_LOG_LEVEL_VERBOSE;
        s_config->log_level_global = (uint8_t)ival;
        changed = true;
    }
    if (json_get_int(buf, "log_level_microlink", &ival)) {
        if (ival < 0) ival = 0;
        if (ival > CFG_LOG_LEVEL_VERBOSE) ival = CFG_LOG_LEVEL_VERBOSE;
        s_config->log_level_microlink = (uint8_t)ival;
        changed = true;
    }
    if (!changed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No log_level_* field");
        return ESP_FAIL;
    }

    config_storage_save(s_config);
    config_storage_apply_log_levels(s_config);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Log levels applied\"}");
    return ESP_OK;
}

static esp_err_t api_scheduler_status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    scheduler_status_t status;
    scheduler_manager_get_status(&status);

    char esc_tz[CFG_SCHED_TZ_LEN * 2];
    char esc_local[SCHED_TIME_STR_LEN * 2];
    char esc_next[SCHED_TIME_STR_LEN * 2];
    char esc_reason[SCHED_REASON_LEN * 2];
    json_escape(esc_tz, sizeof(esc_tz), status.timezone);
    json_escape(esc_local, sizeof(esc_local), status.local_time);
    json_escape(esc_next, sizeof(esc_next), status.next_change_local);
    json_escape(esc_reason, sizeof(esc_reason), status.reason);

    char buf[1152];
    snprintf(buf, sizeof(buf),
        "{\"time_valid\":%s,\"ntp_synced\":%s,\"timezone\":\"%s\","
        "\"local_time\":\"%s\",\"mode\":%u,\"ap_effective\":%s,"
        "\"ap_desired\":%s,\"sta_effective\":%s,\"sta_desired\":%s,"
        "\"tailscale_effective\":%s,\"tailscale_desired\":%s,"
        "\"safety_hold\":%s,\"dormant_active\":%s,"
        "\"dormant_time_sync_active\":%s,\"dormant_next_retry_s\":%lu,"
        "\"dormant_ap_recovery_active\":%s,"
        "\"dormant_ap_recovery_remaining_s\":%lu,"
        "\"dormant_reason\":\"%s\",\"active_rule\":%d,"
        "\"next_rule\":%d,\"next_change_s\":%lu,"
        "\"next_change_local\":\"%s\",\"reason\":\"%s\"}",
        status.time_valid ? "true" : "false",
        status.ntp_synced ? "true" : "false",
        esc_tz, esc_local, (unsigned)status.mode,
        status.ap_effective ? "true" : "false",
        status.ap_desired ? "true" : "false",
        status.sta_effective ? "true" : "false",
        status.sta_desired ? "true" : "false",
        status.tailscale_effective ? "true" : "false",
        status.tailscale_desired ? "true" : "false",
        status.safety_hold ? "true" : "false",
        status.dormant_active ? "true" : "false",
        status.dormant_time_sync_active ? "true" : "false",
        (unsigned long)status.dormant_next_retry_s,
        status.dormant_ap_recovery_active ? "true" : "false",
        (unsigned long)status.dormant_ap_recovery_remaining_s,
        status.dormant_reason,
        status.active_rule, status.next_rule,
        (unsigned long)status.next_change_s,
        esc_next, esc_reason);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_scheduler_config_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char esc_tz[CFG_SCHED_TZ_LEN * 2];
    json_escape(esc_tz, sizeof(esc_tz), s_config->sched_tz);

    httpd_resp_set_type(req, "application/json");
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"timezone\":\"%s\",\"mode\":%u,"
        "\"dormant_time_sync_attempt_s\":%u,"
        "\"dormant_time_sync_retry_s\":%lu,"
        "\"dormant_ap_recovery_enabled\":%s,"
        "\"dormant_ap_recovery_window_s\":%u,"
        "\"dormant_button_wake_enabled\":%s,"
        "\"dormant_button_wake_s\":%u,"
        "\"dormant_keep_ap_off_during_active_window\":%s,"
        "\"rules\":[",
        esc_tz, (unsigned)s_config->sched_mode,
        s_config->dormant_time_sync_attempt_s,
        (unsigned long)s_config->dormant_time_sync_retry_s,
        s_config->dormant_ap_recovery_enabled ? "true" : "false",
        s_config->dormant_ap_recovery_window_s,
        s_config->dormant_button_wake_enabled ? "true" : "false",
        s_config->dormant_button_wake_s,
        s_config->dormant_keep_ap_off_during_active_window ? "true" : "false");
    httpd_resp_send_chunk(req, buf, len);

    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        const scheduler_rule_t *rule = &s_config->sched_rules[i];
        char esc_note[CFG_SCHED_NOTE_LEN * 2];
        json_escape(esc_note, sizeof(esc_note), rule->note);
        len = snprintf(buf, sizeof(buf),
            "%s{\"enabled\":%s,\"days\":%u,\"start_min\":%u,"
            "\"end_min\":%u,\"ap_enabled\":%s,\"sta_enabled\":%s,"
            "\"tailscale_enabled\":%s,\"usb_hid_macro_id\":%lu,\"note\":\"%s\"}",
            i ? "," : "",
            rule->enabled ? "true" : "false",
            rule->days,
            rule->start_min,
            rule->end_min,
            rule->ap_enabled ? "true" : "false",
            rule->sta_enabled ? "true" : "false",
            rule->tailscale_enabled ? "true" : "false",
            (unsigned long)rule->usb_hid_macro_id,
            esc_note);
        httpd_resp_send_chunk(req, buf, len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t api_scheduler_config_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char buf[4096];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char sval[CFG_SCHED_TZ_LEN];
    bool present = false;
    bool bval = false;
    int ival = 0;
    scheduler_mode_t new_mode = s_config->sched_mode;
    char new_tz[CFG_SCHED_TZ_LEN];
    scheduler_rule_t new_rules[CFG_SCHED_RULES_MAX];
    uint16_t new_dormant_time_sync_attempt_s = s_config->dormant_time_sync_attempt_s;
    uint32_t new_dormant_time_sync_retry_s = s_config->dormant_time_sync_retry_s;
    bool new_dormant_ap_recovery_enabled = s_config->dormant_ap_recovery_enabled;
    uint16_t new_dormant_ap_recovery_window_s = s_config->dormant_ap_recovery_window_s;
    bool new_dormant_button_wake_enabled = s_config->dormant_button_wake_enabled;
    uint16_t new_dormant_button_wake_s = s_config->dormant_button_wake_s;
    bool new_dormant_keep_ap_off_during_active_window =
        s_config->dormant_keep_ap_off_during_active_window;
    strlcpy(new_tz, s_config->sched_tz, sizeof(new_tz));
    memcpy(new_rules, s_config->sched_rules, sizeof(new_rules));

    if (!json_get_string_strict(buf, "timezone", sval, sizeof(sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid timezone");
        return ESP_FAIL;
    }
    if (present) {
        if (!scheduler_tz_valid_http(sval)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "timezone must be printable POSIX TZ");
            return ESP_FAIL;
        }
        strlcpy(new_tz, sval, sizeof(new_tz));
    }

    if (!json_get_int_strict(buf, "mode", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode");
        return ESP_FAIL;
    }
    if (present) {
        if (ival < SCHED_MODE_ALWAYS_ON || ival > SCHED_MODE_DORMANT_SCHEDULED) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode out of range");
            return ESP_FAIL;
        }
        new_mode = (scheduler_mode_t)ival;
    }

    bool changed = false;
    if (!runtime_get_u16_range(req, buf, "dormant_time_sync_attempt_s",
                               CFG_DORMANT_TIME_SYNC_ATTEMPT_MIN_S,
                               CFG_DORMANT_TIME_SYNC_ATTEMPT_MAX_S,
                               &new_dormant_time_sync_attempt_s,
                               &changed)) return ESP_FAIL;
    if (!scheduler_get_u32_range(req, buf, "dormant_time_sync_retry_s",
                                 CFG_DORMANT_TIME_SYNC_RETRY_MIN_S,
                                 CFG_DORMANT_TIME_SYNC_RETRY_MAX_S,
                                 &new_dormant_time_sync_retry_s,
                                 &changed)) return ESP_FAIL;
    if (!json_get_bool_strict(buf, "dormant_ap_recovery_enabled", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid dormant_ap_recovery_enabled");
        return ESP_FAIL;
    }
    if (present) new_dormant_ap_recovery_enabled = bval;
    if (!runtime_get_u16_range(req, buf, "dormant_ap_recovery_window_s",
                               CFG_DORMANT_AP_RECOVERY_WINDOW_MIN_S,
                               CFG_DORMANT_AP_RECOVERY_WINDOW_MAX_S,
                               &new_dormant_ap_recovery_window_s,
                               &changed)) return ESP_FAIL;
    if (!json_get_bool_strict(buf, "dormant_button_wake_enabled", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid dormant_button_wake_enabled");
        return ESP_FAIL;
    }
    if (present) new_dormant_button_wake_enabled = bval;
    if (!runtime_get_u16_range(req, buf, "dormant_button_wake_s",
                               CFG_DORMANT_BUTTON_WAKE_MIN_S,
                               CFG_DORMANT_BUTTON_WAKE_MAX_S,
                               &new_dormant_button_wake_s,
                               &changed)) return ESP_FAIL;
    if (!json_get_bool_strict(buf, "dormant_keep_ap_off_during_active_window",
                              &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Invalid dormant_keep_ap_off_during_active_window");
        return ESP_FAIL;
    }
    if (present) new_dormant_keep_ap_off_during_active_window = bval;

    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        char key[24];
        scheduler_rule_t *rule = &new_rules[i];

        snprintf(key, sizeof(key), "rule%d_enabled", i);
        if (!json_get_bool_strict(buf, key, &bval, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule enabled");
            return ESP_FAIL;
        }
        if (present) rule->enabled = bval;

        snprintf(key, sizeof(key), "rule%d_days", i);
        if (!json_get_int_strict(buf, key, &ival, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule days");
            return ESP_FAIL;
        }
        if (present) {
            if (ival < 0 || ival > 0x7f) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rule days out of range");
                return ESP_FAIL;
            }
            rule->days = (uint8_t)ival;
        }

        snprintf(key, sizeof(key), "rule%d_start_min", i);
        if (!json_get_int_strict(buf, key, &ival, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule start");
            return ESP_FAIL;
        }
        if (present) {
            if (ival < 0 || ival >= 1440) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rule start out of range");
                return ESP_FAIL;
            }
            rule->start_min = (uint16_t)ival;
        }

        snprintf(key, sizeof(key), "rule%d_end_min", i);
        if (!json_get_int_strict(buf, key, &ival, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule end");
            return ESP_FAIL;
        }
        if (present) {
            if (ival < 0 || ival > 1440) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rule end out of range");
                return ESP_FAIL;
            }
            rule->end_min = (uint16_t)ival;
        }

        snprintf(key, sizeof(key), "rule%d_ap_enabled", i);
        if (!json_get_bool_strict(buf, key, &bval, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule AP state");
            return ESP_FAIL;
        }
        if (present) rule->ap_enabled = bval;

        snprintf(key, sizeof(key), "rule%d_sta_enabled", i);
        if (!json_get_bool_strict(buf, key, &bval, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule STA state");
            return ESP_FAIL;
        }
        if (present) rule->sta_enabled = bval;

        snprintf(key, sizeof(key), "rule%d_tailscale_enabled", i);
        if (!json_get_bool_strict(buf, key, &bval, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule Tailscale state");
            return ESP_FAIL;
        }
        if (present) rule->tailscale_enabled = bval;

        snprintf(key, sizeof(key), "rule%d_usb_hid_macro_id", i);
        if (!json_get_int_strict(buf, key, &ival, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule USB HID macro");
            return ESP_FAIL;
        }
        if (present) {
            if (ival < 0) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rule USB HID macro out of range");
                return ESP_FAIL;
            }
            rule->usb_hid_macro_id = (uint32_t)ival;
            if (!scheduler_macro_id_valid_http(rule->usb_hid_macro_id)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "scheduled USB HID macro not found");
                return ESP_FAIL;
            }
        }

        char note[CFG_SCHED_NOTE_LEN];
        snprintf(key, sizeof(key), "rule%d_note", i);
        if (!json_get_string_strict(buf, key, note, sizeof(note), &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule note");
            return ESP_FAIL;
        }
        if (present) {
            if (!scheduler_note_valid(note)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rule note must be printable ASCII");
                return ESP_FAIL;
            }
            strlcpy(rule->note, note, sizeof(rule->note));
        }

        if (!scheduler_rule_valid_http(rule)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled rules need days and start < end");
            return ESP_FAIL;
        }
    }

    if (new_mode == SCHED_MODE_MANUAL_OFF) {
        wifi_status_t wifi = {0};
        wifi_manager_get_status(&wifi);
        if (!wifi.sta_connected) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Manual AP off requires active STA management path");
            return ESP_FAIL;
        }
    }
    if (new_mode == SCHED_MODE_DORMANT_SCHEDULED &&
        !new_dormant_button_wake_enabled &&
        !new_dormant_ap_recovery_enabled) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Dormant Scheduled requires AP recovery or BOOT button wake");
        return ESP_FAIL;
    }

    strlcpy(s_config->sched_tz, new_tz, sizeof(s_config->sched_tz));
    s_config->sched_mode = new_mode;
    memcpy(s_config->sched_rules, new_rules, sizeof(s_config->sched_rules));
    s_config->dormant_time_sync_attempt_s = new_dormant_time_sync_attempt_s;
    s_config->dormant_time_sync_retry_s = new_dormant_time_sync_retry_s;
    s_config->dormant_ap_recovery_enabled = new_dormant_ap_recovery_enabled;
    s_config->dormant_ap_recovery_window_s = new_dormant_ap_recovery_window_s;
    s_config->dormant_button_wake_enabled = new_dormant_button_wake_enabled;
    s_config->dormant_button_wake_s = new_dormant_button_wake_s;
    s_config->dormant_keep_ap_off_during_active_window =
        new_dormant_keep_ap_off_during_active_window;

    esp_err_t save_ret = config_storage_save(s_config);
    if (save_ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scheduler save failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Scheduler saved\"}");
    vTaskDelay(pdMS_TO_TICKS(200));
    scheduler_manager_apply_config(s_config);
    return ESP_OK;
}

static esp_err_t api_scheduler_sync_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    scheduler_manager_sync_now();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Time sync requested\"}");
    return ESP_OK;
}

static esp_err_t api_tailscale_status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    tailscale_status_t status;
    tailscale_manager_get_status(&status);

    httpd_resp_set_type(req, "application/json");
    if (!status.available) {
        httpd_resp_sendstr(req, "{\"available\":false,\"reason\":\"not_compiled\"}");
        return ESP_OK;
    }

    char esc_vpn_ip[TAILSCALE_VPN_IP_LEN * 2];
    char esc_route[CFG_TS_CTRL_HOST_LEN * 2];
    char esc_route_state[TAILSCALE_ROUTE_STATE_LEN * 2];
    json_escape(esc_vpn_ip, sizeof(esc_vpn_ip), status.vpn_ip);
    json_escape(esc_route, sizeof(esc_route), status.advertised_cidr);
    json_escape(esc_route_state, sizeof(esc_route_state), status.subnet_route_state);

    char buf[672];
    snprintf(buf, sizeof(buf),
        "{\"available\":true,\"enabled\":%s,\"scheduler_enabled\":%s,\"configured\":%s,"
        "\"started\":%s,\"connected\":%s,\"state\":\"%s\","
        "\"vpn_ip\":\"%s\",\"peer_count\":%u,\"last_error\":%d,"
        "\"subnet_route_advertised\":%s,\"subnet_route_active\":%s,"
        "\"advertised_cidr\":\"%s\",\"subnet_route_state\":\"%s\","
        "\"heap_internal_free\":%lu,\"heap_psram_free\":%lu}",
        status.enabled ? "true" : "false",
        status.scheduler_enabled ? "true" : "false",
        status.configured ? "true" : "false",
        status.started ? "true" : "false",
        status.connected ? "true" : "false",
        tailscale_manager_state_to_string(status.state),
        esc_vpn_ip,
        status.peer_count,
        (int)status.last_error,
        status.subnet_route_advertised ? "true" : "false",
        status.subnet_route_active ? "true" : "false",
        esc_route,
        esc_route_state,
        (unsigned long)status.heap_internal_free,
        (unsigned long)status.heap_psram_free);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_tailscale_config_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char esc_name[CFG_TS_NAME_LEN * 2];
    char esc_ctrl[CFG_TS_CTRL_HOST_LEN * 2];
    char esc_cidr[CFG_TS_CTRL_HOST_LEN * 2];
    json_escape(esc_name, sizeof(esc_name), s_config->tailscale.device_name);
    json_escape(esc_ctrl, sizeof(esc_ctrl), s_config->tailscale.control_host);
    json_escape(esc_cidr, sizeof(esc_cidr), s_config->tailscale.advertise_cidr);

    char buf[768];
    snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"auth_key_set\":%s,"
        "\"device_name\":\"%s\",\"control_host\":\"%s\","
        "\"enable_derp\":%s,\"enable_disco\":%s,\"enable_stun\":%s,"
        "\"max_peers\":%u,\"expose_web_ui\":%s,"
        "\"expose_lan\":%s,\"net_mode\":%u,\"advertise_cidr\":\"%s\"}",
        s_config->tailscale.enabled ? "true" : "false",
        s_config->tailscale.auth_key[0] ? "true" : "false",
        esc_name,
        esc_ctrl,
        s_config->tailscale.enable_derp ? "true" : "false",
        s_config->tailscale.enable_disco ? "true" : "false",
        s_config->tailscale.enable_stun ? "true" : "false",
        s_config->tailscale.max_peers,
        s_config->tailscale.expose_web_ui ? "true" : "false",
        s_config->tailscale.expose_lan ? "true" : "false",
        (unsigned)s_config->tailscale.net_mode,
        esc_cidr);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_tailscale_config_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char buf[768];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload size");
        return ESP_FAIL;
    }

    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, buf + total, req->content_len - total);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        total += received;
    }
    buf[total] = '\0';

    if (!json_is_object_like(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    bool present;
    bool bval;
    int ival;
    char cidr_buf[CFG_TS_CTRL_HOST_LEN];
    char sval[CFG_TS_AUTH_KEY_LEN];
    network_mode_t old_net_mode = s_config->tailscale.net_mode;

    if (!json_get_bool_strict(buf, "enabled", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enabled");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.enabled = bval;

    if (!json_get_string_strict(buf, "auth_key", sval, sizeof(sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid auth_key");
        return ESP_FAIL;
    }
    if (present) {
        strlcpy(s_config->tailscale.auth_key, sval, sizeof(s_config->tailscale.auth_key));
    }

    if (!json_get_string_strict(buf, "device_name", sval, CFG_TS_NAME_LEN, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device_name");
        return ESP_FAIL;
    }
    if (present) {
        if (!tailscale_device_name_valid(sval)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device_name");
            return ESP_FAIL;
        }
        strlcpy(s_config->tailscale.device_name, sval, sizeof(s_config->tailscale.device_name));
    }

    if (!json_get_string_strict(buf, "control_host", sval, CFG_TS_CTRL_HOST_LEN, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid control_host");
        return ESP_FAIL;
    }
    if (present) {
        if (!tailscale_control_host_valid(sval)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid control_host");
            return ESP_FAIL;
        }
        strlcpy(s_config->tailscale.control_host, sval, sizeof(s_config->tailscale.control_host));
    }

    if (!json_get_bool_strict(buf, "enable_derp", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enable_derp");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.enable_derp = bval;

    if (!json_get_bool_strict(buf, "enable_disco", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enable_disco");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.enable_disco = bval;

    if (!json_get_bool_strict(buf, "enable_stun", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enable_stun");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.enable_stun = bval;

    if (!json_get_int_strict(buf, "max_peers", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid max_peers");
        return ESP_FAIL;
    }
    if (present) {
        if (ival < CFG_TS_MAX_PEERS_MIN || ival > CFG_TS_MAX_PEERS_MAX) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid max_peers");
            return ESP_FAIL;
        }
        s_config->tailscale.max_peers = (uint8_t)ival;
    }

    if (!json_get_bool_strict(buf, "expose_web_ui", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid expose_web_ui");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.expose_web_ui = bval;

    if (!json_get_int_strict(buf, "net_mode", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid net_mode");
        return ESP_FAIL;
    }
    if (present) {
        if (ival != NET_MODE_REPEATER && ival != NET_MODE_TS_GATEWAY) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid net_mode");
            return ESP_FAIL;
        }
        s_config->tailscale.net_mode = (network_mode_t)ival;
    }

    if (!json_get_bool_strict(buf, "expose_lan", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid expose_lan");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.expose_lan = bval;

    if (!json_get_string_strict(buf, "advertise_cidr", cidr_buf, sizeof(cidr_buf), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid advertise_cidr");
        return ESP_FAIL;
    }
    if (present) {
        if (cidr_buf[0] != '\0' && !tailscale_cidr4_valid(cidr_buf)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid advertise_cidr");
            return ESP_FAIL;
        }
        strlcpy(s_config->tailscale.advertise_cidr, cidr_buf, sizeof(s_config->tailscale.advertise_cidr));
    }

    /* Product rule: route advertising means Gateway/SNAT. Keeping route-only
     * hidden avoids implying that advertised routes work without either SNAT
     * or router changes. */
    s_config->tailscale.net_mode = s_config->tailscale.expose_lan ?
                                   NET_MODE_TS_GATEWAY : NET_MODE_REPEATER;
    if (s_config->tailscale.expose_lan &&
        !tailscale_cidr4_valid(s_config->tailscale.advertise_cidr)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid advertise_cidr");
        return ESP_FAIL;
    }

    esp_err_t ret = config_storage_save(s_config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Config save failed");
        return ESP_FAIL;
    }

    bool tailscale_configured = s_config->tailscale.auth_key[0] != '\0';
    bool reboot_to_apply = s_config->tailscale.enabled && tailscale_configured;

    /* Send HTTP response before reboot/apply so the browser receives a
     * deterministic result even when WiFi/Tailscale transport drops. */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (reboot_to_apply) {
        ESP_LOGI(TAG, "Tailscale config saved; rebooting to apply (net_mode_changed=%d)",
                 old_net_mode != s_config->tailscale.net_mode ? 1 : 0);
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Tailscale config saved. Rebooting to apply...\",\"rebooting\":true}");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Tailscale config saved\",\"rebooting\":false}");

    tailscale_manager_apply_config(s_config);
    return ESP_OK;
}

static esp_err_t api_clients_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    wifi_sta_list_t sta_list;
    wifi_sta_mac_ip_list_t ip_list;

    esp_err_t ret = esp_wifi_ap_get_sta_list(&sta_list);
    if (ret != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    esp_wifi_ap_get_sta_list_with_ip(&sta_list, &ip_list);

    // Each client entry ~80 bytes, max 10 clients
    char buf[1024];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[");
    for (int i = 0; i < ip_list.num; i++) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(ip_list.sta[i].mac));
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_list.sta[i].ip));
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"mac\":\"%s\",\"ip\":\"%s\"}",
            i > 0 ? "," : "", mac_str, ip_str);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static char *read_request_body(httpd_req_t *req, size_t max_len)
{
    if (req->content_len <= 0 || req->content_len > (int)max_len) {
        return NULL;
    }

    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    memset(buf, 0, req->content_len + 1);

    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, buf + total, req->content_len - total);
        if (received <= 0) {
            free(buf);
            return NULL;
        }
        total += received;
    }
    buf[total] = '\0';
    return buf;
}

#define USB_HID_JSON_BODY_MAX (USB_HID_MACRO_SCRIPT_MAX_BYTES * 2 + 4096)

static bool usb_hid_id_from_uri(const char *uri, uint32_t *id);

static void usb_hid_add_storage_json(cJSON *root, const usb_hid_macro_storage_stats_t *stats)
{
    cJSON *storage = cJSON_AddObjectToObject(root, "storage");
    if (!storage) return;
    cJSON_AddBoolToObject(storage, "available", stats->available);
    cJSON_AddNumberToObject(storage, "total", stats->total_bytes);
    cJSON_AddNumberToObject(storage, "used", stats->used_bytes);
    cJSON_AddNumberToObject(storage, "free", stats->free_bytes);
    cJSON_AddNumberToObject(storage, "max_macros", stats->max_macros);
    cJSON_AddNumberToObject(storage, "used_macros", stats->used_macros);
    cJSON_AddNumberToObject(storage, "max_payload_bytes", USB_HID_MACRO_SCRIPT_MAX_BYTES);
}

static void usb_hid_add_macro_metadata_json(cJSON *item, const usb_hid_macro_t *macro)
{
    cJSON_AddNumberToObject(item, "id", macro->id);
    cJSON_AddStringToObject(item, "name", macro->name);
    cJSON_AddStringToObject(item, "layout", macro->layout[0] ? macro->layout : USB_HID_MACRO_DEFAULT_LAYOUT);
    cJSON_AddNumberToObject(item, "script_len", macro->script_len);
}

static void usb_hid_send_storage_json(httpd_req_t *req)
{
    usb_hid_macro_storage_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (usb_hid_macro_store_stats(&stats) != ESP_OK) {
        stats.max_macros = USB_HID_MACRO_MAX_METADATA;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return;
    }
    usb_hid_add_storage_json(root, &stats);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
    }
}

static void usb_hid_send_macro_json(httpd_req_t *req, const usb_hid_macro_t *macro, bool include_script)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return;
    }
    usb_hid_add_macro_metadata_json(root, macro);
    if (include_script) cJSON_AddStringToObject(root, "script", macro->script ? macro->script : "");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
    }
}

static void usb_hid_send_storage_full_error(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "507 Insufficient Storage");
    httpd_resp_sendstr(req, message);
}

static void usb_hid_send_storage_unavailable(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "macro storage unavailable");
}

static esp_err_t api_usb_hid_macros_list_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    usb_hid_macro_t *macros = calloc(USB_HID_MACRO_MAX_METADATA, sizeof(usb_hid_macro_t));
    if (!macros) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "allocation failed");
        return ESP_FAIL;
    }

    size_t count = 0;
    esp_err_t ret = usb_hid_macro_store_list(macros, USB_HID_MACRO_MAX_METADATA, &count);
    if (ret == ESP_ERR_INVALID_STATE) {
        free(macros);
        usb_hid_send_storage_unavailable(req);
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        free(macros);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "macro list failed");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(macros);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON *items = cJSON_AddArrayToObject(root, "macros");
    if (!items) {
        cJSON_Delete(root);
        free(macros);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            free(macros);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
            return ESP_FAIL;
        }
        usb_hid_add_macro_metadata_json(item, &macros[i]);
        cJSON_AddItemToArray(items, item);
    }
    usb_hid_macro_storage_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (usb_hid_macro_store_stats(&stats) != ESP_OK) {
        stats.max_macros = USB_HID_MACRO_MAX_METADATA;
    }
    usb_hid_add_storage_json(root, &stats);
    free(macros);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t api_usb_hid_macros_save_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    if (req->content_len > USB_HID_JSON_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "macro payload exceeds configured 512 KiB script limit");
        return ESP_FAIL;
    }
    char *body = read_request_body(req, USB_HID_JSON_BODY_MAX);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid macro payload");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *layout = cJSON_GetObjectItem(root, "layout");
    cJSON *script = cJSON_GetObjectItem(root, "script");
    if (!cJSON_IsString(name)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name is required");
        return ESP_FAIL;
    }
    if (script && !cJSON_IsString(script)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "script must be a string");
        return ESP_FAIL;
    }

    usb_hid_macro_t *macro = calloc(1, sizeof(*macro));
    if (!macro) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    if (cJSON_IsNumber(id) && id->valuedouble > 0) {
        macro->id = (uint32_t)id->valuedouble;
    } else {
        uint32_t uri_id = 0;
        if (usb_hid_id_from_uri(req->uri, &uri_id)) {
            macro->id = uri_id;
        }
    }
    strlcpy(macro->name, name->valuestring, sizeof(macro->name));
    strlcpy(macro->layout, cJSON_IsString(layout) ? layout->valuestring : USB_HID_MACRO_DEFAULT_LAYOUT,
            sizeof(macro->layout));
    if (cJSON_IsString(script)) {
        macro->script = script->valuestring;
        macro->script_len = strlen(script->valuestring);
        if (macro->script_len > USB_HID_MACRO_SCRIPT_MAX_BYTES) {
            cJSON_Delete(root);
            free(macro);
            httpd_resp_set_status(req, "413 Payload Too Large");
            httpd_resp_sendstr(req, "script exceeds configured 512 KiB limit");
            return ESP_FAIL;
        }
    } else if (macro->id != 0 && req->method == HTTP_PUT) {
        usb_hid_macro_t existing;
        memset(&existing, 0, sizeof(existing));
        esp_err_t get_ret = usb_hid_macro_store_get(macro->id, &existing);
        if (get_ret != ESP_OK) {
            cJSON_Delete(root);
            free(macro);
            httpd_resp_send_err(req, get_ret == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND : HTTPD_500_INTERNAL_SERVER_ERROR,
                                get_ret == ESP_ERR_NOT_FOUND ? "macro not found" : "macro get failed");
            return ESP_FAIL;
        }
        macro->script = existing.script;
        macro->script_len = existing.script_len;
    } else {
        cJSON_Delete(root);
        free(macro);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "script is required");
        return ESP_FAIL;
    }

    uint32_t saved_id = 0;
    esp_err_t ret = usb_hid_macro_store_save(macro, &saved_id);
    if (ret == ESP_ERR_INVALID_ARG) {
        if (macro->script != (script && cJSON_IsString(script) ? script->valuestring : NULL)) usb_hid_macro_free(macro);
        cJSON_Delete(root);
        free(macro);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid macro");
        return ESP_FAIL;
    }
    if (ret == ESP_ERR_NO_MEM) {
        if (macro->script != (script && cJSON_IsString(script) ? script->valuestring : NULL)) usb_hid_macro_free(macro);
        cJSON_Delete(root);
        free(macro);
        usb_hid_send_storage_full_error(req, "macro storage full");
        return ESP_FAIL;
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        if (macro->script != (script && cJSON_IsString(script) ? script->valuestring : NULL)) usb_hid_macro_free(macro);
        cJSON_Delete(root);
        free(macro);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "macro not found");
        return ESP_FAIL;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        if (macro->script != (script && cJSON_IsString(script) ? script->valuestring : NULL)) usb_hid_macro_free(macro);
        cJSON_Delete(root);
        free(macro);
        usb_hid_send_storage_unavailable(req);
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        if (macro->script != (script && cJSON_IsString(script) ? script->valuestring : NULL)) usb_hid_macro_free(macro);
        cJSON_Delete(root);
        free(macro);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "macro save failed");
        return ESP_FAIL;
    }

    macro->id = saved_id;
    usb_hid_send_macro_json(req, macro, false);
    if (macro->script != (script && cJSON_IsString(script) ? script->valuestring : NULL)) usb_hid_macro_free(macro);
    cJSON_Delete(root);
    free(macro);
    return ESP_OK;
}

static bool usb_hid_id_from_uri(const char *uri, uint32_t *id)
{
    const char *prefix = "/api/usb-hid/macros/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(uri, prefix, prefix_len) != 0) return false;
    const char *id_text = uri + prefix_len;
    if (*id_text == '\0') return false;
    char *end = NULL;
    unsigned long parsed = strtoul(id_text, &end, 10);
    if (end == id_text || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) return false;
    *id = (uint32_t)parsed;
    return true;
}

static esp_err_t api_usb_hid_macro_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    uint32_t id = 0;
    if (!usb_hid_id_from_uri(req->uri, &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid macro id");
        return ESP_FAIL;
    }

    usb_hid_macro_t *macro = calloc(1, sizeof(*macro));
    if (!macro) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    esp_err_t ret = usb_hid_macro_store_get(id, macro);
    if (ret == ESP_ERR_NOT_FOUND) {
        free(macro);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "macro not found");
        return ESP_FAIL;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        free(macro);
        usb_hid_send_storage_unavailable(req);
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        free(macro);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "macro get failed");
        return ESP_FAIL;
    }

    usb_hid_send_macro_json(req, macro, true);
    usb_hid_macro_free(macro);
    free(macro);
    return ESP_OK;
}

static esp_err_t api_usb_hid_macro_delete_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    uint32_t id = 0;
    if (!usb_hid_id_from_uri(req->uri, &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid macro id");
        return ESP_FAIL;
    }

    esp_err_t ret = usb_hid_macro_store_delete(id);
    if (ret == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "macro not found");
        return ESP_FAIL;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        usb_hid_send_storage_unavailable(req);
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "macro delete failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t api_usb_hid_storage_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    usb_hid_send_storage_json(req);
    return ESP_OK;
}

static esp_err_t api_usb_hid_validate_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    if (req->content_len > USB_HID_JSON_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "validation payload exceeds configured 512 KiB script limit");
        return ESP_FAIL;
    }
    char *body = read_request_body(req, USB_HID_JSON_BODY_MAX);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid validation payload");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    cJSON *script = cJSON_GetObjectItem(root, "script");
    if (!cJSON_IsString(script)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "script is required");
        return ESP_FAIL;
    }

    usb_hid_parse_result_t result = usb_hid_macro_validate(script->valuestring);
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", result.ok);
    cJSON_AddNumberToObject(resp, "line", result.line);
    cJSON_AddStringToObject(resp, "message", result.ok ? "Macro is valid" : result.message);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    httpd_resp_set_type(req, "application/json");
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t api_usb_hid_execute_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    if (req->content_len > USB_HID_JSON_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "execute payload exceeds configured 512 KiB script limit");
        return ESP_FAIL;
    }
    char *body = read_request_body(req, USB_HID_JSON_BODY_MAX);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid execute payload");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *layout = cJSON_GetObjectItem(root, "layout");
    cJSON *script = cJSON_GetObjectItem(root, "script");
    if (!cJSON_IsString(script)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "script is required");
        return ESP_FAIL;
    }
    size_t script_len = strlen(script->valuestring);
    if (script_len > USB_HID_MACRO_SCRIPT_MAX_BYTES) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "script exceeds configured 512 KiB limit");
        return ESP_FAIL;
    }

    usb_hid_macro_t *macro = calloc(1, sizeof(*macro));
    if (!macro) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    strlcpy(macro->name, cJSON_IsString(name) ? name->valuestring : "Unsaved Macro", sizeof(macro->name));
    strlcpy(macro->layout, cJSON_IsString(layout) ? layout->valuestring : USB_HID_MACRO_DEFAULT_LAYOUT,
            sizeof(macro->layout));
    macro->script = script->valuestring;
    macro->script_len = script_len;

    usb_hid_parse_result_t parsed = usb_hid_macro_validate(macro->script);
    if (!parsed.ok) {
        char msg[128];
        snprintf(msg, sizeof(msg), "line %" PRIu32 ": %s", parsed.line, parsed.message);
        cJSON_Delete(root);
        free(macro);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_FAIL;
    }

    esp_err_t ret = usb_hid_executor_start(macro);
    cJSON_Delete(root);
    free(macro);
    if (ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "executor busy");
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "execute failed");
        return ESP_FAIL;
    }

    usb_hid_exec_status_t status;
    usb_hid_executor_get_status(&status);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"queued\",\"dry_run\":%s}",
             status.dry_run ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_usb_hid_stop_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    esp_err_t ret = usb_hid_executor_stop();
    if (ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "executor is not running");
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stop failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"stopping\"}");
    return ESP_OK;
}

static esp_err_t api_usb_hid_panic_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    esp_err_t ret = usb_hid_executor_panic_stop();
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "panic stop failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"panic_stop\"}");
    return ESP_OK;
}

static esp_err_t api_usb_hid_status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    usb_hid_exec_status_t status;
    usb_hid_executor_get_status(&status);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "state", usb_hid_exec_state_name(status.state));
    cJSON_AddBoolToObject(root, "dry_run", status.dry_run);
    cJSON_AddNumberToObject(root, "current_line", status.current_line);
    cJSON_AddNumberToObject(root, "total_lines", status.total_lines);
    cJSON_AddStringToObject(root, "macro_name", status.macro_name);
    cJSON_AddStringToObject(root, "message", status.message);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t usb_hid_send_keepalive_json(httpd_req_t *req)
{
    usb_hid_keepalive_status_t status;
    usb_hid_executor_get_keepalive_status(&status);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "enabled", status.enabled);
    cJSON_AddBoolToObject(root, "paused", status.paused);
    cJSON_AddBoolToObject(root, "dry_run", status.dry_run);
    cJSON_AddBoolToObject(root, "ready", status.ready);
    cJSON_AddNumberToObject(root, "interval_s", status.interval_s);
    cJSON_AddNumberToObject(root, "sent_count", status.sent_count);
    cJSON_AddStringToObject(root, "key", status.key);
    cJSON_AddStringToObject(root, "message", status.message);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t api_usb_hid_keepalive_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    return usb_hid_send_keepalive_json(req);
}

static esp_err_t api_usb_hid_keepalive_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char *body = read_request_body(req, 512);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid keep-awake payload");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    bool enabled = s_config->usb_hid_keepalive_enabled;
    uint16_t interval_s = s_config->usb_hid_keepalive_interval_s;
    char key[CFG_USB_HID_KEEPALIVE_KEY_LEN];
    strlcpy(key, s_config->usb_hid_keepalive_key, sizeof(key));

    cJSON *enabled_item = cJSON_GetObjectItem(root, "enabled");
    if (enabled_item) {
        if (!cJSON_IsBool(enabled_item)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled must be boolean");
            return ESP_FAIL;
        }
        enabled = cJSON_IsTrue(enabled_item);
    }

    cJSON *interval_item = cJSON_GetObjectItem(root, "interval_s");
    if (interval_item) {
        if (!cJSON_IsNumber(interval_item)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "interval_s must be a number");
            return ESP_FAIL;
        }
        int value = interval_item->valueint;
        if (value < CFG_USB_HID_KEEPALIVE_INTERVAL_MIN_S ||
            value > CFG_USB_HID_KEEPALIVE_INTERVAL_MAX_S) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "interval_s out of range");
            return ESP_FAIL;
        }
        interval_s = (uint16_t)value;
    }

    cJSON *key_item = cJSON_GetObjectItem(root, "key");
    if (key_item) {
        if (!cJSON_IsString(key_item) ||
            strlen(key_item->valuestring) >= CFG_USB_HID_KEEPALIVE_KEY_LEN ||
            !usb_hid_keepalive_key_valid(key_item->valuestring)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid keep-awake key");
            return ESP_FAIL;
        }
        strlcpy(key, key_item->valuestring, sizeof(key));
    }
    cJSON_Delete(root);

    s_config->usb_hid_keepalive_enabled = enabled;
    s_config->usb_hid_keepalive_interval_s = interval_s;
    strlcpy(s_config->usb_hid_keepalive_key, key, sizeof(s_config->usb_hid_keepalive_key));

    esp_err_t ret = config_storage_save(s_config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "keep-awake save failed");
        return ESP_FAIL;
    }

    ret = usb_hid_executor_configure_keepalive(enabled, key, interval_s);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "keep-awake apply failed");
        return ESP_FAIL;
    }

    return usb_hid_send_keepalive_json(req);
}

static esp_err_t api_ping_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char target[64];
    if (!json_get_string(buf, "target", target, sizeof(target)) || strlen(target) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing target");
        return ESP_FAIL;
    }

    ping_result_t result;
    esp_err_t ret = wifi_manager_ping(target, &result);

    char resp[256];
    if (ret == ESP_OK && result.success) {
        char ip_str[16];
        esp_ip4_addr_t ip = { .addr = result.addr };
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));
        snprintf(resp, sizeof(resp),
            "{\"success\":true,\"ip\":\"%s\",\"time_ms\":%lu,\"target\":\"%s\"}",
            ip_str, (unsigned long)result.elapsed_ms, target);
    } else {
        const char *reason = "timeout";
        if (ret == ESP_ERR_NOT_FOUND) reason = "dns_failed";
        snprintf(resp, sizeof(resp),
            "{\"success\":false,\"reason\":\"%s\",\"target\":\"%s\"}",
            reason, target);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_logs_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    /* Buffer can be up to 32 KB; prefer PSRAM to avoid DRAM pressure. */
    char *buf = heap_caps_malloc(LOG_BUFFER_SIZE + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(LOG_BUFFER_SIZE + 1);
    }
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t len = log_buffer_read(buf, LOG_BUFFER_SIZE + 1);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

static esp_err_t api_restart_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Restarting...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_auth_change_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char new_user[CFG_USER_LEN];
    char new_pass[CFG_PASS_LEN];

    if (!json_get_string(buf, "new_user", new_user, sizeof(new_user)) ||
        !json_get_string(buf, "new_pass", new_pass, sizeof(new_pass))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing new_user or new_pass");
        return ESP_FAIL;
    }

    if (strlen(new_user) == 0 || strlen(new_pass) < 4) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"User required, password min 4 chars\"}");
        return ESP_OK;
    }

    strlcpy(s_config->web_user, new_user, sizeof(s_config->web_user));
    strlcpy(s_config->web_pass, new_pass, sizeof(s_config->web_pass));
    config_storage_save(s_config);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Credentials updated\"}");
    return ESP_OK;
}

static esp_err_t api_auth_check_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"authenticated\":true}");
    return ESP_OK;
}

static esp_err_t api_ota_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: writing to partition '%s' at offset 0x%lx, size %lu",
             update_partition->label, (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    esp_ota_handle_t ota_handle;
    esp_err_t ret = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int total_received = 0;
    int content_len = req->content_len;
    ESP_LOGI(TAG, "OTA: receiving %d bytes", content_len);

    while (total_received < content_len) {
        int received = httpd_req_recv(req, buf, 4096);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA: receive error at %d/%d", total_received, content_len);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        ret = esp_ota_write(ota_handle, buf, received);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write failed: %s", esp_err_to_name(ret));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        total_received += received;
    }

    free(buf);
    ESP_LOGI(TAG, "OTA: received %d bytes total", total_received);

    ret = esp_ota_end(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA: validation failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set boot partition failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: success, rebooting...");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"OTA successful, rebooting...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    ESP_LOGW(TAG, "Factory reset requested!");
    config_storage_erase();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Factory reset done, rebooting...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t web_server_start(repeater_config_t *config)
{
    s_config = config;

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.max_uri_handlers = 42;
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    http_config.lru_purge_enable = true;
    http_config.stack_size = 8192;
    http_config.recv_wait_timeout = 30;

    esp_err_t ret = httpd_start(&s_server, &http_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t uri_index    = { .uri = "/",            .method = HTTP_GET,  .handler = index_handler };
    httpd_uri_t uri_css      = { .uri = "/styles.css",  .method = HTTP_GET,  .handler = css_handler };
    httpd_uri_t uri_js       = { .uri = "/app.js",      .method = HTTP_GET,  .handler = js_handler };
    httpd_uri_t uri_usb_hid_js = { .uri = "/usb_hid.js", .method = HTTP_GET, .handler = usb_hid_js_handler };
    httpd_uri_t uri_status   = { .uri = "/api/status",  .method = HTTP_GET,  .handler = api_status_handler };
    httpd_uri_t uri_wifi_state = { .uri = "/api/wifi/state", .method = HTTP_GET,  .handler = api_wifi_state_handler };
    httpd_uri_t uri_wifi_pause = { .uri = "/api/wifi/pause", .method = HTTP_POST, .handler = api_wifi_pause_handler };
    httpd_uri_t uri_wifi_resume = { .uri = "/api/wifi/resume", .method = HTTP_POST, .handler = api_wifi_resume_handler };
    httpd_uri_t uri_scan     = { .uri = "/api/scan",    .method = HTTP_GET,  .handler = api_scan_handler };
    httpd_uri_t uri_cfg_get  = { .uri = "/api/config",  .method = HTTP_GET,  .handler = api_config_get_handler };
    httpd_uri_t uri_cfg_post = { .uri = "/api/config",  .method = HTTP_POST, .handler = api_config_post_handler };
    httpd_uri_t uri_runtime_cfg_get = { .uri = "/api/runtime/config", .method = HTTP_GET, .handler = api_runtime_config_get_handler };
    httpd_uri_t uri_runtime_cfg_post = { .uri = "/api/runtime/config", .method = HTTP_POST, .handler = api_runtime_config_post_handler };
    httpd_uri_t uri_ts_status = { .uri = "/api/tailscale/status", .method = HTTP_GET,  .handler = api_tailscale_status_handler };
    httpd_uri_t uri_ts_cfg_get = { .uri = "/api/tailscale/config", .method = HTTP_GET,  .handler = api_tailscale_config_get_handler };
    httpd_uri_t uri_ts_cfg_post = { .uri = "/api/tailscale/config", .method = HTTP_POST, .handler = api_tailscale_config_post_handler };
    httpd_uri_t uri_clients  = { .uri = "/api/clients", .method = HTTP_GET,  .handler = api_clients_handler };
    httpd_uri_t uri_usb_hid_macros_get = { .uri = "/api/usb-hid/macros", .method = HTTP_GET, .handler = api_usb_hid_macros_list_handler };
    httpd_uri_t uri_usb_hid_macros_post = { .uri = "/api/usb-hid/macros", .method = HTTP_POST, .handler = api_usb_hid_macros_save_handler };
    httpd_uri_t uri_usb_hid_storage = { .uri = "/api/usb-hid/storage", .method = HTTP_GET, .handler = api_usb_hid_storage_handler };
    httpd_uri_t uri_usb_hid_macro_get = { .uri = "/api/usb-hid/macros/*", .method = HTTP_GET, .handler = api_usb_hid_macro_get_handler };
    httpd_uri_t uri_usb_hid_macro_put = { .uri = "/api/usb-hid/macros/*", .method = HTTP_PUT, .handler = api_usb_hid_macros_save_handler };
    httpd_uri_t uri_usb_hid_macro_delete = { .uri = "/api/usb-hid/macros/*", .method = HTTP_DELETE, .handler = api_usb_hid_macro_delete_handler };
    httpd_uri_t uri_usb_hid_validate = { .uri = "/api/usb-hid/validate", .method = HTTP_POST, .handler = api_usb_hid_validate_handler };
    httpd_uri_t uri_usb_hid_execute = { .uri = "/api/usb-hid/execute", .method = HTTP_POST, .handler = api_usb_hid_execute_handler };
    httpd_uri_t uri_usb_hid_stop = { .uri = "/api/usb-hid/stop", .method = HTTP_POST, .handler = api_usb_hid_stop_handler };
    httpd_uri_t uri_usb_hid_panic = { .uri = "/api/usb-hid/panic", .method = HTTP_POST, .handler = api_usb_hid_panic_handler };
    httpd_uri_t uri_usb_hid_status = { .uri = "/api/usb-hid/status", .method = HTTP_GET, .handler = api_usb_hid_status_handler };
    httpd_uri_t uri_usb_hid_keepalive_get = { .uri = "/api/usb-hid/keepalive", .method = HTTP_GET, .handler = api_usb_hid_keepalive_get_handler };
    httpd_uri_t uri_usb_hid_keepalive_post = { .uri = "/api/usb-hid/keepalive", .method = HTTP_POST, .handler = api_usb_hid_keepalive_post_handler };
    httpd_uri_t uri_ping     = { .uri = "/api/ping",        .method = HTTP_POST, .handler = api_ping_handler };
    httpd_uri_t uri_restart  = { .uri = "/api/restart",     .method = HTTP_POST, .handler = api_restart_handler };
    httpd_uri_t uri_auth_chg = { .uri = "/api/auth/change", .method = HTTP_POST, .handler = api_auth_change_handler };
    httpd_uri_t uri_auth_chk = { .uri = "/api/auth/check",  .method = HTTP_GET,  .handler = api_auth_check_handler };
    httpd_uri_t uri_ota      = { .uri = "/api/ota",           .method = HTTP_POST, .handler = api_ota_handler };
    httpd_uri_t uri_freset   = { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = api_factory_reset_handler };
    httpd_uri_t uri_logs     = { .uri = "/api/logs",          .method = HTTP_GET,  .handler = api_logs_handler };
    httpd_uri_t uri_loglevel = { .uri = "/api/loglevel",      .method = HTTP_POST, .handler = api_loglevel_post_handler };
    httpd_uri_t uri_sched_status = { .uri = "/api/scheduler/status", .method = HTTP_GET, .handler = api_scheduler_status_handler };
    httpd_uri_t uri_sched_cfg_get = { .uri = "/api/scheduler/config", .method = HTTP_GET, .handler = api_scheduler_config_get_handler };
    httpd_uri_t uri_sched_cfg_post = { .uri = "/api/scheduler/config", .method = HTTP_POST, .handler = api_scheduler_config_post_handler };
    httpd_uri_t uri_sched_sync = { .uri = "/api/scheduler/sync", .method = HTTP_POST, .handler = api_scheduler_sync_handler };
    httpd_uri_t uri_catchall = { .uri = "/*",               .method = HTTP_GET,  .handler = captive_redirect_handler };

    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_css);
    httpd_register_uri_handler(s_server, &uri_js);
    httpd_register_uri_handler(s_server, &uri_usb_hid_js);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_wifi_state);
    httpd_register_uri_handler(s_server, &uri_wifi_pause);
    httpd_register_uri_handler(s_server, &uri_wifi_resume);
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_cfg_get);
    httpd_register_uri_handler(s_server, &uri_cfg_post);
    httpd_register_uri_handler(s_server, &uri_runtime_cfg_get);
    httpd_register_uri_handler(s_server, &uri_runtime_cfg_post);
    httpd_register_uri_handler(s_server, &uri_ts_status);
    httpd_register_uri_handler(s_server, &uri_ts_cfg_get);
    httpd_register_uri_handler(s_server, &uri_ts_cfg_post);
    httpd_register_uri_handler(s_server, &uri_clients);
    httpd_register_uri_handler(s_server, &uri_usb_hid_macros_get);
    httpd_register_uri_handler(s_server, &uri_usb_hid_macros_post);
    httpd_register_uri_handler(s_server, &uri_usb_hid_storage);
    httpd_register_uri_handler(s_server, &uri_usb_hid_macro_get);
    httpd_register_uri_handler(s_server, &uri_usb_hid_macro_put);
    httpd_register_uri_handler(s_server, &uri_usb_hid_macro_delete);
    httpd_register_uri_handler(s_server, &uri_usb_hid_validate);
    httpd_register_uri_handler(s_server, &uri_usb_hid_execute);
    httpd_register_uri_handler(s_server, &uri_usb_hid_stop);
    httpd_register_uri_handler(s_server, &uri_usb_hid_panic);
    httpd_register_uri_handler(s_server, &uri_usb_hid_status);
    httpd_register_uri_handler(s_server, &uri_usb_hid_keepalive_get);
    httpd_register_uri_handler(s_server, &uri_usb_hid_keepalive_post);
    httpd_register_uri_handler(s_server, &uri_ping);
    httpd_register_uri_handler(s_server, &uri_restart);
    httpd_register_uri_handler(s_server, &uri_auth_chg);
    httpd_register_uri_handler(s_server, &uri_auth_chk);
    httpd_register_uri_handler(s_server, &uri_ota);
    httpd_register_uri_handler(s_server, &uri_freset);
    httpd_register_uri_handler(s_server, &uri_logs);
    httpd_register_uri_handler(s_server, &uri_loglevel);
    httpd_register_uri_handler(s_server, &uri_sched_status);
    httpd_register_uri_handler(s_server, &uri_sched_cfg_get);
    httpd_register_uri_handler(s_server, &uri_sched_cfg_post);
    httpd_register_uri_handler(s_server, &uri_sched_sync);
    httpd_register_uri_handler(s_server, &uri_catchall);

    ESP_LOGI(TAG, "HTTP server started on port %d", http_config.server_port);
    return ESP_OK;
}

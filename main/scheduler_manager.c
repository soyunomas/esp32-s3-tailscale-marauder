#include "scheduler_manager.h"
#include "wifi_manager.h"
#include "tailscale_manager.h"
#include "usb_hid_executor.h"
#include "usb_hid_macro_store.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "scheduler";

#define SCHEDULER_TASK_STACK_SIZE 4096
#define SCHEDULER_TASK_PRIORITY 4

static repeater_config_t *s_config = NULL;
static SemaphoreHandle_t s_lock = NULL;
static bool s_ntp_started = false;
static bool s_ntp_synced = false;
static bool s_last_ap_effective = true;
static bool s_last_ap_desired = true;
static bool s_last_sta_effective = true;
static bool s_last_sta_desired = true;
static bool s_last_tailscale_effective = true;
static bool s_last_tailscale_desired = true;
static bool s_last_safety_hold = false;
static bool s_last_dormant_active = false;
static bool s_dormant_time_sync_active = false;
static TickType_t s_dormant_time_sync_started_tick = 0;
static TickType_t s_dormant_next_retry_tick = 0;
static bool s_dormant_ap_recovery_active = false;
static TickType_t s_dormant_ap_recovery_until_tick = 0;
static int s_last_active_rule = -1;
static int s_last_macro_trigger_rule = -1;
static int s_last_macro_trigger_yday = -1;
static uint16_t s_last_macro_trigger_min = UINT16_MAX;
static uint32_t s_last_macro_trigger_id = 0;
static char s_last_reason[SCHED_REASON_LEN] = "Always on";
static char s_last_dormant_reason[SCHED_REASON_LEN] = "";
static TaskHandle_t s_scheduler_task = NULL;

static void scheduler_evaluate(void);

static void scheduler_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void scheduler_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static bool time_is_valid(void)
{
    time_t now = time(NULL);
    return now >= 1700000000;
}

static bool dormant_mode_locked(void)
{
    return s_config && s_config->sched_mode == SCHED_MODE_DORMANT_SCHEDULED;
}

static uint32_t ticks_remaining_s(TickType_t until_tick, TickType_t now_tick)
{
    if (until_tick == 0 || until_tick <= now_tick) return 0;
    TickType_t remaining = until_tick - now_tick;
    return (uint32_t)((remaining + pdMS_TO_TICKS(999)) / pdMS_TO_TICKS(1000));
}

static void format_time(time_t when, char *out, size_t out_size)
{
    if (!time_is_valid()) {
        strlcpy(out, "--", out_size);
        return;
    }
    struct tm tm_now;
    localtime_r(&when, &tm_now);
    strftime(out, out_size, "%Y-%m-%d %H:%M", &tm_now);
}

static int tm_day_bit(const struct tm *tm_now)
{
    return (tm_now->tm_wday + 6) % 7; /* bit0=Mon */
}

static bool rule_matches_time(const scheduler_rule_t *rule, const struct tm *tm_now)
{
    if (!rule->enabled) return false;
    if ((rule->days & (1U << tm_day_bit(tm_now))) == 0) return false;

    uint16_t minute = (uint16_t)(tm_now->tm_hour * 60 + tm_now->tm_min);
    return minute >= rule->start_min && minute < rule->end_min;
}

static bool evaluate_schedule_at(time_t when, bool *ap_desired, bool *sta_desired,
                                 bool *tailscale_desired, int *active_rule)
{
    if (!s_config) return false;
    *ap_desired = true;
    *sta_desired = true;
    *tailscale_desired = true;
    *active_rule = -1;

    if (s_config->sched_mode == SCHED_MODE_ALWAYS_ON) {
        return true;
    }
    if (s_config->sched_mode == SCHED_MODE_MANUAL_OFF) {
        *ap_desired = false;
        return true;
    }
    if (s_config->sched_mode != SCHED_MODE_SCHEDULED &&
        s_config->sched_mode != SCHED_MODE_DORMANT_SCHEDULED) {
        return true;
    }

    bool dormant = s_config->sched_mode == SCHED_MODE_DORMANT_SCHEDULED;
    if (dormant) {
        *ap_desired = false;
        *sta_desired = false;
        *tailscale_desired = false;
    }

    struct tm tm_now;
    localtime_r(&when, &tm_now);
    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        if (rule_matches_time(&s_config->sched_rules[i], &tm_now)) {
            *ap_desired = s_config->sched_rules[i].ap_enabled;
            *sta_desired = s_config->sched_rules[i].sta_enabled;
            *tailscale_desired = s_config->sched_rules[i].tailscale_enabled;
            if (dormant && s_config->dormant_keep_ap_off_during_active_window) {
                *ap_desired = false;
            }
            *active_rule = i;
            return true;
        }
    }
    return true;
}

static void find_next_change(time_t now,
                             uint32_t *next_s, int *next_rule, time_t *next_time)
{
    *next_s = 0;
    *next_rule = -1;
    *next_time = 0;
    if (!time_is_valid() || !s_config) return;

    time_t scan = now - (now % 60) + 60;
    for (int i = 1; i <= 7 * 24 * 60; i++, scan += 60) {
        bool ap = true;
        bool sta = true;
        bool ts = true;
        int rule = -1;
        evaluate_schedule_at(scan, &ap, &sta, &ts, &rule);
        if (ap != s_last_ap_desired || sta != s_last_sta_desired ||
            ts != s_last_tailscale_desired || rule != s_last_active_rule) {
            *next_s = (uint32_t)(scan - now);
            *next_rule = rule;
            *next_time = scan;
            return;
        }
    }
}

static void apply_timezone_locked(void)
{
    if (!s_config) return;
    setenv("TZ", s_config->sched_tz, 1);
    tzset();
}

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_ntp_synced = true;
    ESP_LOGI(TAG, "NTP time synchronized");
    if (s_scheduler_task) {
        xTaskNotifyGive(s_scheduler_task);
    }
}

static void start_sntp_locked(void)
{
    if (s_ntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_config ? s_config->ntp_server_1 : CFG_NTP_SERVER_1_DEFAULT);
    esp_sntp_setservername(1, s_config ? s_config->ntp_server_2 : CFG_NTP_SERVER_2_DEFAULT);
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
    s_ntp_started = true;
    ESP_LOGI(TAG, "SNTP started: server1=%s server2=%s",
             s_config ? s_config->ntp_server_1 : CFG_NTP_SERVER_1_DEFAULT,
             s_config ? s_config->ntp_server_2 : CFG_NTP_SERVER_2_DEFAULT);
}

static bool scheduled_macro_due_locked(time_t now, int previous_active_rule,
                                       int active_rule,
                                       uint32_t *macro_id_out)
{
    if (!s_config || s_config->sched_mode != SCHED_MODE_SCHEDULED) return false;
    if (active_rule < 0 || active_rule >= CFG_SCHED_RULES_MAX) return false;
    if (active_rule == previous_active_rule) return false;

    const scheduler_rule_t *rule = &s_config->sched_rules[active_rule];
    if (rule->usb_hid_macro_id == 0) return false;

    struct tm tm_now;
    localtime_r(&now, &tm_now);

    if (s_last_macro_trigger_rule == active_rule &&
        s_last_macro_trigger_yday == tm_now.tm_yday &&
        s_last_macro_trigger_min == rule->start_min &&
        s_last_macro_trigger_id == rule->usb_hid_macro_id) {
        return false;
    }

    s_last_macro_trigger_rule = active_rule;
    s_last_macro_trigger_yday = tm_now.tm_yday;
    s_last_macro_trigger_min = rule->start_min;
    s_last_macro_trigger_id = rule->usb_hid_macro_id;
    *macro_id_out = rule->usb_hid_macro_id;
    return true;
}

static void trigger_scheduled_macro(uint32_t macro_id, int rule_index)
{
    usb_hid_macro_t *macro = calloc(1, sizeof(*macro));
    if (!macro) {
        ESP_LOGE(TAG, "Scheduled USB HID macro allocation failed for rule %d", rule_index + 1);
        return;
    }

    esp_err_t ret = usb_hid_macro_store_get(macro_id, macro);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scheduled USB HID macro %lu for rule %d not found: %s",
                 (unsigned long)macro_id, rule_index + 1, esp_err_to_name(ret));
        free(macro);
        return;
    }

    ret = usb_hid_executor_start(macro);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scheduled USB HID macro '%s' for rule %d failed to start: %s",
                 macro->name, rule_index + 1, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Scheduled USB HID macro '%s' queued by rule %d",
                 macro->name, rule_index + 1);
    }
    usb_hid_macro_free(macro);
    free(macro);
}

static void scheduler_evaluate(void)
{
    scheduler_lock();
    bool valid = time_is_valid();
    bool dormant_mode = dormant_mode_locked();
    TickType_t now_tick = xTaskGetTickCount();
    bool ap_desired = true;
    bool sta_desired = true;
    bool tailscale_desired = true;
    int active_rule = -1;
    int previous_active_rule = s_last_active_rule;
    const char *reason = "Always on";
    const char *dormant_reason = "";
    uint32_t trigger_macro_id = 0;
    int trigger_macro_rule = -1;
    bool dormant_active = false;

    if (!valid && dormant_mode) {
        ap_desired = false;
        sta_desired = false;
        tailscale_desired = false;
        dormant_active = true;

        bool retry_due = s_dormant_next_retry_tick == 0 ||
                         ticks_remaining_s(s_dormant_next_retry_tick, now_tick) == 0;
        if (!s_dormant_time_sync_active && retry_due) {
            s_dormant_time_sync_active = true;
            s_dormant_time_sync_started_tick = now_tick;
            s_dormant_next_retry_tick = 0;
            if (s_ntp_started) {
                esp_sntp_restart();
            } else {
                start_sntp_locked();
            }
            ESP_LOGI(TAG, "Dormant NTP acquisition started for %u s",
                     s_config ? s_config->dormant_time_sync_attempt_s :
                     CFG_DORMANT_TIME_SYNC_ATTEMPT_DEFAULT_S);
        }

        if (s_dormant_time_sync_active) {
            uint32_t elapsed_s = (uint32_t)((now_tick - s_dormant_time_sync_started_tick) /
                                            pdMS_TO_TICKS(1000));
            uint32_t attempt_s = s_config ? s_config->dormant_time_sync_attempt_s :
                                 CFG_DORMANT_TIME_SYNC_ATTEMPT_DEFAULT_S;
            if (elapsed_s < attempt_s) {
                sta_desired = true;
                reason = "Dormant time sync";
                dormant_reason = "waiting_ntp";
            } else {
                s_dormant_time_sync_active = false;
                uint32_t retry_s = s_config ? s_config->dormant_time_sync_retry_s :
                                   CFG_DORMANT_TIME_SYNC_RETRY_DEFAULT_S;
                s_dormant_next_retry_tick = now_tick + pdMS_TO_TICKS(retry_s * 1000U);
                reason = "Dormant NTP retry pending";
                dormant_reason = "retry_pending";
                if (s_config && s_config->dormant_ap_recovery_enabled) {
                    s_dormant_ap_recovery_active = true;
                    s_dormant_ap_recovery_until_tick = now_tick +
                        pdMS_TO_TICKS((uint32_t)s_config->dormant_ap_recovery_window_s * 1000U);
                    reason = "Dormant AP recovery";
                    dormant_reason = "ap_recovery";
                }
            }
        } else {
            reason = "Dormant NTP retry pending";
            dormant_reason = "retry_pending";
        }
    } else if (!valid) {
        ap_desired = true;
        sta_desired = true;
        tailscale_desired = true;
        reason = "Waiting for time sync";
    } else {
        if (dormant_mode) {
            s_dormant_time_sync_active = false;
            s_dormant_next_retry_tick = 0;
        }
        time_t now = time(NULL);
        evaluate_schedule_at(now, &ap_desired, &sta_desired, &tailscale_desired, &active_rule);
        if (s_config && s_config->sched_mode == SCHED_MODE_MANUAL_OFF) {
            reason = "Manual off";
        } else if (s_config && s_config->sched_mode == SCHED_MODE_SCHEDULED && active_rule >= 0) {
            reason = "Rule active";
        } else if (s_config && s_config->sched_mode == SCHED_MODE_SCHEDULED) {
            reason = "No active rule";
        } else if (dormant_mode && active_rule >= 0) {
            reason = "Dormant rule active";
            dormant_reason = "active_window";
        } else if (dormant_mode) {
            reason = "Dormant";
            dormant_reason = "dormant";
            dormant_active = true;
        }
    }

    bool ap_effective = ap_desired;
    bool sta_effective = sta_desired;
    bool tailscale_effective = tailscale_desired && sta_effective;
    bool safety_hold = false;

    if (dormant_mode && s_dormant_ap_recovery_active) {
        uint32_t recovery_remaining = ticks_remaining_s(s_dormant_ap_recovery_until_tick, now_tick);
        if (recovery_remaining > 0) {
            ap_effective = true;
            dormant_active = false;
            if (dormant_reason[0] == '\0' || strcmp(dormant_reason, "dormant") == 0) {
                reason = "Dormant AP recovery";
                dormant_reason = "ap_recovery";
            }
        } else {
            s_dormant_ap_recovery_active = false;
            s_dormant_ap_recovery_until_tick = 0;
        }
    }

    if (!dormant_mode && (!ap_desired || !sta_desired)) {
        wifi_status_t wifi = {0};
        wifi_manager_get_status(&wifi);
        if (!ap_desired && !wifi.sta_connected) {
            ap_effective = true;
            safety_hold = true;
            reason = "Safety hold: STA offline";
        }
        if (!sta_desired) {
            ap_effective = true;
            tailscale_effective = false;
            reason = "STA off, AP kept for management";
        }
    }

    strlcpy(s_last_reason, reason, sizeof(s_last_reason));
    strlcpy(s_last_dormant_reason, dormant_reason, sizeof(s_last_dormant_reason));
    if (valid && scheduled_macro_due_locked(time(NULL), previous_active_rule,
                                            active_rule, &trigger_macro_id)) {
        trigger_macro_rule = active_rule;
    }
    s_last_ap_desired = ap_desired;
    s_last_ap_effective = ap_effective;
    s_last_sta_desired = sta_desired;
    s_last_sta_effective = sta_effective;
    s_last_tailscale_desired = tailscale_desired;
    s_last_tailscale_effective = tailscale_effective;
    s_last_safety_hold = safety_hold;
    s_last_dormant_active = dormant_active;
    s_last_active_rule = active_rule;
    scheduler_unlock();

    esp_err_t ret = wifi_manager_set_ap_enabled(ap_effective);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply scheduled AP state: %s", esp_err_to_name(ret));
    }
    ret = tailscale_manager_set_scheduler_enabled(tailscale_effective);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply scheduled Tailscale state: %s", esp_err_to_name(ret));
    }
    ret = wifi_manager_set_sta_scheduler_enabled(sta_effective);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply scheduled STA state: %s", esp_err_to_name(ret));
    }
    if (trigger_macro_id != 0) {
        trigger_scheduled_macro(trigger_macro_id, trigger_macro_rule);
    }
}

static void scheduler_task(void *arg)
{
    (void)arg;
    while (true) {
        scheduler_evaluate();
        uint16_t poll_s = s_config ? s_config->scheduler_poll_interval_s :
                          CFG_SCHED_POLL_INTERVAL_DEFAULT_S;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(poll_s * 1000U));
    }
}

esp_err_t scheduler_manager_init(repeater_config_t *config)
{
    s_config = config;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    scheduler_lock();
    apply_timezone_locked();
    start_sntp_locked();
    scheduler_unlock();

    BaseType_t ret = xTaskCreate(scheduler_task, "scheduler",
                                 SCHEDULER_TASK_STACK_SIZE, NULL,
                                 SCHEDULER_TASK_PRIORITY, &s_scheduler_task);
    return ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t scheduler_manager_apply_config(repeater_config_t *config)
{
    scheduler_lock();
    s_config = config;
    apply_timezone_locked();
    if (s_ntp_started) {
        esp_sntp_setservername(0, s_config->ntp_server_1);
        esp_sntp_setservername(1, s_config->ntp_server_2);
        esp_sntp_restart();
    } else {
        start_sntp_locked();
    }
    scheduler_unlock();
    scheduler_evaluate();
    return ESP_OK;
}

esp_err_t scheduler_manager_sync_now(void)
{
    scheduler_lock();
    if (!s_ntp_started) {
        start_sntp_locked();
    } else {
        esp_sntp_restart();
    }
    scheduler_unlock();
    if (s_scheduler_task) {
        xTaskNotifyGive(s_scheduler_task);
    }
    return ESP_OK;
}

esp_err_t scheduler_manager_button_wake_ap(void)
{
    scheduler_lock();
    if (!dormant_mode_locked() || !s_config->dormant_button_wake_enabled) {
        scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t now_tick = xTaskGetTickCount();
    s_dormant_ap_recovery_active = true;
    s_dormant_ap_recovery_until_tick = now_tick +
        pdMS_TO_TICKS((uint32_t)s_config->dormant_button_wake_s * 1000U);
    strlcpy(s_last_dormant_reason, "button_wake", sizeof(s_last_dormant_reason));
    scheduler_unlock();

    ESP_LOGI(TAG, "Dormant AP button wake enabled for %u s",
             s_config->dormant_button_wake_s);
    if (s_scheduler_task) {
        xTaskNotifyGive(s_scheduler_task);
    } else {
        scheduler_evaluate();
    }
    return ESP_OK;
}

void scheduler_manager_get_status(scheduler_status_t *status)
{
    memset(status, 0, sizeof(*status));
    scheduler_lock();
    time_t now = time(NULL);
    bool valid = time_is_valid();
    uint32_t next_s = 0;
    int next_rule = -1;
    time_t next_time = 0;

    status->time_valid = valid;
    status->ntp_synced = s_ntp_synced || esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
    status->ap_effective = s_last_ap_effective;
    status->ap_desired = s_last_ap_desired;
    status->sta_effective = s_last_sta_effective;
    status->sta_desired = s_last_sta_desired;
    status->tailscale_effective = s_last_tailscale_effective;
    status->tailscale_desired = s_last_tailscale_desired;
    status->safety_hold = s_last_safety_hold;
    status->dormant_active = s_last_dormant_active;
    status->dormant_time_sync_active = s_dormant_time_sync_active;
    TickType_t now_tick = xTaskGetTickCount();
    status->dormant_next_retry_s = ticks_remaining_s(s_dormant_next_retry_tick, now_tick);
    status->dormant_ap_recovery_active = s_dormant_ap_recovery_active &&
        ticks_remaining_s(s_dormant_ap_recovery_until_tick, now_tick) > 0;
    status->dormant_ap_recovery_remaining_s =
        ticks_remaining_s(s_dormant_ap_recovery_until_tick, now_tick);
    status->mode = s_config ? s_config->sched_mode : SCHED_MODE_ALWAYS_ON;
    status->active_rule = s_last_active_rule;
    status->next_rule = -1;
    strlcpy(status->timezone, s_config ? s_config->sched_tz : CFG_SCHED_TZ_DEFAULT,
            sizeof(status->timezone));
    strlcpy(status->reason, s_last_reason, sizeof(status->reason));
    strlcpy(status->dormant_reason, s_last_dormant_reason, sizeof(status->dormant_reason));

    format_time(now, status->local_time, sizeof(status->local_time));
    find_next_change(now, &next_s, &next_rule, &next_time);
    status->next_change_s = next_s;
    status->next_rule = next_rule;
    if (next_time > 0) {
        format_time(next_time, status->next_change_local, sizeof(status->next_change_local));
    } else {
        strlcpy(status->next_change_local, "--", sizeof(status->next_change_local));
    }
    scheduler_unlock();
}

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "usb_hid_macro_store.h"
#include "nvs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "usb_hid_store";
static const char *NVS_NAMESPACE = "usb_hid";
static const char *SPIFFS_BASE = "/spiffs";
static const char *MACRO_DIR = "/spiffs/hid_macros";
static const uint32_t SCHEMA_VERSION = 2;
static bool s_storage_mounted;

typedef struct {
    uint32_t ids[USB_HID_MACRO_MAX_METADATA];
    size_t count;
} macro_id_index_t;

static bool printable_multiline_ascii(const char *value)
{
    for (size_t i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '\r' || c == '\n' || c == '\t') continue;
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

bool usb_hid_macro_name_valid(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len == 0 || len >= USB_HID_MACRO_NAME_LEN) return false;
    return printable_multiline_ascii(name);
}

bool usb_hid_macro_layout_valid(const char *layout)
{
    static const char *const valid[] = {
        "es", "us", "uk", "latam", "fr", "de", "it", "pt", "br", "nordic", "be", "tr",
        "pl", "cz", "sk", "hu", "ro", "hr", "sr", "sl", "bg", "ru", "ua", "zh", "tw"
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (layout && strcmp(layout, valid[i]) == 0) return true;
    }
    return false;
}

bool usb_hid_macro_script_valid(const char *script)
{
    size_t len = script ? strlen(script) : 0;
    if (len == 0 || len > USB_HID_MACRO_SCRIPT_MAX_BYTES) return false;
    return printable_multiline_ascii(script);
}

void usb_hid_macro_free(usb_hid_macro_t *item)
{
    if (!item) return;
    free(item->script);
    item->script = NULL;
    item->script_len = 0;
}

static void meta_key(char *out, size_t out_size, uint32_t id, char suffix)
{
    snprintf(out, out_size, "m%08" PRIx32 "%c", id, suffix);
}

static void old_key_for(char *out, size_t out_size, const char *prefix, int slot)
{
    snprintf(out, out_size, "%s%d", prefix, slot);
}

static void script_path(char *out, size_t out_size, uint32_t id)
{
    snprintf(out, out_size, "%s/%08" PRIu32 ".txt", MACRO_DIR, id);
}

static void script_tmp_path(char *out, size_t out_size, uint32_t id)
{
    snprintf(out, out_size, "%s/%08" PRIu32 ".tmp", MACRO_DIR, id);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static esp_err_t ensure_storage_available(void)
{
    return s_storage_mounted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t load_index(nvs_handle_t handle, macro_id_index_t *index)
{
    memset(index, 0, sizeof(*index));
    size_t len = 0;
    esp_err_t ret = nvs_get_blob(handle, "ids", NULL, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(ret, TAG, "read macro id index length failed");
    if (len % sizeof(uint32_t) != 0 ||
        len / sizeof(uint32_t) > USB_HID_MACRO_MAX_METADATA) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len == 0) return ESP_OK;
    ret = nvs_get_blob(handle, "ids", index->ids, &len);
    ESP_RETURN_ON_ERROR(ret, TAG, "read macro id index failed");
    index->count = len / sizeof(uint32_t);
    return ESP_OK;
}

static esp_err_t save_index(nvs_handle_t handle, const macro_id_index_t *index)
{
    if (index->count == 0) {
        esp_err_t ret = nvs_erase_key(handle, "ids");
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
    }
    return nvs_set_blob(handle, "ids", index->ids, index->count * sizeof(uint32_t));
}

static bool index_contains(const macro_id_index_t *index, uint32_t id)
{
    for (size_t i = 0; i < index->count; i++) {
        if (index->ids[i] == id) return true;
    }
    return false;
}

static esp_err_t index_add(macro_id_index_t *index, uint32_t id)
{
    if (index_contains(index, id)) return ESP_OK;
    if (index->count >= USB_HID_MACRO_MAX_METADATA) return ESP_ERR_NO_MEM;
    index->ids[index->count++] = id;
    return ESP_OK;
}

static void index_remove(macro_id_index_t *index, uint32_t id)
{
    for (size_t i = 0; i < index->count; i++) {
        if (index->ids[i] != id) continue;
        memmove(&index->ids[i], &index->ids[i + 1],
                (index->count - i - 1) * sizeof(index->ids[0]));
        index->count--;
        return;
    }
}

static esp_err_t read_metadata(nvs_handle_t handle, uint32_t id, usb_hid_macro_t *item)
{
    char key[16];
    memset(item, 0, sizeof(*item));
    item->id = id;

    meta_key(key, sizeof(key), id, 'n');
    size_t len = sizeof(item->name);
    esp_err_t ret = nvs_get_str(handle, key, item->name, &len);
    ESP_RETURN_ON_ERROR(ret, TAG, "read macro name failed");

    meta_key(key, sizeof(key), id, 'l');
    len = sizeof(item->layout);
    ret = nvs_get_str(handle, key, item->layout, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(item->layout, USB_HID_MACRO_DEFAULT_LAYOUT, sizeof(item->layout));
    } else {
        ESP_RETURN_ON_ERROR(ret, TAG, "read macro layout failed");
    }

    uint32_t script_len = 0;
    meta_key(key, sizeof(key), id, 's');
    ret = nvs_get_u32(handle, key, &script_len);
    ESP_RETURN_ON_ERROR(ret, TAG, "read macro script length failed");
    item->script_len = script_len;
    return ESP_OK;
}

static esp_err_t write_metadata(nvs_handle_t handle, const usb_hid_macro_t *item, uint32_t id)
{
    char key[16];
    meta_key(key, sizeof(key), id, 'n');
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, key, item->name), TAG, "set macro name failed");
    meta_key(key, sizeof(key), id, 'l');
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, key,
                                    item->layout[0] ? item->layout : USB_HID_MACRO_DEFAULT_LAYOUT),
                        TAG, "set macro layout failed");
    meta_key(key, sizeof(key), id, 's');
    ESP_RETURN_ON_ERROR(nvs_set_u32(handle, key, (uint32_t)item->script_len),
                        TAG, "set macro script length failed");
    return ESP_OK;
}

static void erase_metadata(nvs_handle_t handle, uint32_t id)
{
    char key[16];
    meta_key(key, sizeof(key), id, 'n');
    nvs_erase_key(handle, key);
    meta_key(key, sizeof(key), id, 'l');
    nvs_erase_key(handle, key);
    meta_key(key, sizeof(key), id, 's');
    nvs_erase_key(handle, key);
}

static esp_err_t write_script_file(uint32_t id, const char *script, size_t script_len)
{
    char path[64];
    char tmp[64];
    script_path(path, sizeof(path), id);
    script_tmp_path(tmp, sizeof(tmp), id);

    FILE *fp = fopen(tmp, "wb");
    if (!fp) return ESP_FAIL;
    size_t written = fwrite(script, 1, script_len, fp);
    int close_ret = fclose(fp);
    if (written != script_len || close_ret != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t read_script_file(uint32_t id, char **script_out, size_t *len_out)
{
    char path[64];
    script_path(path, sizeof(path), id);
    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_ERR_NOT_FOUND;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    long size = ftell(fp);
    if (size < 0 || size > USB_HID_MACRO_SCRIPT_MAX_BYTES) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(fp);

    char *script = malloc((size_t)size + 1);
    if (!script) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    size_t read_len = fread(script, 1, (size_t)size, fp);
    fclose(fp);
    if (read_len != (size_t)size) {
        free(script);
        return ESP_FAIL;
    }
    script[read_len] = '\0';
    *script_out = script;
    *len_out = read_len;
    return ESP_OK;
}

static esp_err_t remove_script_file(uint32_t id)
{
    char path[64];
    script_path(path, sizeof(path), id);
    if (unlink(path) == 0 || errno == ENOENT) return ESP_OK;
    return ESP_FAIL;
}

static esp_err_t migrate_old_slot(nvs_handle_t handle, macro_id_index_t *index, int slot,
                                  uint32_t *max_id)
{
    char key[24];
    uint32_t id = 0;
    old_key_for(key, sizeof(key), "id", slot);
    esp_err_t ret = nvs_get_u32(handle, key, &id);
    if (ret == ESP_ERR_NVS_NOT_FOUND || id == 0) return ESP_OK;
    ESP_RETURN_ON_ERROR(ret, TAG, "read old macro id failed");

    usb_hid_macro_t old = { .id = id };
    old_key_for(key, sizeof(key), "name", slot);
    size_t len = sizeof(old.name);
    ESP_RETURN_ON_ERROR(nvs_get_str(handle, key, old.name, &len), TAG, "read old macro name failed");

    old_key_for(key, sizeof(key), "layout", slot);
    len = sizeof(old.layout);
    ret = nvs_get_str(handle, key, old.layout, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(old.layout, USB_HID_MACRO_DEFAULT_LAYOUT, sizeof(old.layout));
    } else {
        ESP_RETURN_ON_ERROR(ret, TAG, "read old macro layout failed");
    }

    old_key_for(key, sizeof(key), "script", slot);
    len = 0;
    ESP_RETURN_ON_ERROR(nvs_get_str(handle, key, NULL, &len), TAG, "read old macro script length failed");
    if (len == 0 || len > USB_HID_MACRO_SCRIPT_MAX_BYTES + 1) return ESP_ERR_INVALID_SIZE;
    old.script = malloc(len);
    if (!old.script) return ESP_ERR_NO_MEM;
    ret = nvs_get_str(handle, key, old.script, &len);
    if (ret != ESP_OK) {
        usb_hid_macro_free(&old);
        return ret;
    }
    old.script_len = strlen(old.script);
    if (!usb_hid_macro_name_valid(old.name) ||
        !usb_hid_macro_layout_valid(old.layout) ||
        !usb_hid_macro_script_valid(old.script)) {
        ESP_LOGW(TAG, "Skipping invalid old HID macro in slot %d", slot);
        usb_hid_macro_free(&old);
        return ESP_OK;
    }

    char path[64];
    script_path(path, sizeof(path), id);
    if (!file_exists(path)) {
        ret = write_script_file(id, old.script, old.script_len);
        if (ret != ESP_OK) {
            usb_hid_macro_free(&old);
            return ret;
        }
    }
    ret = write_metadata(handle, &old, id);
    if (ret == ESP_OK) ret = index_add(index, id);
    if (id > *max_id) *max_id = id;
    usb_hid_macro_free(&old);
    return ret;
}

static esp_err_t migrate_old_slots(nvs_handle_t handle)
{
    macro_id_index_t index;
    esp_err_t ret = load_index(handle, &index);
    ESP_RETURN_ON_ERROR(ret, TAG, "load macro index for migration failed");

    uint32_t max_id = 0;
    for (size_t i = 0; i < index.count; i++) {
        if (index.ids[i] > max_id) max_id = index.ids[i];
    }

    for (int slot = 0; slot < 12; slot++) {
        ret = migrate_old_slot(handle, &index, slot, &max_id);
        ESP_RETURN_ON_ERROR(ret, TAG, "old macro slot migration failed");
    }

    uint32_t next_id = 1;
    nvs_get_u32(handle, "next_id", &next_id);
    if (next_id <= max_id) {
        next_id = max_id + 1;
        ESP_RETURN_ON_ERROR(nvs_set_u32(handle, "next_id", next_id), TAG, "set migrated next_id failed");
    }
    ESP_RETURN_ON_ERROR(save_index(handle, &index), TAG, "save migrated macro index failed");
    ESP_RETURN_ON_ERROR(nvs_set_u32(handle, "schema_ver", SCHEMA_VERSION), TAG, "set macro schema failed");
    return nvs_commit(handle);
}

esp_err_t usb_hid_macro_store_init(void)
{
    if (!s_storage_mounted) {
        esp_vfs_spiffs_conf_t conf = {
            .base_path = SPIFFS_BASE,
            .partition_label = "storage",
            .max_files = 8,
            .format_if_mount_failed = true,
        };
        esp_err_t ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS storage mount failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_storage_mounted = true;
        if (mkdir(MACRO_DIR, 0775) != 0 && errno != EEXIST && errno != ENOSYS) {
            ESP_LOGW(TAG, "Macro directory creation skipped: errno=%d", errno);
        }
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    uint32_t schema_ver = 0;
    nvs_get_u32(handle, "schema_ver", &schema_ver);
    ret = schema_ver == SCHEMA_VERSION ? ESP_OK : migrate_old_slots(handle);
    nvs_close(handle);
    if (ret == ESP_OK) ESP_LOGI(TAG, "USB HID macro storage ready");
    return ret;
}

esp_err_t usb_hid_macro_store_list(usb_hid_macro_t *items, size_t max_items, size_t *count)
{
    if (!items || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    ESP_RETURN_ON_ERROR(ensure_storage_available(), TAG, "macro storage unavailable");

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;

    macro_id_index_t index;
    ret = load_index(handle, &index);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    for (size_t i = 0; i < index.count && *count < max_items; i++) {
        usb_hid_macro_t item;
        if (read_metadata(handle, index.ids[i], &item) == ESP_OK) {
            items[*count] = item;
            (*count)++;
        }
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t usb_hid_macro_store_get(uint32_t id, usb_hid_macro_t *item)
{
    if (id == 0 || !item) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(ensure_storage_available(), TAG, "macro storage unavailable");

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) return ret;
    ret = read_metadata(handle, id, item);
    nvs_close(handle);
    if (ret != ESP_OK) {
        memset(item, 0, sizeof(*item));
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret;
    }

    ret = read_script_file(id, &item->script, &item->script_len);
    if (ret != ESP_OK) {
        usb_hid_macro_free(item);
        memset(item, 0, sizeof(*item));
    }
    return ret;
}

esp_err_t usb_hid_macro_store_save(const usb_hid_macro_t *item, uint32_t *saved_id)
{
    if (!item || !saved_id || !item->script ||
        item->script_len == 0 || item->script_len > USB_HID_MACRO_SCRIPT_MAX_BYTES ||
        strlen(item->script) != item->script_len ||
        !usb_hid_macro_name_valid(item->name) ||
        !usb_hid_macro_layout_valid(item->layout) ||
        !usb_hid_macro_script_valid(item->script)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_storage_available(), TAG, "macro storage unavailable");

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    macro_id_index_t index;
    ret = load_index(handle, &index);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    uint32_t id = item->id;
    if (id == 0) {
        uint32_t next_id = 1;
        nvs_get_u32(handle, "next_id", &next_id);
        id = next_id == 0 ? 1 : next_id;
        ret = nvs_set_u32(handle, "next_id", id + 1);
        if (ret != ESP_OK) {
            nvs_close(handle);
            return ret;
        }
    } else if (!index_contains(&index, id)) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    bool is_new = !index_contains(&index, id);
    ret = index_add(&index, id);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    ret = write_script_file(id, item->script, item->script_len);
    if (ret == ESP_OK) ret = write_metadata(handle, item, id);
    if (ret == ESP_OK) ret = save_index(handle, &index);
    if (ret == ESP_OK) ret = nvs_set_u32(handle, "schema_ver", SCHEMA_VERSION);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    if (ret != ESP_OK && is_new) remove_script_file(id);
    nvs_close(handle);
    if (ret == ESP_OK) *saved_id = id;
    return ret;
}

esp_err_t usb_hid_macro_store_delete(uint32_t id)
{
    if (id == 0) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(ensure_storage_available(), TAG, "macro storage unavailable");

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    macro_id_index_t index;
    ret = load_index(handle, &index);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }
    if (!index_contains(&index, id)) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    erase_metadata(handle, id);
    index_remove(&index, id);
    ret = save_index(handle, &index);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret == ESP_OK) ret = remove_script_file(id);
    return ret;
}

esp_err_t usb_hid_macro_store_stats(usb_hid_macro_storage_stats_t *stats)
{
    if (!stats) return ESP_ERR_INVALID_ARG;
    memset(stats, 0, sizeof(*stats));
    stats->max_macros = USB_HID_MACRO_MAX_METADATA;
    stats->available = s_storage_mounted;
    if (s_storage_mounted) {
        size_t total = 0;
        size_t used = 0;
        esp_err_t ret = esp_spiffs_info("storage", &total, &used);
        if (ret == ESP_OK) {
            stats->total_bytes = total;
            stats->used_bytes = used;
            stats->free_bytes = total > used ? total - used : 0;
        }
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;
    macro_id_index_t index;
    ret = load_index(handle, &index);
    nvs_close(handle);
    if (ret != ESP_OK) return ret;
    stats->used_macros = index.count;
    return ESP_OK;
}

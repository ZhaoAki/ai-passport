/* Cyber Lanlan Passport application entry.
 *
 * The application is a networked pet-care recorder: it renders the service's
 * records and reminders, never creates them, and keeps working offline from a
 * bounded cache.
 *
 * Runtime shape (see docs/applications/cyber-lanlan.md section 1):
 *   - one input/UI worker task owns the model, the cache and its NVS commit, the
 *     rendering, the reminder evaluation, the backlight policy and the PCM feed;
 *   - one network worker task is created by lanlan_sync_init();
 *   - the button callback only enqueues a (key, event) pair;
 *   - every LVGL access from a worker is wrapped in bsp_lvgl_lock().
 *
 * Nothing here logs the Wi-Fi password, the device token or a caregiver note. */

#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lanlan_cache.h"
#include "lanlan_caregiver.h"
#include "lanlan_model.h"
#include "lanlan_record.h"
#include "lanlan_reminder.h"
#include "lanlan_sfx_data.h"
#include "lanlan_strings.h"
#include "lanlan_sync.h"
#include "lanlan_time.h"
#include "lanlan_ui.h"

static const char *TAG = "lanlan";

/* assets/music/lanlan_sfx_16k.pcm, linked by target_add_binary_data(). */
extern const uint8_t lanlan_sfx_start[] asm("_binary_lanlan_sfx_16k_pcm_start");
extern const uint8_t lanlan_sfx_end[] asm("_binary_lanlan_sfx_16k_pcm_end");

#define LANLAN_APP_TASK_STACK 16384
#define LANLAN_APP_TASK_PRIORITY 5
#define LANLAN_INPUT_QUEUE_DEPTH 8
#define LANLAN_REMINDER_POLL_US (10 * 1000000LL)
#define LANLAN_BATTERY_POLL_US (30 * 1000000LL)
#define LANLAN_BANNER_US (30 * 1000000LL)
#define LANLAN_BACKLIGHT_DIM_PERCENT 15
#define LANLAN_BACKLIGHT_ON_PERCENT 65
#define LANLAN_AUDIO_IDLE_SUSPEND_US (2000000LL)
#define LANLAN_RUNG_BLOB_BYTES \
    (2u + LANLAN_CACHE_REMINDER_CAPACITY * (LANLAN_ID_BYTES + 8u))

/* --------------------------------------------------------------- state -- */

typedef struct {
    bsp_btn_t key;
    bsp_btn_ev_t event;
} input_event_t;

static lanlan_config_t s_config;
static lanlan_cred_t s_cred;
static lanlan_model_t *s_model;
static lanlan_records_view_t *s_view;
static lanlan_cache_t *s_staging;
static void *s_work;
static uint32_t s_cursor;

static nvs_handle_t s_nvs;
static bool s_nvs_open;
static SemaphoreHandle_t s_nvs_mutex;
static SemaphoreHandle_t s_app_mutex;

static bool s_display_ok;
static bool s_audio_ok;
static bool s_buttons_ok;
static bool s_audio_awake;
static bool s_ready;
static int s_battery = -1;

static QueueHandle_t s_queue;
static lv_obj_t *s_screen;

/* Clock anchor: a monotonic extrapolation from the last trusted source. */
static portMUX_TYPE s_clock_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_clock_epoch;
static int64_t s_clock_anchor_us;
static bool s_clock_synced;
static bool s_clock_trusted;
static int16_t s_pending_offset;
static bool s_pending_offset_valid;

/* Cache bookkeeping shown by the UI. */
static bool s_cache_rebuilt;
static bool s_storage_limited;
static uint32_t s_cache_generation;

/* Reminder rung instances (NVS `rem_v1`), independent of the cache blob so a
 * cache rebuild cannot re-ring an already-rung instance. */
static struct {
    uint8_t count;
    uint8_t id[LANLAN_CACHE_REMINDER_CAPACITY][LANLAN_ID_BYTES];
    int64_t day[LANLAN_CACHE_REMINDER_CAPACITY];
} s_rungs;

/* Caregiver directory (NVS `care_v1`): service user id -> display name, plus
 * the 0/1 slot the cached record struct stores. Defined and host-tested in
 * main/lanlan_caregiver.c. */
static lanlan_caregiver_table_t s_caregivers;
/* Set when `care_v1` could not be read as the current format. The cached
 * records then hold slots from the previous directory, so they are dropped and
 * re-fetched instead of showing the wrong caregiver. */
static bool s_caregiver_reset;

/* Backlight and animation bookkeeping. */
static int64_t s_last_input_us;
static bool s_dimmed;
static bool s_dark;
static bool s_render_dirty = true;
static int64_t s_last_reminder_us;
static int64_t s_last_battery_us;
static int64_t s_banner_until_us;
static int s_rendered_battery = -2;
static uint32_t s_rendered_sync_state = 0xFFFFFFFFu;
static bool s_clear_cache_requested;

/* Audio feed. */
static uint32_t s_pcm_offset;
static uint32_t s_pcm_remaining;
static int64_t s_last_audio_write;

/* ------------------------------------------------------------- helpers -- */

static int64_t now_us(void) { return esp_timer_get_time(); }

static void put16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t get16(const uint8_t *in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static void put64(uint8_t *out, int64_t value) {
    uint64_t raw = (uint64_t)value;
    for (int i = 0; i < 8; ++i) out[i] = (uint8_t)((raw >> (8 * i)) & 0xFFu);
}

static int64_t get64(const uint8_t *in) {
    uint64_t raw = 0;
    for (int i = 0; i < 8; ++i) raw |= (uint64_t)in[i] << (8 * i);
    return (int64_t)raw;
}

/* ------------------------------------------------------------ clock -- */

static void clock_apply(int64_t epoch, int16_t offset_minutes, bool trusted, void *user) {
    (void)user;
    int64_t anchor = esp_timer_get_time();
    portENTER_CRITICAL(&s_clock_mux);
    s_clock_epoch = epoch;
    s_clock_anchor_us = anchor;
    s_clock_synced = true;
    s_clock_trusted = trusted;
    if (offset_minutes != s_config.utc_offset_minutes) {
        s_pending_offset = offset_minutes;
        s_pending_offset_valid = true;
    }
    portEXIT_CRITICAL(&s_clock_mux);
}

static bool clock_is_trusted(void) { return s_clock_trusted; }

static int64_t clock_now(void) {
    portENTER_CRITICAL(&s_clock_mux);
    int64_t epoch = s_clock_epoch;
    int64_t anchor = s_clock_anchor_us;
    bool synced = s_clock_synced;
    portEXIT_CRITICAL(&s_clock_mux);
    if (!synced) return 0;
    return epoch + (now_us() - anchor) / 1000000;
}

static void local_now(lanlan_local_now_t *out, int64_t *epoch_out) {
    int64_t epoch = clock_now();
    int16_t offset = s_config.utc_offset_minutes;
    *epoch_out = epoch;
    memset(out, 0, sizeof(*out));
    if (epoch <= 0) return;
    lanlan_date_t date;
    if (lanlan_time_local_date(epoch, offset, &date) == LANLAN_TIME_OK) out->local_day = date.day;
    int hour = 0;
    int minute = 0;
    if (lanlan_time_local_hhmm(epoch, offset, &hour, &minute) == LANLAN_TIME_OK) {
        out->hour = hour;
        out->minute = minute;
    }
}

/* ------------------------------------------------------- NVS persistence -- */

/* The caller holds s_nvs_mutex. */

static esp_err_t store_config_locked(void) {
    uint8_t blob[LANLAN_CONFIG_BLOB_BYTES];
    size_t len = lanlan_config_encode(&s_config, blob, sizeof(blob));
    if (len == 0) return ESP_ERR_INVALID_SIZE;
    return nvs_set_blob(s_nvs, LANLAN_NVS_KEY_CONFIG, blob, len);
}

static esp_err_t store_cred_locked(void) {
    uint8_t blob[LANLAN_CRED_BLOB_BYTES];
    size_t len = lanlan_cred_encode(&s_cred, blob, sizeof(blob));
    if (len == 0) return ESP_ERR_INVALID_SIZE;
    return nvs_set_blob(s_nvs, LANLAN_NVS_KEY_CRED, blob, len);
}

static esp_err_t store_rungs_locked(void) {
    uint8_t blob[LANLAN_RUNG_BLOB_BYTES];
    memset(blob, 0, sizeof(blob));
    put16(blob, s_rungs.count);
    size_t offset = 2;
    for (uint8_t i = 0; i < s_rungs.count; ++i) {
        memcpy(blob + offset, s_rungs.id[i], LANLAN_ID_BYTES);
        offset += LANLAN_ID_BYTES;
        put64(blob + offset, s_rungs.day[i]);
        offset += 8;
    }
    return nvs_set_blob(s_nvs, LANLAN_NVS_KEY_REMINDER, blob, sizeof(blob));
}

static esp_err_t store_caregivers_locked(void) {
    /* Versioned, fixed length and CRC checked by the caregiver module. */
    uint8_t blob[LANLAN_CAREGIVER_BLOB_BYTES];
    size_t length = lanlan_caregiver_encode(&s_caregivers, blob, sizeof(blob));
    if (length == 0) return ESP_ERR_INVALID_SIZE;
    return nvs_set_blob(s_nvs, LANLAN_NVS_KEY_CAREGIVER, blob, length);
}

static void load_config(void) {
    lanlan_config_defaults(&s_config);
    lanlan_cred_defaults(&s_cred);
    if (!s_nvs_open) return;
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    uint8_t blob[LANLAN_CONFIG_BLOB_BYTES];
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_nvs, LANLAN_NVS_KEY_CONFIG, blob, &len) == ESP_OK) {
        (void)lanlan_config_decode(blob, len, &s_config);
    }
    uint8_t cred_blob[LANLAN_CRED_BLOB_BYTES];
    len = sizeof(cred_blob);
    if (nvs_get_blob(s_nvs, LANLAN_NVS_KEY_CRED, cred_blob, &len) == ESP_OK) {
        (void)lanlan_cred_decode(cred_blob, len, &s_cred);
    }
    /* Reminder rung instances survive a cache rebuild. */
    uint8_t rung_blob[LANLAN_RUNG_BLOB_BYTES];
    len = sizeof(rung_blob);
    memset(&s_rungs, 0, sizeof(s_rungs));
    if (nvs_get_blob(s_nvs, LANLAN_NVS_KEY_REMINDER, rung_blob, &len) == ESP_OK
        && len >= LANLAN_RUNG_BLOB_BYTES) {
        uint16_t count = get16(rung_blob);
        if (count > LANLAN_CACHE_REMINDER_CAPACITY) count = LANLAN_CACHE_REMINDER_CAPACITY;
        size_t offset = 2;
        for (uint16_t i = 0; i < count; ++i) {
            memcpy(s_rungs.id[i], rung_blob + offset, LANLAN_ID_BYTES);
            offset += LANLAN_ID_BYTES;
            s_rungs.day[i] = get64(rung_blob + offset);
            offset += 8;
        }
        s_rungs.count = (uint8_t)count;
    }
    uint8_t care_blob[LANLAN_CAREGIVER_BLOB_BYTES];
    len = sizeof(care_blob);
    lanlan_caregiver_clear(&s_caregivers);
    s_caregiver_reset = true;
    if (nvs_get_blob(s_nvs, LANLAN_NVS_KEY_CAREGIVER, care_blob, &len) == ESP_OK) {
        lanlan_caregiver_status_t status = lanlan_caregiver_decode(care_blob, len, &s_caregivers);
        s_caregiver_reset = status != LANLAN_CAREGIVER_OK;
        if (s_caregiver_reset) {
            ESP_LOGW(TAG, "caregiver directory rejected (%s); re-learning from the service",
                     lanlan_caregiver_status_name(status));
        }
    } else {
        ESP_LOGI(TAG, "no caregiver directory yet; it is learned from the service");
    }
    xSemaphoreGive(s_nvs_mutex);
}

static void rung_record(const uint8_t id[LANLAN_ID_BYTES], int64_t day) {
    for (uint8_t i = 0; i < s_rungs.count; ++i) {
        if (memcmp(s_rungs.id[i], id, LANLAN_ID_BYTES) == 0) {
            if (day > s_rungs.day[i]) s_rungs.day[i] = day;
            return;
        }
    }
    if (s_rungs.count >= LANLAN_CACHE_REMINDER_CAPACITY) return;
    memcpy(s_rungs.id[s_rungs.count], id, LANLAN_ID_BYTES);
    s_rungs.day[s_rungs.count] = day;
    s_rungs.count++;
}

/* Re-applies the persisted rung instances to a freshly loaded or replaced
 * cache, so a rebuild, a restart or a clock change cannot ring twice. */
static void rungs_apply(void) {
    for (uint32_t i = 0; i < s_model->cache.reminder_count; ++i) {
        lanlan_reminder_t *reminder = &s_model->cache.reminders[i];
        for (uint8_t r = 0; r < s_rungs.count; ++r) {
            if (memcmp(s_rungs.id[r], reminder->id, LANLAN_ID_BYTES) == 0) {
                if (s_rungs.day[r] > reminder->last_rung_day) {
                    reminder->last_rung_day = s_rungs.day[r];
                }
                break;
            }
        }
    }
}

/* --------------------------------------------------------- cache load -- */

static void cache_load(void) {
    lanlan_cache_clear(&s_model->cache);
    s_cursor = 0;
    if (!s_nvs_open) {
        s_cache_rebuilt = true;
        return;
    }
    uint8_t *image = (uint8_t *)s_work;
    size_t len = LANLAN_CACHE_MAX_BLOB_BYTES;
    esp_err_t err;
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    err = nvs_get_blob(s_nvs, LANLAN_NVS_KEY_CACHE, image, &len);
    xSemaphoreGive(s_nvs_mutex);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no cached records yet");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cache unreadable (%s); rebuilding from the service", esp_err_to_name(err));
        s_cache_rebuilt = true;
        return;
    }
    /* The decode result is a whole extra cache, so it is allocated for the load
     * and released immediately: nothing else needs it. */
    lanlan_cache_decode_result_t *result =
        heap_caps_malloc(sizeof(*result), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!result) {
        ESP_LOGE(TAG, "no memory to decode the cache; rebuilding from the service");
        s_cache_rebuilt = true;
        return;
    }
    lanlan_cache_status_t status = lanlan_cache_decode(image, len, result);
    if (status == LANLAN_CACHE_OK || status == LANLAN_CACHE_EMPTY) {
        if (s_caregiver_reset && result->cache.record_count > 0) {
            /* A cached record stores a caregiver *slot*; the directory it was
             * mapped against is gone, so the records are re-fetched rather than
             * attributed to the wrong person. */
            ESP_LOGW(TAG, "caregiver directory changed; rebuilding the record cache");
            result->cache.record_count = 0;
            result->cache.tombstone_count = 0;
            result->cache.cursor = 0;
            s_cache_rebuilt = true;
        }
        lanlan_model_load_cache(s_model, &result->cache);
        s_cursor = result->cache.cursor;
        rungs_apply();
        ESP_LOGI(TAG, "cache loaded: %u records, %u reminders, cursor=%u",
                 (unsigned)result->cache.record_count, (unsigned)result->cache.reminder_count,
                 (unsigned)s_cursor);
    } else {
        /* Corrupt, truncated or from another format version: discard it and let
         * the next sync rebuild it. No other NVS key is touched. */
        ESP_LOGW(TAG, "cache rejected (%s); rebuilding from the service",
                 lanlan_cache_status_name(status));
        lanlan_cache_clear(&s_model->cache);
        s_cursor = 0;
        s_cache_rebuilt = true;
    }
    heap_caps_free(result);
}

/* ============================================================ sync glue == */

/* Applies the family directory from the service. It runs before the records of
 * the same payload are mapped, so a record's caregiver id resolves through the
 * service's own list rather than through whatever order records arrived in. */
static void sync_members(const lanlan_caregiver_member_t *members, int count, void *user) {
    (void)user;
    if (!s_app_mutex) return;
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    lanlan_caregiver_apply_members(&s_caregivers, members, count);
    s_caregiver_reset = false;
    s_render_dirty = true;
    xSemaphoreGive(s_app_mutex);
}

/* Maps a record's caregiver user id to the 0/1 slot the record struct stores.
 * The record validator accepts only 0 and 1, so an id that cannot take a slot
 * (a third caregiver) still has to answer with one; such a record renders the
 * neutral label for the slot it lands in. */
static uint8_t sync_caregiver(const char *id, void *user) {
    (void)user;
    if (!s_app_mutex) return 0;
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    int slot = lanlan_caregiver_learn(&s_caregivers, id, NULL);
    if (slot < 0) slot = lanlan_caregiver_index_for_id(&s_caregivers, id);
    if (slot < 0) slot = 0;
    xSemaphoreGive(s_app_mutex);
    return (uint8_t)slot;
}

/* The commit rule, in one place: apply into the staging buffer, persist the
 * batch plus the new cursor, and publish only after nvs_commit() returned
 * ESP_OK. Runs on the sync worker. */
static bool apply_sync_batch(lanlan_sync_apply_mode_t mode, const lanlan_sync_batch_t *batch,
                             void *user) {
    (void)user;
    if (!batch || !s_app_mutex || !s_staging) return false;
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    bool ok = false;
    do {
        const lanlan_cache_t *live = &s_model->cache;
        if (mode == LANLAN_SYNC_APPLY_REPLACE) {
            /* Full resynchronization: the batch carries current-state records,
             * so the base is an empty cache. The reminder rung instances are
             * re-applied afterwards from their own key. */
            lanlan_cache_clear(s_staging);
            live = s_staging;
        }
        lanlan_cache_drop_report_t drops;
        lanlan_cache_revoked_report_t removed;
        lanlan_cache_t *target =
            (mode == LANLAN_SYNC_APPLY_REPLACE) ? &s_view->cache : s_staging;
        lanlan_cache_status_t status =
            lanlan_cache_apply_batch(live, batch, target, &drops, &removed);
        if (status != LANLAN_CACHE_OK) {
            ESP_LOGW(TAG, "batch rejected (%s); cache unchanged", lanlan_cache_status_name(status));
            break;
        }
        lanlan_cache_t *staged = target;
        staged->cursor = batch->cursor;

        uint8_t *image = (uint8_t *)s_work;
        size_t length = lanlan_cache_encode(staged, image, LANLAN_SYNC_WORK_BYTES);
        if (length != (size_t)LANLAN_CACHE_BLOB_BYTES) {
            ESP_LOGE(TAG, "cache encode failed");
            break;
        }
        xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
        esp_err_t err = s_nvs_open ? nvs_set_blob(s_nvs, LANLAN_NVS_KEY_CACHE, image, length)
                                   : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) err = store_rungs_locked();
        if (err == ESP_OK) err = store_caregivers_locked();
        if (err == ESP_OK) err = nvs_commit(s_nvs);
        xSemaphoreGive(s_nvs_mutex);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cache commit failed: %s", esp_err_to_name(err));
            s_storage_limited = true;
            break;
        }
        s_storage_limited = false;
        if (!lanlan_cache_commit(&s_model->cache, staged, &s_cursor, batch->cursor, true)) {
            ESP_LOGE(TAG, "cache commit refused");
            break;
        }
        lanlan_model_load_cache(s_model, staged);
        /* Rung instances come from their own key, so a rebuilt or replaced
         * cache cannot re-ring an instance that already rang. */
        rungs_apply();
        s_view->cache = s_model->cache;
        s_view->cursor = s_cursor;
        s_cache_rebuilt = false;
        s_cache_generation++;
        s_render_dirty = true;
        ok = true;
        ESP_LOGI(TAG, "batch applied: %u records, %u reminders, cursor=%u",
                 (unsigned)s_model->cache.record_count,
                 (unsigned)s_model->cache.reminder_count, (unsigned)s_cursor);
    } while (false);
    if (!ok) {
        /* Keep the previously published cache visible. */
        s_view->cache = s_model->cache;
        s_view->cursor = s_cursor;
    }
    xSemaphoreGive(s_app_mutex);
    return ok;
}

static void refresh_view(void) {
    s_view->cache = s_model->cache;
    s_view->cursor = s_cursor;
    s_view->clock_trusted = clock_is_trusted();
    s_view->utc_offset_min = s_config.utc_offset_minutes;
    s_view->last_sync_epoch = lanlan_sync_last_ok_epoch();
    s_view->now_epoch = clock_now();
    s_view->battery_percent = s_battery;
    /* The row helpers fill the window that starts at these offsets, so the
     * renderer and the data it draws stay aligned while a list is scrolled. */
    s_view->list_offset = s_model->list_offset;
    s_view->reminder_offset = s_model->reminder_offset;
}

/* -------------------------------------------------------------- audio -- */

static void audio_failed(const char *what, esp_err_t err) {
    s_audio_ok = false;
    s_pcm_remaining = 0;
    ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
}

static void play_clip(unsigned index) {
    if (!s_audio_ok || index >= LANLAN_SFX_COUNT) return;
    if (s_model->globally_muted) return;
    const lanlan_sfx_clip_t *clip = &lanlan_sfx_clips[index];
    size_t total = (size_t)(lanlan_sfx_end - lanlan_sfx_start);
    if (clip->offset > total || clip->bytes > total - clip->offset || (clip->bytes & 1u)
        || clip->bytes == 0) {
        ESP_LOGW(TAG, "audio clip %u is out of range", index);
        return;
    }
    esp_err_t err = s_audio_awake ? ESP_OK : bsp_audio_wake();
    if (err == ESP_OK) err = bsp_audio_set_format(16000, 16, 1);
    if (err != ESP_OK) {
        audio_failed("audio start", err);
        return;
    }
    s_audio_awake = true;
    s_pcm_offset = clip->offset;
    s_pcm_remaining = clip->bytes;
}

/* Feeds at most 512 aligned bytes per iteration and never allocates: the same
 * pattern the offline Korean application proved on this board. */
static void feed_audio(void) {
    int16_t pcm[256];
    size_t bytes = s_pcm_remaining < sizeof(pcm) ? s_pcm_remaining : sizeof(pcm);
    memcpy(pcm, lanlan_sfx_start + s_pcm_offset, bytes);
    esp_err_t err = bsp_audio_write(pcm, bytes);
    if (err != ESP_OK) {
        audio_failed("audio write", err);
        return;
    }
    s_last_audio_write = now_us();
    s_pcm_offset += (uint32_t)bytes;
    s_pcm_remaining -= (uint32_t)bytes;
    vTaskDelay(pdMS_TO_TICKS(1));
}

static void audio_idle(void) {
    if (s_pcm_remaining || !s_audio_awake) return;
    if (now_us() - s_last_audio_write < LANLAN_AUDIO_IDLE_SUSPEND_US) return;
    esp_err_t err = bsp_audio_sleep();
    s_audio_awake = false;
    if (err != ESP_OK) audio_failed("audio suspend", err);
}

/* -------------------------------------------------------------- render -- */

static const char *sync_state_text(lanlan_sync_state_t state) {
    switch (state) {
    case LANLAN_SYNC_STATE_OK: return LANLAN_STR_STATUS_OK;
    case LANLAN_SYNC_STATE_CREDENTIAL_REJECTED: return LANLAN_STR_STATUS_CREDENTIAL_REJECTED;
    case LANLAN_SYNC_STATE_OFFLINE: return LANLAN_STR_STATUS_OFFLINE;
    case LANLAN_SYNC_STATE_SERVER_ERROR: return LANLAN_STR_ERRORS_SERVER;
    case LANLAN_SYNC_STATE_STORAGE_ERROR: return LANLAN_STR_ERRORS_STORAGE;
    case LANLAN_SYNC_STATE_CONFIG_REQUIRED: return LANLAN_STR_SYNC_NEVER;
    case LANLAN_SYNC_STATE_WIFI_CONNECTING:
    case LANLAN_SYNC_STATE_TIME_PENDING:
    case LANLAN_SYNC_STATE_SYNCING: return LANLAN_STR_SYNC_JUST_NOW;
    default: return LANLAN_STR_SYNC_NEVER;
    }
}

static uint32_t sync_state_color(lanlan_sync_state_t state) {
    switch (state) {
    case LANLAN_SYNC_STATE_OK: return 0x3FA36Bu;
    case LANLAN_SYNC_STATE_CREDENTIAL_REJECTED: return 0xD4553Fu;
    case LANLAN_SYNC_STATE_STORAGE_ERROR: return 0xD4553Fu;
    case LANLAN_SYNC_STATE_OFFLINE:
    case LANLAN_SYNC_STATE_SERVER_ERROR: return 0xE0A030u;
    default: return 0xA79C90u;
    }
}

static void render(void) {
    if (!s_display_ok || !s_screen) return;
    if (!bsp_lvgl_lock(1000)) return;
    lanlan_ui_state_t state;
    memset(&state, 0, sizeof(state));
    lanlan_sync_state_t sync_state = lanlan_sync_state();
    char status_text[64];
    lanlan_view_sync_status_text(s_view, status_text, sizeof(status_text));
    lanlan_local_now_t local;
    int64_t epoch = 0;
    local_now(&local, &epoch);
    state.model = s_model;
    state.view = s_view;
    state.now = &local;
    state.caregivers = &s_caregivers;
    state.battery_percent = s_battery;
    state.status_text = status_text;
    state.status_color = sync_state_color(sync_state);
    state.status_detail = sync_state_text(sync_state);
    state.cache_rebuilt = s_cache_rebuilt;
    state.storage_limited = s_storage_limited;
    state.secure_url = !lanlan_url_is_insecure(s_config.base_url);
    state.banner_text = (now_us() < s_banner_until_us) ? LANLAN_STR_REMINDERS_DUE : NULL;
    lanlan_ui_render(s_screen, &state);
    bsp_lvgl_unlock();
    s_rendered_battery = s_battery;
    s_rendered_sync_state = (uint32_t)sync_state;
    s_render_dirty = false;
}

/* ------------------------------------------------------------- config -- */

static void config_apply_offset(void) {
    if (!s_pending_offset_valid) return;
    portENTER_CRITICAL(&s_clock_mux);
    int16_t offset = s_pending_offset;
    s_pending_offset_valid = false;
    portEXIT_CRITICAL(&s_clock_mux);
    if (offset == s_config.utc_offset_minutes) return;
    s_config.utc_offset_minutes = offset;
    s_model->utc_offset_min = offset;
    if (s_nvs_open) {
        xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
        (void)store_config_locked();
        (void)nvs_commit(s_nvs);
        xSemaphoreGive(s_nvs_mutex);
    }
    s_render_dirty = true;
}

static void config_sync_from_model(void) {
    bool changed = false;
    if (s_config.mute != (uint8_t)(s_model->globally_muted ? 1u : 0u)) {
        s_config.mute = s_model->globally_muted ? 1u : 0u;
        changed = true;
    }
    if (s_config.reminder_sound != (uint8_t)(s_model->reminder_sound_enabled ? 1u : 0u)) {
        s_config.reminder_sound = s_model->reminder_sound_enabled ? 1u : 0u;
        changed = true;
    }
    if (!changed || !s_nvs_open) return;
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    (void)store_config_locked();
    (void)nvs_commit(s_nvs);
    xSemaphoreGive(s_nvs_mutex);
}

/* ------------------------------------------------------------- actions -- */

static void handle_action(lanlan_action_t action) {
    switch (action) {
    case LANLAN_ACTION_COMPANION_PET:
        /* A pet action changes mood and plays a short clip; it must never
         * produce a record action. */
        (void)lanlan_ui_pet_invariant_held(s_model);
        play_clip(LANLAN_SFX_CHIRP);
        if (s_display_ok && bsp_lvgl_lock(1000)) {
            lanlan_ui_companion_react(LANLAN_CHARACTER_HAPPY);
            bsp_lvgl_unlock();
        }
        break;
    case LANLAN_ACTION_SETTINGS_REFRESH:
    case LANLAN_ACTION_STATUS_RETRY:
        lanlan_sync_request_now();
        break;
    case LANLAN_ACTION_RECORD_CREATE:
    case LANLAN_ACTION_RECORD_REVOKE:
        /* The passport never writes records: this is unreachable by design. */
        ESP_LOGE(TAG, "record write action returned by the model");
        break;
    default: break;
    }
}

static void handle_input(const input_event_t *event, bool display_was_off) {
    if (!s_ready) return;
    s_last_input_us = now_us();
    if (s_dimmed && s_display_ok) {
        bsp_display_backlight(LANLAN_BACKLIGHT_ON_PERCENT);
        s_dimmed = false;
    }
    if (display_was_off) {
        /* The first gesture after the screen turned off only wakes it. The long
         * press that wakes is consumed entirely, so releasing it cannot also
         * deliver a click. */
        if (s_display_ok) bsp_display_backlight(LANLAN_BACKLIGHT_ON_PERCENT);
        s_dark = false;
        lanlan_model_set_active(s_model, true);
        s_render_dirty = true;
        return;
    }
    if (event->event == BSP_BTN_DOUBLE && event->key == BSP_BTN_OK) {
        /* The documented companion controls are UP/DOWN and an OK click. A
         * double click is an extra: the bark frame plus its short clip. It is
         * still a pet interaction and never creates a record. */
        if (s_model->page != LANLAN_PAGE_COMPANION) return;
        (void)lanlan_ui_pet_invariant_held(s_model);
        play_clip(LANLAN_SFX_BARK);
        if (s_display_ok && bsp_lvgl_lock(1000)) {
            lanlan_ui_companion_bark();
            bsp_lvgl_unlock();
        }
        return;
    }
    lanlan_key_t key;
    switch (event->key) {
    case BSP_BTN_UP: key = LANLAN_KEY_UP; break;
    case BSP_BTN_DOWN: key = LANLAN_KEY_DOWN; break;
    case BSP_BTN_OK:
    default: key = event->event == BSP_BTN_LONG ? LANLAN_KEY_OK_LONG : LANLAN_KEY_OK_CLICK; break;
    }
    if (event->event != BSP_BTN_CLICK && event->event != BSP_BTN_LONG) return;
    /* A long press on UP/DOWN is not assigned by the design. */
    if (event->event == BSP_BTN_LONG && event->key != BSP_BTN_OK) return;

    lanlan_key_event_t model_event;
    model_event.key = key;
    model_event.woke_screen = display_was_off;
    s_pcm_remaining = 0;
    lanlan_action_t action = lanlan_model_handle_key(s_model, &model_event);
    handle_action(action);
    config_sync_from_model();
    s_render_dirty = true;
}

/* ---------------------------------------------------------- reminders -- */

static void reminder_poll(void) {
    int64_t now = now_us();
    if (now - s_last_reminder_us < LANLAN_REMINDER_POLL_US) return;
    s_last_reminder_us = now;
    bool trusted = clock_is_trusted();
    lanlan_local_now_t local;
    int64_t epoch = 0;
    local_now(&local, &epoch);
    lanlan_model_set_clock(s_model, &local, trusted);
    if (s_model->cache.reminder_count == 0) return;

    bool rung = false;
    int due = 0;
    for (uint32_t i = 0; i < s_model->cache.reminder_count; ++i) {
        lanlan_reminder_t *reminder = &s_model->cache.reminders[i];
        if (lanlan_reminder_due(reminder, &local, trusted) == LANLAN_REMINDER_DUE) ++due;
        /* The tone plays only when reminder sound is enabled, the global mute is
         * off and the clock is trusted; the visual due state above is
         * independent of all three. */
        if (!lanlan_reminder_should_ring(reminder, &local, trusted,
                                         s_model->reminder_sound_enabled,
                                         s_model->globally_muted)) {
            continue;
        }
        if (lanlan_reminder_mark_rung(reminder, &local)) {
            rung_record(reminder->id, reminder->last_rung_day);
            rung = true;
        }
    }
    if (due > 0) {
        s_banner_until_us = now + LANLAN_BANNER_US;
        s_render_dirty = true;
    }
    if (rung) {
        play_clip(LANLAN_SFX_REMINDER);
        if (s_nvs_open) {
            xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
            (void)store_rungs_locked();
            (void)nvs_commit(s_nvs);
            xSemaphoreGive(s_nvs_mutex);
        }
        s_banner_until_us = now + LANLAN_BANNER_US;
        s_render_dirty = true;
    }
}

/* ---------------------------------------------------------- backlight -- */

static void backlight_poll(void) {
    if (!s_display_ok) return;
    int64_t idle = now_us() - s_last_input_us;
    int64_t dim = (int64_t)s_config.dim_seconds * 1000000LL;
    int64_t off = (int64_t)s_config.screen_off_seconds * 1000000LL;
    if (!s_dark && idle >= off) {
        /* Not deep sleep: only the backlight is turned off, and the first
         * gesture restores it. No battery-life claim is made. */
        bsp_display_backlight(0);
        s_dark = true;
        lanlan_model_set_active(s_model, false);
        return;
    }
    if (!s_dimmed && idle >= dim) {
        bsp_display_backlight(LANLAN_BACKLIGHT_DIM_PERCENT);
        s_dimmed = true;
    }
}

/* ------------------------------------------------------------ workers -- */

static void on_key(bsp_btn_t key, bsp_btn_ev_t event, void *user) {
    (void)user;
    if (!s_ready) return;
    if (event != BSP_BTN_CLICK && event != BSP_BTN_LONG && event != BSP_BTN_DOUBLE) return;
    const input_event_t input = {key, event};
    /* The callback runs in the shared button timer task: enqueue only. */
    (void)xQueueSend(s_queue, &input, 0);
}

static void clear_cache_now(void) {
    /* `lanlan cache clear` only sets a flag; the reset and its NVS commit happen
     * here, on the worker that owns the cache. The cursor returns to 0 so the
     * next sync re-fetches everything from the service. */
    lanlan_cache_clear(&s_model->cache);
    s_cursor = 0;
    s_cache_rebuilt = false;
    /* s_work belongs to the sync worker, which fills it while a response is
     * being parsed outside the application lock, so the clear path uses its own
     * scratch buffer. It only exists while the command is being carried out. */
    uint8_t *image = heap_caps_malloc(LANLAN_CACHE_BLOB_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!image) {
        ESP_LOGE(TAG, "no memory to persist the cleared cache");
        s_storage_limited = true;
    } else {
        size_t length = lanlan_cache_encode(&s_model->cache, image, LANLAN_CACHE_BLOB_BYTES);
        if (length == (size_t)LANLAN_CACHE_BLOB_BYTES && s_nvs_open) {
            xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
            esp_err_t err = nvs_set_blob(s_nvs, LANLAN_NVS_KEY_CACHE, image, length);
            if (err == ESP_OK) err = nvs_commit(s_nvs);
            xSemaphoreGive(s_nvs_mutex);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "cache clear could not be persisted: %s", esp_err_to_name(err));
                s_storage_limited = true;
            }
        }
        heap_caps_free(image);
    }
    s_view->cache = s_model->cache;
    s_view->cursor = 0;
    s_render_dirty = true;
    lanlan_sync_request_now();
}

static void app_task(void *argument) {
    (void)argument;
    s_last_input_us = now_us();
    s_last_reminder_us = s_last_input_us;
    s_last_battery_us = s_last_input_us;
    for (;;) {
        input_event_t input;
        TickType_t wait = s_pcm_remaining ? 0 : pdMS_TO_TICKS(100);
        if (xQueueReceive(s_queue, &input, wait) == pdTRUE) {
            /* The sync worker publishes a batch into the same model, so key
             * handling takes the application lock as well. */
            xSemaphoreTake(s_app_mutex, portMAX_DELAY);
            handle_input(&input, s_dark);
            xSemaphoreGive(s_app_mutex);
        }
        if (s_pcm_remaining) feed_audio();
        audio_idle();

        if (s_clear_cache_requested) {
            s_clear_cache_requested = false;
            clear_cache_now();
        }
        xSemaphoreTake(s_app_mutex, portMAX_DELAY);
        config_apply_offset();
        refresh_view();
        reminder_poll();
        if (s_display_ok) {
            lanlan_sync_state_t sync_state = lanlan_sync_state();
            if (s_rendered_battery != s_battery || s_rendered_sync_state != (uint32_t)sync_state) {
                s_render_dirty = true;
            }
        }
        backlight_poll();
        int64_t now = now_us();
        if (now - s_last_battery_us > LANLAN_BATTERY_POLL_US) {
            s_last_battery_us = now;
            int soc = bsp_battery_soc();
            if (soc != s_battery) {
                s_battery = soc;
                s_render_dirty = true;
            }
        }
        if (s_render_dirty && !s_dark) render();
        xSemaphoreGive(s_app_mutex);
    }
}

/* ------------------------------------------------------------ console -- */

static int console_cfg(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: lanlan cfg ssid|pass|url|token|tz <value> | lanlan cfg show\n");
        return 1;
    }
    if (strcmp(argv[2], "show") == 0) {
        char secret[32];
        char zone[10];
        (void)lanlan_timezone_format(s_config.utc_offset_minutes, zone, sizeof(zone));
        lanlan_secret_status(s_cred.wifi_password, secret, sizeof(secret));
        printf("url=%s\n", s_config.base_url[0] ? s_config.base_url : "<unset>");
        printf("ssid=%s\n", s_cred.wifi_ssid[0] ? s_cred.wifi_ssid : "<unset>");
        printf("password=%s\n", secret);
        lanlan_secret_status(s_cred.device_token, secret, sizeof(secret));
        printf("token=%s\n", secret);
        printf("tz=%s\n", zone);
        printf("mute=%s\n", s_config.mute ? "on" : "off");
        printf("reminder_sound=%s\n", s_config.reminder_sound ? "on" : "off");
        printf("timers: dim=%us off=%us\n", (unsigned)s_config.dim_seconds,
               (unsigned)s_config.screen_off_seconds);
        printf("url_scheme=%s\n",
               lanlan_url_is_insecure(s_config.base_url)
                   ? "http (LAN development only, traffic is not encrypted)"
                   : "https");
        return 0;
    }
    if (argc < 4) {
        printf("error: a value is required\n");
        return 1;
    }
    if (argc > 4) {
        printf("error: the value must not contain spaces\n");
        return 1;
    }
    const char *value = argv[3];
    bool changed = false;
    if (strcmp(argv[2], "ssid") == 0) {
        snprintf(s_cred.wifi_ssid, sizeof(s_cred.wifi_ssid), "%s", value);
        changed = true;
    } else if (strcmp(argv[2], "pass") == 0) {
        snprintf(s_cred.wifi_password, sizeof(s_cred.wifi_password), "%s", value);
        changed = true;
    } else if (strcmp(argv[2], "token") == 0) {
        snprintf(s_cred.device_token, sizeof(s_cred.device_token), "%s", value);
        changed = true;
    } else if (strcmp(argv[2], "url") == 0) {
        char normalized[LANLAN_CONFIG_URL_BYTES];
        if (!lanlan_url_normalize(value, normalized, sizeof(normalized))) {
            printf("error: the url must start with https:// (or http:// for a LAN address)\n");
            return 1;
        }
        snprintf(s_config.base_url, sizeof(s_config.base_url), "%s", normalized);
        printf("note: the url was stored without a trailing slash\n");
        changed = true;
    } else if (strcmp(argv[2], "tz") == 0) {
        int16_t offset = 0;
        if (!lanlan_timezone_parse(value, &offset)) {
            printf("error: the timezone must look like +08:00 or -05:30\n");
            return 1;
        }
        s_config.utc_offset_minutes = offset;
        changed = true;
    } else {
        printf("error: unknown cfg key\n");
        return 1;
    }
    if (!changed) return 1;
    if (s_nvs_open) {
        xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
        esp_err_t err = store_config_locked();
        if (err == ESP_OK) err = store_cred_locked();
        if (err == ESP_OK) err = nvs_commit(s_nvs);
        xSemaphoreGive(s_nvs_mutex);
        if (err != ESP_OK) {
            printf("error: could not persist (%s)\n", esp_err_to_name(err));
            return 1;
        }
    }
    printf("ok: %s updated\n", argv[2]);
    /* Re-read by the network layer; this also clears a credential lockout. */
    lanlan_sync_set_credentials(&s_config, &s_cred);
    if (s_model) s_model->utc_offset_min = s_config.utc_offset_minutes;
    s_render_dirty = true;
    return 0;
}

static int console_status(void) {
    lanlan_sync_state_t state = lanlan_sync_state();
    uint32_t ok = 0;
    uint32_t failed = 0;
    lanlan_sync_counters(&ok, &failed);
    char stamp[LANLAN_RFC3339_MAX];
    int64_t last_ok = lanlan_sync_last_ok_epoch();
    if (last_ok > 0) {
        (void)lanlan_time_format_rfc3339(last_ok, stamp, sizeof(stamp));
    } else {
        snprintf(stamp, sizeof(stamp), "never");
    }
    char zone[10];
    (void)lanlan_timezone_format(s_config.utc_offset_minutes, zone, sizeof(zone));
    char secret[32];
    printf("state=%s error=%s\n", lanlan_sync_state_name(state),
           lanlan_sync_error_name(lanlan_sync_last_error()));
    printf("last_sync=%s cursor=%u ok=%u failed=%u backoff=%us\n", stamp,
           (unsigned)lanlan_sync_cursor(), (unsigned)ok, (unsigned)failed,
           (unsigned)lanlan_sync_backoff_seconds());
    printf("clock=%s tz=%s trust=%s\n", s_clock_synced ? "anchored" : "unset", zone,
           clock_is_trusted() ? "trusted" : "untrusted");
    printf("cache=%u records %u reminders storage=%s%s\n",
           (unsigned)(s_model ? s_model->cache.record_count : 0u),
           (unsigned)(s_model ? s_model->cache.reminder_count : 0u),
           s_nvs_open ? "open" : "unavailable", s_storage_limited ? " (limited)" : "");
    printf("cache_rebuilt_notice=%s\n", s_cache_rebuilt ? "yes" : "no");
    printf("heap free=%u largest_internal=%u\n", (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("ssid=%s\n", s_cred.wifi_ssid[0] ? s_cred.wifi_ssid : "<unset>");
    lanlan_secret_status(s_cred.device_token, secret, sizeof(secret));
    printf("token=%s url=%s\n", secret, s_config.base_url[0] ? s_config.base_url : "<unset>");
    printf("url_scheme=%s\n",
           lanlan_url_is_insecure(s_config.base_url) ? "http (LAN development only)" : "https");
    printf("display=%s audio=%s buttons=%s\n", s_display_ok ? "ok" : "failed",
           s_audio_ok ? "ok" : "failed", s_buttons_ok ? "ok" : "failed");
    return 0;
}

static int cmd_lanlan(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: lanlan cfg <ssid|pass|url|token|tz> <value> | lanlan cfg show\n");
        printf("       lanlan sync now | lanlan status | lanlan cache clear\n");
        return 1;
    }
    if (strcmp(argv[1], "cfg") == 0) return console_cfg(argc, argv);
    if (strcmp(argv[1], "sync") == 0) {
        if (argc < 3 || strcmp(argv[2], "now") != 0) {
            printf("usage: lanlan sync now\n");
            return 1;
        }
        lanlan_sync_request_now();
        printf("ok: sync requested\n");
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) return console_status();
    if (strcmp(argv[1], "cache") == 0) {
        if (argc < 3 || strcmp(argv[2], "clear") != 0) {
            printf("usage: lanlan cache clear\n");
            return 1;
        }
        s_clear_cache_requested = true;
        printf("ok: the cache will be cleared and resynchronized\n");
        return 0;
    }
    printf("error: unknown subcommand\n");
    return 1;
}

static void console_start(void) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "lanlan>";
    repl_config.max_cmdline_length = 160;
    repl_config.task_stack_size = 6144;
    esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console unavailable: %s", esp_err_to_name(err));
        return;
    }
    const esp_console_cmd_t command = {
        .command = "lanlan",
        .help = "Cyber Lanlan provisioning and diagnostics",
        .hint = NULL,
        .func = cmd_lanlan,
    };
    err = esp_console_cmd_register(&command);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console command registration failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console start failed: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------- storage -- */

static void storage_init(void) {
    s_nvs_mutex = xSemaphoreCreateMutex();
    s_app_mutex = xSemaphoreCreateMutex();
    if (!s_nvs_mutex || !s_app_mutex) return;
    /* Never erase unrelated NVS data when initialization fails. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs namespace needs formatting: %s", esp_err_to_name(err));
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage unavailable: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_open(LANLAN_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "namespace open failed: %s", esp_err_to_name(err));
        return;
    }
    s_nvs_open = true;
}

static bool buffers_alloc(void) {
    s_model = heap_caps_calloc(1, sizeof(lanlan_model_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_view = heap_caps_calloc(1, sizeof(lanlan_records_view_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_staging = heap_caps_calloc(1, sizeof(lanlan_cache_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_work = heap_caps_malloc(LANLAN_SYNC_WORK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_model || !s_view || !s_staging || !s_work) {
        ESP_LOGE(TAG, "application buffers do not fit: free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    lanlan_model_init(s_model);
    return true;
}

/* ---------------------------------------------------------------- app -- */

void app_main(void) {
    storage_init();
    load_config();
    if (!buffers_alloc()) return;
    s_model->globally_muted = s_config.mute != 0;
    s_model->reminder_sound_enabled = s_config.reminder_sound != 0;
    s_model->utc_offset_min = s_config.utc_offset_minutes;
    cache_load();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display initialization failed; continuing without a UI");
        s_display_ok = false;
    } else {
        s_display_ok = true;
    }
    s_audio_ok = bsp_audio_init() == ESP_OK;
    s_audio_awake = s_audio_ok;
    if (s_audio_ok) bsp_audio_set_volume(65);
    if (bsp_battery_init() == ESP_OK) s_battery = bsp_battery_soc();
    s_queue = xQueueCreate(LANLAN_INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "input queue allocation failed");
        return;
    }
    s_buttons_ok = bsp_button_init(on_key, NULL) == ESP_OK;

    if (s_display_ok) {
        if (bsp_lvgl_lock(1000)) {
            if (lanlan_ui_init(&s_screen) == ESP_OK) lv_screen_load(s_screen);
            bsp_lvgl_unlock();
        }
        bsp_display_backlight(LANLAN_BACKLIGHT_ON_PERCENT);
    }
    console_start();

    refresh_view();
    const lanlan_sync_hooks_t hooks = {
        .work = s_work,
        .work_bytes = LANLAN_SYNC_WORK_BYTES,
        .cursor = s_cursor,
        .apply = apply_sync_batch,
        .clock = clock_apply,
        .members = sync_members,
        .caregiver = sync_caregiver,
        .user = NULL,
    };
    esp_err_t sync_err = lanlan_sync_init(&s_config, &s_cred, &hooks);
    if (sync_err != ESP_OK) {
        ESP_LOGE(TAG, "sync worker unavailable: %s", esp_err_to_name(sync_err));
    }

    if (xTaskCreate(app_task, "lanlan_app", LANLAN_APP_TASK_STACK, NULL, LANLAN_APP_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "application task allocation failed");
        return;
    }
    s_ready = true;
    s_last_input_us = now_us();
    ESP_LOGI(TAG, "Cyber Lanlan ready; free heap=%u largest internal=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

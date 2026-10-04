/* Cyber Lanlan networking worker. See lanlan_sync.h for the contract.
 *
 * Task model: exactly one worker task owns Wi-Fi, SNTP, the HTTP client and the
 * JSON parse. It never touches LVGL and never blocks on the application lock
 * while holding the network open. Failed attempts keep the previous cache and
 * cursor, and back off exponentially. A 401 stops the retry loop until new
 * credentials arrive. */

#include "lanlan_sync.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lanlan_time.h"

static const char *TAG = "lanlan_sync";

/* -------------------------------------------------------------- constants -- */

/* The worker stack must hold the two large by-value locals the pure-logic cache
 * module uses (lanlan_cache_encode() and lanlan_cache_apply_batch() each copy a
 * whole cache onto the stack, at most LANLAN_CACHE_MAX_LIVE_BYTES = 8 KB by the
 * module's own budget assert). They are called sequentially, and the TLS session
 * is already closed when they run, so 20 KB covers the deepest frame plus the
 * drop/revoked reports and the HTTP client locals. LANLAN_SYNC_WORK_BYTES covers
 * the batch and the serialized image, which live on the heap. */
#define LANLAN_SYNC_TASK_STACK (20 * 1024)
#define LANLAN_SYNC_TASK_PRIORITY 4

#define LANLAN_SYNC_WIFI_ATTEMPTS 6
#define LANLAN_SYNC_WIFI_WAIT_MS 15000
#define LANLAN_SYNC_IDLE_INTERVAL_US (120 * 1000000LL)
#define LANLAN_SYNC_BACKOFF_BASE_SECONDS 5u
#define LANLAN_SYNC_BACKOFF_MAX_SECONDS 600u
#define LANLAN_SYNC_RETRY_AFTER_US (60 * 1000000LL)

#define LANLAN_SYNC_HTTP_TIMEOUT_MS 12000
#define LANLAN_SYNC_SNTP_WAIT_MS 15000
#define LANLAN_SYNC_RESPONSE_MAX (16 * 1024)
#define LANLAN_SYNC_URL_MAX 256
#define LANLAN_SYNC_AUTH_MAX 160
#define LANLAN_SYNC_PAGE_LIMIT 8
#define LANLAN_SYNC_MAX_PAGES 12

/* The project's sdkconfig keeps CONFIG_LWIP_SNTP_MAX_SERVERS at its default of
 * one, so a single pool server is configured. SNTP is an optimisation: the
 * service's `server_time` also anchors the clock, and neither is trusted until
 * lanlan_time_clock_trusted() accepts the resulting epoch. */
#define LANLAN_SNTP_SERVER "pool.ntp.org"

#define LANLAN_SYNC_BIT_CONNECTED BIT0
#define LANLAN_SYNC_BIT_DISCONNECTED BIT1
#define LANLAN_SYNC_BIT_TRIGGER BIT2

/* ------------------------------------------------------------ static state -- */

static lanlan_sync_hooks_t s_hooks;
static lanlan_config_t s_config;
static lanlan_cred_t s_cred;
static SemaphoreHandle_t s_config_lock;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static bool s_started;
static bool s_wifi_ready;
static bool s_wifi_started;
static bool s_netif_ready;
static bool s_sntp_ready;
static char s_active_ssid[LANLAN_CRED_SSID_BYTES];
static esp_netif_t *s_netif;
static int16_t s_offset_minutes;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile lanlan_sync_state_t s_state = LANLAN_SYNC_STATE_IDLE;
static volatile lanlan_sync_error_t s_error = LANLAN_SYNC_ERROR_NONE;
static volatile uint32_t s_cursor;
static volatile int64_t s_last_ok_epoch;
static volatile uint32_t s_backoff_seconds;
static volatile bool s_credential_rejected;
static volatile uint32_t s_ok_count;
static volatile uint32_t s_fail_count;

/* One response buffer for the whole worker: bounded by LANLAN_SYNC_RESPONSE_MAX
 * instead of trusting the server's Content-Length. */
static char s_body[LANLAN_SYNC_RESPONSE_MAX];
static char s_url[LANLAN_SYNC_URL_MAX];
static char s_auth[LANLAN_SYNC_AUTH_MAX];
static char s_post[256];
/* Copy of the configured base URL, so the request path never takes the config
 * lock while a connection is open. */
static char s_base_url[LANLAN_CONFIG_URL_BYTES];
/* The snapshot's reminder array is only sent on the first page, so it is held
 * here while the newest record page is fetched. */
static lanlan_reminder_t s_head_reminders[LANLAN_CACHE_REMINDER_CAPACITY];
static int32_t s_head_reminder_count;

/* ---------------------------------------------------------- small helpers -- */

static void copy_str(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    size_t len = src ? strlen(src) : 0;
    if (len >= dst_size) len = dst_size - 1;
    if (len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

static void put16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t get16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static void state_set(lanlan_sync_state_t state, lanlan_sync_error_t error) {
    portENTER_CRITICAL(&s_state_mux);
    s_state = state;
    s_error = error;
    portEXIT_CRITICAL(&s_state_mux);
}

/* ---------------------------------------------------- config blob codec -- */

_Static_assert(sizeof(lanlan_config_t) <= LANLAN_CONFIG_BLOB_BYTES,
               "lanlan_config_t must fit the cfg_v1 blob");
_Static_assert(sizeof(lanlan_cred_t) <= LANLAN_CRED_BLOB_BYTES,
               "lanlan_cred_t must fit the cred_v1 blob");

#define LANLAN_CONFIG_WIRE_BYTES (12 + LANLAN_CONFIG_URL_BYTES)
#define LANLAN_CRED_WIRE_BYTES \
    (4 + LANLAN_CRED_TOKEN_BYTES + LANLAN_CRED_DEVICE_ID_BYTES + LANLAN_CRED_SSID_BYTES \
     + LANLAN_CRED_PASS_BYTES)

void lanlan_config_defaults(lanlan_config_t *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->version = (uint16_t)LANLAN_CONFIG_FORMAT_VERSION;
    config->utc_offset_minutes = 8 * 60;
    config->mute = 0;
    config->reminder_sound = 0;
    config->dim_seconds = LANLAN_DIM_DEFAULT_SECONDS;
    config->screen_off_seconds = LANLAN_SCREEN_OFF_DEFAULT_SECONDS;
}

void lanlan_cred_defaults(lanlan_cred_t *cred) {
    if (!cred) return;
    memset(cred, 0, sizeof(*cred));
    cred->version = (uint16_t)LANLAN_CRED_FORMAT_VERSION;
}

bool lanlan_config_clamp(lanlan_config_t *config) {
    if (!config) return false;
    bool changed = false;
    if (config->utc_offset_minutes < LANLAN_TIME_OFFSET_MIN
        || config->utc_offset_minutes > LANLAN_TIME_OFFSET_MAX) {
        config->utc_offset_minutes = 0;
        changed = true;
    }
    if (config->dim_seconds < LANLAN_DIM_MIN_SECONDS || config->dim_seconds > LANLAN_DIM_MAX_SECONDS) {
        config->dim_seconds = LANLAN_DIM_DEFAULT_SECONDS;
        changed = true;
    }
    if (config->screen_off_seconds < LANLAN_SCREEN_OFF_MIN_SECONDS
        || config->screen_off_seconds > LANLAN_SCREEN_OFF_MAX_SECONDS) {
        config->screen_off_seconds = LANLAN_SCREEN_OFF_DEFAULT_SECONDS;
        changed = true;
    }
    if (config->screen_off_seconds <= config->dim_seconds) {
        config->screen_off_seconds = (uint16_t)(config->dim_seconds + 60);
        if (config->screen_off_seconds > LANLAN_SCREEN_OFF_MAX_SECONDS) {
            config->screen_off_seconds = LANLAN_SCREEN_OFF_MAX_SECONDS;
        }
        changed = true;
    }
    if (config->mute > 1) {
        config->mute = 0;
        changed = true;
    }
    if (config->reminder_sound > 1) {
        config->reminder_sound = 0;
        changed = true;
    }
    return changed;
}

bool lanlan_config_sanitize(lanlan_config_t *config) {
    if (!config) return false;
    config->base_url[LANLAN_CONFIG_URL_BYTES - 1] = '\0';
    bool changed = lanlan_config_clamp(config);
    if (config->version != (uint16_t)LANLAN_CONFIG_FORMAT_VERSION) {
        config->version = (uint16_t)LANLAN_CONFIG_FORMAT_VERSION;
        changed = true;
    }
    return changed;
}

bool lanlan_cred_sanitize(lanlan_cred_t *cred) {
    if (!cred) return false;
    cred->device_token[LANLAN_CRED_TOKEN_BYTES - 1] = '\0';
    cred->device_id[LANLAN_CRED_DEVICE_ID_BYTES - 1] = '\0';
    cred->wifi_ssid[LANLAN_CRED_SSID_BYTES - 1] = '\0';
    cred->wifi_password[LANLAN_CRED_PASS_BYTES - 1] = '\0';
    if (cred->version != (uint16_t)LANLAN_CRED_FORMAT_VERSION) {
        cred->version = (uint16_t)LANLAN_CRED_FORMAT_VERSION;
        return true;
    }
    return false;
}

size_t lanlan_config_encode(const lanlan_config_t *config, uint8_t *out, size_t out_size) {
    if (!config || !out || out_size < LANLAN_CONFIG_WIRE_BYTES) return 0;
    memset(out, 0, LANLAN_CONFIG_WIRE_BYTES);
    put16(out, config->version);
    put16(out + 2, (uint16_t)config->utc_offset_minutes);
    out[4] = config->mute ? 1u : 0u;
    out[5] = config->reminder_sound ? 1u : 0u;
    put16(out + 6, config->dim_seconds);
    put16(out + 8, config->screen_off_seconds);
    size_t url_len = strlen(config->base_url);
    if (url_len > LANLAN_CONFIG_URL_BYTES - 1) url_len = LANLAN_CONFIG_URL_BYTES - 1;
    memcpy(out + 12, config->base_url, url_len);
    return LANLAN_CONFIG_WIRE_BYTES;
}

bool lanlan_config_decode(const uint8_t *data, size_t size, lanlan_config_t *out) {
    if (!data || !out || size < LANLAN_CONFIG_WIRE_BYTES) return false;
    if (get16(data) != (uint16_t)LANLAN_CONFIG_FORMAT_VERSION) return false;
    lanlan_config_t value;
    memset(&value, 0, sizeof(value));
    value.version = get16(data);
    value.utc_offset_minutes = (int16_t)get16(data + 2);
    value.mute = data[4] ? 1u : 0u;
    value.reminder_sound = data[5] ? 1u : 0u;
    value.dim_seconds = get16(data + 6);
    value.screen_off_seconds = get16(data + 8);
    memcpy(value.base_url, data + 12, LANLAN_CONFIG_URL_BYTES);
    value.base_url[LANLAN_CONFIG_URL_BYTES - 1] = '\0';
    lanlan_config_sanitize(&value);
    *out = value;
    return true;
}

size_t lanlan_cred_encode(const lanlan_cred_t *cred, uint8_t *out, size_t out_size) {
    if (!cred || !out || out_size < LANLAN_CRED_WIRE_BYTES) return 0;
    memset(out, 0, LANLAN_CRED_WIRE_BYTES);
    put16(out, cred->version);
    put16(out + 2, 0);
    size_t offset = 4;
    copy_str((char *)out + offset, LANLAN_CRED_TOKEN_BYTES, cred->device_token);
    offset += LANLAN_CRED_TOKEN_BYTES;
    copy_str((char *)out + offset, LANLAN_CRED_DEVICE_ID_BYTES, cred->device_id);
    offset += LANLAN_CRED_DEVICE_ID_BYTES;
    copy_str((char *)out + offset, LANLAN_CRED_SSID_BYTES, cred->wifi_ssid);
    offset += LANLAN_CRED_SSID_BYTES;
    copy_str((char *)out + offset, LANLAN_CRED_PASS_BYTES, cred->wifi_password);
    return LANLAN_CRED_WIRE_BYTES;
}

bool lanlan_cred_decode(const uint8_t *data, size_t size, lanlan_cred_t *out) {
    if (!data || !out || size < LANLAN_CRED_WIRE_BYTES) return false;
    if (get16(data) != (uint16_t)LANLAN_CRED_FORMAT_VERSION) return false;
    lanlan_cred_t value;
    memset(&value, 0, sizeof(value));
    value.version = get16(data);
    size_t offset = 4;
    copy_str(value.device_token, LANLAN_CRED_TOKEN_BYTES, (const char *)data + offset);
    offset += LANLAN_CRED_TOKEN_BYTES;
    copy_str(value.device_id, LANLAN_CRED_DEVICE_ID_BYTES, (const char *)data + offset);
    offset += LANLAN_CRED_DEVICE_ID_BYTES;
    copy_str(value.wifi_ssid, LANLAN_CRED_SSID_BYTES, (const char *)data + offset);
    offset += LANLAN_CRED_SSID_BYTES;
    copy_str(value.wifi_password, LANLAN_CRED_PASS_BYTES, (const char *)data + offset);
    lanlan_cred_sanitize(&value);
    *out = value;
    return true;
}

bool lanlan_config_ready(const lanlan_config_t *config, const lanlan_cred_t *cred) {
    if (!config || !cred) return false;
    return config->base_url[0] != '\0' && cred->device_token[0] != '\0' && cred->wifi_ssid[0] != '\0';
}

void lanlan_secret_status(const char *secret, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t len = secret ? strlen(secret) : 0;
    if (len == 0) {
        snprintf(out, out_size, "<unset>");
    } else {
        snprintf(out, out_size, "<set, %u bytes>", (unsigned)len);
    }
    out[out_size - 1] = '\0';
}

bool lanlan_url_normalize(const char *input, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!input) return false;
    while (*input == ' ') ++input;
    if (strncmp(input, "https://", 8) != 0 && strncmp(input, "http://", 7) != 0) return false;
    size_t len = strlen(input);
    while (len > 0 && input[len - 1] == '/') --len;
    if (len == 0 || len >= out_size) return false;
    /* Reject embedded credentials: user:pass@host would end up in logs. */
    for (size_t i = 0; i < len; ++i) {
        if (input[i] == '@') return false;
    }
    memcpy(out, input, len);
    out[len] = '\0';
    return true;
}

bool lanlan_url_is_insecure(const char *base_url) {
    return base_url != NULL && strncmp(base_url, "http://", 7) == 0;
}

bool lanlan_timezone_parse(const char *text, int16_t *offset_minutes) {
    if (!text || !offset_minutes) return false;
    if (strcmp(text, "Z") == 0 || strcmp(text, "z") == 0 || strcmp(text, "+00:00") == 0) {
        *offset_minutes = 0;
        return true;
    }
    char sign = text[0];
    if (sign != '+' && sign != '-') return false;
    if (strlen(text) != 6 || text[3] != ':') return false;
    int hour = (text[1] - '0') * 10 + (text[2] - '0');
    int minute = (text[4] - '0') * 10 + (text[5] - '0');
    if (text[1] < '0' || text[1] > '9' || text[2] < '0' || text[2] > '9' || text[4] < '0'
        || text[4] > '9' || text[5] < '0' || text[5] > '9') {
        return false;
    }
    if (hour > 14 || minute > 59) return false;
    int total = hour * 60 + minute;
    if (sign == '-') total = -total;
    if (total < LANLAN_TIME_OFFSET_MIN || total > LANLAN_TIME_OFFSET_MAX) return false;
    *offset_minutes = (int16_t)total;
    return true;
}

size_t lanlan_timezone_format(int16_t offset_minutes, char *out, size_t out_size) {
    if (!out || out_size < 7) return 0;
    int total = offset_minutes;
    char sign = '+';
    if (total < 0) {
        sign = '-';
        total = -total;
    }
    int written = snprintf(out, out_size, "%c%02d:%02d", sign, total / 60, total % 60);
    return written < 0 ? 0 : (size_t)written;
}

/* ------------------------------------------------------------- accessors -- */

const char *lanlan_sync_state_name(lanlan_sync_state_t state) {
    switch (state) {
    case LANLAN_SYNC_STATE_IDLE: return "idle";
    case LANLAN_SYNC_STATE_CONFIG_REQUIRED: return "config_required";
    case LANLAN_SYNC_STATE_WIFI_CONNECTING: return "wifi_connecting";
    case LANLAN_SYNC_STATE_TIME_PENDING: return "time_pending";
    case LANLAN_SYNC_STATE_SYNCING: return "syncing";
    case LANLAN_SYNC_STATE_OK: return "ok";
    case LANLAN_SYNC_STATE_OFFLINE: return "offline";
    case LANLAN_SYNC_STATE_CREDENTIAL_REJECTED: return "credential_rejected";
    case LANLAN_SYNC_STATE_SERVER_ERROR: return "server_error";
    case LANLAN_SYNC_STATE_STORAGE_ERROR: return "storage_error";
    default: return "unknown";
    }
}

const char *lanlan_sync_error_name(lanlan_sync_error_t error) {
    switch (error) {
    case LANLAN_SYNC_ERROR_NONE: return "none";
    case LANLAN_SYNC_ERROR_NO_CREDENTIALS: return "no_credentials";
    case LANLAN_SYNC_ERROR_WIFI: return "wifi";
    case LANLAN_SYNC_ERROR_DNS: return "dns";
    case LANLAN_SYNC_ERROR_CONNECT: return "connect";
    case LANLAN_SYNC_ERROR_TIMEOUT: return "timeout";
    case LANLAN_SYNC_ERROR_TRANSPORT: return "transport";
    case LANLAN_SYNC_ERROR_HTTP_STATUS: return "http_status";
    case LANLAN_SYNC_ERROR_MALFORMED: return "malformed";
    case LANLAN_SYNC_ERROR_OVERSIZE: return "oversize";
    case LANLAN_SYNC_ERROR_STORAGE: return "storage";
    case LANLAN_SYNC_ERROR_INTERNAL: return "internal";
    default: return "unknown";
    }
}

lanlan_sync_state_t lanlan_sync_state(void) { return s_state; }
lanlan_sync_error_t lanlan_sync_last_error(void) { return s_error; }
uint32_t lanlan_sync_backoff_seconds(void) { return s_backoff_seconds; }
bool lanlan_sync_credential_rejected(void) { return s_credential_rejected; }
uint32_t lanlan_sync_cursor(void) { return s_cursor; }

int64_t lanlan_sync_last_ok_epoch(void) {
    portENTER_CRITICAL(&s_state_mux);
    int64_t value = s_last_ok_epoch;
    portEXIT_CRITICAL(&s_state_mux);
    return value;
}

void lanlan_sync_counters(uint32_t *ok, uint32_t *failed) {
    if (ok) *ok = s_ok_count;
    if (failed) *failed = s_fail_count;
}

/* ---------------------------------------------------------------- Wi-Fi -- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Log the reason code only: it contains no credential material. */
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "wifi disconnected, reason=%d", event ? (int)event->reason : -1);
        xEventGroupClearBits(s_events, LANLAN_SYNC_BIT_CONNECTED);
        xEventGroupSetBits(s_events, LANLAN_SYNC_BIT_DISCONNECTED);
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        /* The address itself is not a secret, but it is not needed either. */
        ESP_LOGI(TAG, "wifi connected, got ip");
        (void)event;
        xEventGroupClearBits(s_events, LANLAN_SYNC_BIT_DISCONNECTED);
        xEventGroupSetBits(s_events, LANLAN_SYNC_BIT_CONNECTED);
    }
}

static esp_err_t wifi_start(const lanlan_cred_t *cred) {
    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) return ESP_ERR_NO_MEM;
        s_netif_ready = true;
    }
    if (!s_wifi_ready) {
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        /* Dynamic buffers keep internal RAM for TLS; calibration data is not
         * stored in NVS because the lanlan namespace is space constrained. */
        init.nvs_enable = 0;
        esp_err_t err = esp_wifi_init(&init);
        if (err != ESP_OK) return err;
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
                                                  NULL, NULL);
        if (err != ESP_OK) return err;
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler,
                                                  NULL, NULL);
        if (err != ESP_OK) return err;
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK) return err;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) return err;
        s_wifi_ready = true;
    }
    wifi_config_t station;
    memset(&station, 0, sizeof(station));
    copy_str((char *)station.sta.ssid, sizeof(station.sta.ssid), cred->wifi_ssid);
    copy_str((char *)station.sta.password, sizeof(station.sta.password), cred->wifi_password);
    station.sta.threshold.authmode = WIFI_AUTH_OPEN;
    station.sta.pmf_cfg.capable = true;
    station.sta.pmf_cfg.required = false;
    /* Re-applying the credentials on every cycle would drop the link; only do
     * it when provisioning changed them. */
    if (strcmp(s_active_ssid, cred->wifi_ssid) != 0) {
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station);
        if (err != ESP_OK) return err;
        copy_str(s_active_ssid, sizeof(s_active_ssid), cred->wifi_ssid);
        if (s_wifi_started) {
            xEventGroupClearBits(s_events, LANLAN_SYNC_BIT_CONNECTED);
            (void)esp_wifi_disconnect();
        }
    }
    if (!s_wifi_started) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;
        /* No modem sleep: a sync cycle must not wait for a DTIM window. */
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        s_wifi_started = true;
    }
    return ESP_OK;
}

/* Returns true when an IP address is available. Retries with a linear wait; the
 * exponential application backoff is applied by the caller. */
static bool wifi_connect(void) {
    if (xEventGroupGetBits(s_events) & LANLAN_SYNC_BIT_CONNECTED) return true;
    for (int attempt = 0; attempt < LANLAN_SYNC_WIFI_ATTEMPTS; ++attempt) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "wifi connect call failed: %s", esp_err_to_name(err));
            return false;
        }
        EventBits_t bits = xEventGroupWaitBits(s_events,
                                               LANLAN_SYNC_BIT_CONNECTED | LANLAN_SYNC_BIT_DISCONNECTED,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(LANLAN_SYNC_WIFI_WAIT_MS));
        if (bits & LANLAN_SYNC_BIT_CONNECTED) return true;
    }
    return false;
}

static bool sntp_sync(void) {
    if (!s_sntp_ready) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(LANLAN_SNTP_SERVER);
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(err));
            return false;
        }
        s_sntp_ready = true;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(LANLAN_SYNC_SNTP_WAIT_MS)) != ESP_OK) return false;
    time_t now = time(NULL);
    if ((int64_t)now < LANLAN_TIME_TRUST_FLOOR_EPOCH) return false;
    if (s_hooks.clock) {
        s_hooks.clock((int64_t)now, s_offset_minutes,
                      lanlan_time_clock_trusted(true, (int64_t)now), s_hooks.user);
    }
    ESP_LOGI(TAG, "sntp synchronized: epoch=%lld", (long long)now);
    return true;
}

/* ------------------------------------------------------------ HTTP client -- */

typedef struct {
    int status;
    size_t length;
    bool oversize;
    bool complete;
} http_reply_t;

static void http_configure(esp_http_client_config_t *config, const char *url) {
    memset(config, 0, sizeof(*config));
    config->url = url;
    config->timeout_ms = LANLAN_SYNC_HTTP_TIMEOUT_MS;
    config->buffer_size = 1024;
    config->disable_auto_redirect = true;
    config->keep_alive_enable = false;
    if (!lanlan_url_is_insecure(url)) config->crt_bundle_attach = esp_crt_bundle_attach;
}

static lanlan_sync_error_t http_classify(esp_http_client_handle_t client, esp_err_t err) {
    int tls_error = 0;
    int tls_flags = 0;
    if (esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags) == ESP_OK
        && tls_error != 0) {
        (void)tls_flags;
        if (tls_error == ESP_TLS_ERR_SSL_TIMEOUT
            || tls_error == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT
            || tls_error == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT) {
            return LANLAN_SYNC_ERROR_TIMEOUT;
        }
        if (tls_error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) return LANLAN_SYNC_ERROR_DNS;
        if (tls_error == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) return LANLAN_SYNC_ERROR_CONNECT;
        return LANLAN_SYNC_ERROR_TRANSPORT;
    }
    int err_no = esp_http_client_get_errno(client);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_READ_TIMEOUT || err_no == ETIMEDOUT) {
        return LANLAN_SYNC_ERROR_TIMEOUT;
    }
    if (err_no == ECONNREFUSED || err_no == ECONNRESET || err_no == EHOSTUNREACH
        || err_no == ENETUNREACH) {
        return LANLAN_SYNC_ERROR_CONNECT;
    }
    if (err_no == EAI_FAIL || err_no == EAI_NONAME || err_no == EAI_MEMORY || err_no == EAI_FAMILY
        || err_no == HOST_NOT_FOUND) {
        return LANLAN_SYNC_ERROR_DNS;
    }
    /* A non-zero errno that is none of the above came from the resolution or
     * setup path, which is reported as a DNS failure. */
    if (err_no != 0) return LANLAN_SYNC_ERROR_DNS;
    return LANLAN_SYNC_ERROR_TRANSPORT;
}

/* Runs one request. The body is read through a hard cap: Content-Length is used
 * only as an early hint, never as a trust decision. */
static esp_err_t http_run(const char *path, const char *post_body, size_t post_len,
                          char *body, size_t body_cap, http_reply_t *reply,
                          lanlan_sync_error_t *error_out) {
    if (strlen(s_base_url) + strlen(path) + 1 > sizeof(s_url)) {
        *error_out = LANLAN_SYNC_ERROR_INTERNAL;
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(s_url, sizeof(s_url), "%s%s", s_base_url, path);

    esp_http_client_config_t config;
    http_configure(&config, s_url);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        *error_out = LANLAN_SYNC_ERROR_INTERNAL;
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_auth, sizeof(s_auth), "Bearer %s", s_cred.device_token);
    esp_http_client_set_header(client, "Authorization", s_auth);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, post_body ? (int)post_len : 0);
    if (err != ESP_OK) {
        *error_out = http_classify(client, err);
        ESP_LOGW(TAG, "request open failed: %s (%s)", esp_err_to_name(err),
                 lanlan_sync_error_name(*error_out));
        esp_http_client_cleanup(client);
        return err;
    }
    if (post_body) {
        int written = esp_http_client_write(client, post_body, (int)post_len);
        if (written < 0 || (size_t)written != post_len) {
            *error_out = LANLAN_SYNC_ERROR_TRANSPORT;
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    reply->status = esp_http_client_get_status_code(client);
    reply->length = 0;
    reply->oversize = false;
    reply->complete = false;

    if (content_length > (int64_t)(body_cap - 1)) {
        /* The server announced more than the cap; never buffer it. */
        reply->oversize = true;
        esp_http_client_cleanup(client);
        *error_out = LANLAN_SYNC_ERROR_OVERSIZE;
        return ESP_OK;
    }
    for (;;) {
        if (reply->length >= body_cap - 1) {
            reply->oversize = true;
            break;
        }
        size_t room = body_cap - 1 - reply->length;
        if (room > 1024) room = 1024;
        int read_len = esp_http_client_read(client, body + reply->length, (int)room);
        if (read_len < 0) {
            *error_out = http_classify(client, ESP_FAIL);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (read_len == 0) break;
        reply->length += (size_t)read_len;
    }
    body[reply->length] = '\0';
    reply->complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

/* Best-effort diagnostic POST; a failure is deliberately silent. */
static void http_ack(uint32_t cursor) {
    char stamp[LANLAN_RFC3339_MAX];
    time_t now = time(NULL);
    /* Reporting a clock we do not trust would poison the server-side skew
     * diagnostics, so an unsynchronized device simply does not ack. */
    if (!lanlan_time_clock_trusted(true, (int64_t)now)) return;
    if (lanlan_time_format_rfc3339((int64_t)now, stamp, sizeof(stamp)) == 0) return;
    int written = snprintf(s_post, sizeof(s_post), "{\"cursor\":%u,\"applied_at\":\"%s\"}",
                           (unsigned)cursor, stamp);
    if (written <= 0 || (size_t)written >= sizeof(s_post)) return;
    http_reply_t reply;
    lanlan_sync_error_t error = LANLAN_SYNC_ERROR_NONE;
    (void)http_run("/api/v1/sync/ack", s_post, (size_t)written, s_body, sizeof(s_body), &reply,
                   &error);
}

/* ------------------------------------------------------------ JSON parse -- */

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_uuid(const char *text, uint8_t out[LANLAN_ID_BYTES]) {
    if (!text || strlen(text) != 36) return false;
    size_t index = 0;
    size_t produced = 0;
    while (index < 36) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') return false;
            ++index;
            continue;
        }
        if (index + 1 >= 36 || produced >= LANLAN_ID_BYTES) return false;
        int high = hex_digit(text[index]);
        int low = hex_digit(text[index + 1]);
        if (high < 0 || low < 0) return false;
        out[produced++] = (uint8_t)((high << 4) | low);
        index += 2;
    }
    return produced == LANLAN_ID_BYTES && !lanlan_record_id_is_zero(out);
}

static bool json_string(const cJSON *object, const char *name, const char **out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item) {
        *out = NULL;
        return true;
    }
    if (cJSON_IsNull(item)) {
        *out = NULL;
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    *out = item->valuestring;
    return true;
}

static bool json_number(const cJSON *object, const char *name, double *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item || cJSON_IsNull(item)) return false;
    if (!cJSON_IsNumber(item)) return false;
    *out = cJSON_GetNumberValue(item);
    return true;
}

static lanlan_category_t parse_category(const char *text) {
    if (!text) return LANLAN_CAT_COUNT;
    if (strcmp(text, "meal") == 0) return LANLAN_CAT_MEAL;
    if (strcmp(text, "water") == 0) return LANLAN_CAT_WATER;
    if (strcmp(text, "care") == 0) return LANLAN_CAT_CARE;
    if (strcmp(text, "cleaning") == 0) return LANLAN_CAT_CLEANING;
    if (strcmp(text, "walk") == 0) return LANLAN_CAT_WALK;
    if (strcmp(text, "other") == 0) return LANLAN_CAT_OTHER;
    return LANLAN_CAT_COUNT;
}

static lanlan_subitem_t parse_subitem(lanlan_category_t category, const char *text) {
    if (!text || text[0] == '\0') return LANLAN_SUB_NONE;
    if (category == LANLAN_CAT_CARE) {
        if (strcmp(text, "bath") == 0) return LANLAN_SUB_BATH;
        if (strcmp(text, "grooming") == 0) return LANLAN_SUB_GROOMING;
        if (strcmp(text, "teeth") == 0) return LANLAN_SUB_TEETH;
        if (strcmp(text, "comb") == 0) return LANLAN_SUB_COMB;
        if (strcmp(text, "other") == 0) return LANLAN_SUB_CARE_OTHER;
        return LANLAN_SUB_COUNT;
    }
    if (category == LANLAN_CAT_CLEANING) {
        if (strcmp(text, "ear") == 0) return LANLAN_SUB_EAR;
        if (strcmp(text, "paw") == 0) return LANLAN_SUB_PAW;
        if (strcmp(text, "pad") == 0) return LANLAN_SUB_PAD;
        if (strcmp(text, "litter") == 0) return LANLAN_SUB_LITTER;
        if (strcmp(text, "other") == 0) return LANLAN_SUB_CLEANING_OTHER;
        return LANLAN_SUB_COUNT;
    }
    return LANLAN_SUB_COUNT;
}

static lanlan_unit_t parse_unit(const char *text) {
    if (!text || text[0] == '\0') return LANLAN_UNIT_NONE;
    if (strcmp(text, "g") == 0) return LANLAN_UNIT_G;
    if (strcmp(text, "ml") == 0) return LANLAN_UNIT_ML;
    if (strcmp(text, "scoop") == 0) return LANLAN_UNIT_SCOOP;
    if (strcmp(text, "cup") == 0) return LANLAN_UNIT_CUP;
    if (strcmp(text, "piece") == 0) return LANLAN_UNIT_PIECE;
    if (strcmp(text, "bag") == 0) return LANLAN_UNIT_BAG;
    if (strcmp(text, "bowl") == 0) return LANLAN_UNIT_BOWL;
    return LANLAN_UNIT_COUNT;
}

/* Copies a server string into a fixed field: sanitized, UTF-8 safe, bounded by
 * characters, and always terminated. A missing value leaves an empty string.
 * The sanitize buffer is a fixed local; the server bounds the source at 200
 * characters and anything longer is cut on both steps. */
#define LANLAN_SYNC_TEXT_SCRATCH 192
static void copy_text_field(const char *source, char *destination, size_t destination_size,
                            size_t max_chars) {
    if (!destination || destination_size == 0) return;
    destination[0] = '\0';
    if (!source) return;
    char sanitized[LANLAN_SYNC_TEXT_SCRATCH];
    (void)lanlan_text_sanitize(source, sanitized, sizeof(sanitized));
    (void)lanlan_utf8_copy_chars(sanitized, destination, destination_size, max_chars);
}

/* The record struct stores a caregiver index, not an id, and the validator
 * rejects anything above 1. The service sends user ids, so the glue layer owns
 * a two-slot table; the hook must always answer 0 or 1. */
static uint8_t caregiver_index(const cJSON *object, const char *name) {
    const char *text = NULL;
    if (!json_string(object, name, &text) || !text) return 0;
    uint8_t id[LANLAN_ID_BYTES];
    if (!parse_uuid(text, id)) return 0;
    bool is_new = false;
    uint8_t index = s_hooks.caregiver ? s_hooks.caregiver(id, &is_new, s_hooks.user) : 0;
    return index < 2 ? index : 0;
}

static bool parse_record(const cJSON *item, int16_t fallback_offset_min, lanlan_record_t *record) {
    const char *text = NULL;
    if (!json_string(item, "id", &text) || !text) return false;
    memset(record, 0, sizeof(*record));
    if (!parse_uuid(text, record->id)) return false;

    double number = 0;
    if (json_number(item, "v", &number) && number >= 1) {
        record->version = (uint32_t)number;
    } else {
        record->version = 1;
    }
    if (!json_string(item, "cat", &text)) return false;
    record->category = parse_category(text);
    if (record->category >= LANLAN_CAT_COUNT) return false;

    if (!json_string(item, "sub", &text)) return false;
    record->subitem = parse_subitem(record->category, text);
    if (record->subitem >= LANLAN_SUB_COUNT) return false;

    if (!json_string(item, "at", &text) || !text) return false;
    int16_t parsed_offset = 0;
    if (lanlan_time_parse_rfc3339(text, &record->occurred_epoch, &parsed_offset)
        != LANLAN_TIME_OK) {
        return false;
    }
    /* The compact record carries an IANA zone name in "tz"; the device has no
     * timezone database, so display uses the offset the response reported. */
    (void)json_string(item, "tz", &text);
    record->occurred_tz_offset_min = fallback_offset_min;
    record->created_epoch = record->occurred_epoch;

    if (!json_string(item, "tc", &text)) return false;
    record->time_confidence = (text && strcmp(text, "estimated") == 0)
                                  ? LANLAN_TIME_CONFIDENCE_ESTIMATED
                                  : LANLAN_TIME_CONFIDENCE_TRUSTED;

    record->created_by = caregiver_index(item, "by");
    record->performed_by = caregiver_index(item, "perf");

    /* The cache stores bounded previews, cut on a UTF-8 boundary by the record
     * module, which also records that the note was truncated. */
    if (!json_string(item, "name", &text)) return false;
    (void)lanlan_record_set_custom_name(record, text ? text : "");
    if (!json_string(item, "note", &text)) return false;
    (void)lanlan_record_set_note(record, text ? text : "");

    lanlan_record_set_amount_unknown(record);
    if (!json_string(item, "unit", &text)) return false;
    lanlan_unit_t unit = parse_unit(text);
    if (unit >= LANLAN_UNIT_COUNT) return false;
    if (json_number(item, "amt", &number)) {
        lanlan_record_set_amount(record, number, unit);
    } else if (unit != LANLAN_UNIT_NONE) {
        return false;
    }

    if (json_number(item, "dur", &number) && number > 0) {
        if (number > LANLAN_RECORD_DURATION_MAX) return false;
        record->duration_minutes = (uint16_t)number;
    }

    if (!json_string(item, "st", &text)) return false;
    record->status = (text && strcmp(text, "revoked") == 0) ? LANLAN_STATUS_REVOKED
                                                           : LANLAN_STATUS_ACTIVE;

    if (json_number(item, "seq", &number) && number >= 0) {
        record->seq = (uint32_t)number;
    }
    return lanlan_record_is_valid(record) == LANLAN_RECORD_OK;
}

static bool parse_reminder(const cJSON *item, lanlan_reminder_t *reminder) {
    const char *text = NULL;
    if (!json_string(item, "id", &text) || !text) return false;
    memset(reminder, 0, sizeof(*reminder));
    if (!parse_uuid(text, reminder->id)) return false;
    double number = 0;
    reminder->version = (json_number(item, "v", &number) && number >= 1) ? (uint32_t)number : 1;
    if (!json_string(item, "cat", &text)) return false;
    reminder->category = parse_category(text);
    if (reminder->category >= LANLAN_CAT_COUNT) return false;
    if (!json_string(item, "sub", &text)) return false;
    reminder->subitem = parse_subitem(reminder->category, text);
    if (reminder->subitem >= LANLAN_SUB_COUNT) return false;
    if (!json_string(item, "name", &text)) return false;
    copy_text_field(text, reminder->custom_name, sizeof(reminder->custom_name),
                    LANLAN_RECORD_CUSTOM_CHARS);
    if (json_number(item, "en", &number)) {
        reminder->enabled = number != 0;
    }
    if (!json_string(item, "t", &text)) return false;
    reminder->time_local[0] = '\0';
    if (text && text[0] != '\0') {
        int hour = 0;
        int minute = 0;
        if (!lanlan_time_parse_hhmm(text, &hour, &minute)) return false;
        if (lanlan_time_format_hhmm(hour, minute, reminder->time_local,
                                    sizeof(reminder->time_local))
            == 0) {
            return false;
        }
    }
    reminder->last_rung_day = LANLAN_REMINDER_NO_RUNG;
    return true;
}

/* Parses one /sync/changes page into `batch`, which already carries the
 * envelope's clock fields. `has_more` and the cursor come from the envelope;
 * the batch arrays stay inside their capacity bounds. */
static bool parse_changes(const char *body, size_t length, lanlan_sync_batch_t *batch,
                          bool *has_more) {
    cJSON *root = cJSON_ParseWithLength(body, length);
    if (!root) return false;
    bool ok = false;
    batch->record_count = 0;
    batch->reminder_count = 0;
    batch->revoked_count = 0;
    batch->cursor = 0;
    *has_more = false;
    do {
        const cJSON *cursor = cJSON_GetObjectItemCaseSensitive(root, "cursor");
        const cJSON *records = cJSON_GetObjectItemCaseSensitive(root, "records");
        const cJSON *reminders = cJSON_GetObjectItemCaseSensitive(root, "reminders");
        const cJSON *revoked = cJSON_GetObjectItemCaseSensitive(root, "revoked");
        if (!cJSON_IsNumber(cursor) || !cJSON_IsArray(records) || !cJSON_IsArray(reminders)
            || !cJSON_IsArray(revoked)) {
            break;
        }
        double cursor_value = cJSON_GetNumberValue(cursor);
        if (cursor_value < 0 || cursor_value > (double)UINT32_MAX) break;
        batch->cursor = (uint32_t)cursor_value;

        const cJSON *has_more_item = cJSON_GetObjectItemCaseSensitive(root, "has_more");
        *has_more = cJSON_IsTrue(has_more_item);

        int32_t count = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, records) {
            if (count >= (int32_t)LANLAN_CACHE_RECORD_CAPACITY) break;
            if (!parse_record(item, batch->utc_offset_minutes, &batch->records[count])) {
                count = -1;
                break;
            }
            ++count;
        }
        if (count < 0) break;
        batch->record_count = count;

        count = 0;
        cJSON_ArrayForEach(item, reminders) {
            if (count >= (int32_t)LANLAN_CACHE_REMINDER_CAPACITY) break;
            if (!parse_reminder(item, &batch->reminders[count])) {
                count = -1;
                break;
            }
            ++count;
        }
        if (count < 0) break;
        batch->reminder_count = count;

        count = 0;
        cJSON_ArrayForEach(item, revoked) {
            if (count >= (int32_t)LANLAN_CACHE_RECORD_CAPACITY) break;
            if (!cJSON_IsString(item) || !item->valuestring) {
                count = -1;
                break;
            }
            if (!parse_uuid(item->valuestring, batch->revoked[count])) {
                count = -1;
                break;
            }
            ++count;
        }
        if (count < 0) break;
        batch->revoked_count = count;
        ok = true;
    } while (false);
    cJSON_Delete(root);
    return ok;
}

/* The envelope's clock fields are parsed before the records: the compact record
 * itself carries no numeric offset, and the device has no timezone database. */
static void parse_envelope_time(const char *body, size_t length, lanlan_sync_batch_t *batch) {
    batch->utc_offset_minutes = s_offset_minutes;
    batch->server_epoch = 0;
    batch->has_server_time = false;
    cJSON *root = cJSON_ParseWithLength(body, length);
    if (!root) return;
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(root, "utc_offset_minutes");
    if (cJSON_IsNumber(offset)) {
        double value = cJSON_GetNumberValue(offset);
        if (value >= LANLAN_TIME_OFFSET_MIN && value <= LANLAN_TIME_OFFSET_MAX) {
            batch->utc_offset_minutes = (int16_t)value;
        }
    }
    const cJSON *server_time = cJSON_GetObjectItemCaseSensitive(root, "server_time");
    if (cJSON_IsString(server_time) && server_time->valuestring) {
        int64_t epoch = 0;
        int16_t parsed_offset = 0;
        if (lanlan_time_parse_rfc3339(server_time->valuestring, &epoch, &parsed_offset)
            == LANLAN_TIME_OK) {
            batch->server_epoch = epoch;
            batch->has_server_time = true;
        }
    }
    cJSON_Delete(root);
}

/* Reads the reminder array of a snapshot page; reminders are only sent for
 * offset 0, and their rung state is preserved by the merge. */
static bool parse_snapshot_reminders(const char *body, size_t length,
                                     lanlan_sync_batch_t *batch) {
    cJSON *root = cJSON_ParseWithLength(body, length);
    if (!root) return false;
    bool ok = true;
    const cJSON *reminders = cJSON_GetObjectItemCaseSensitive(root, "reminders");
    if (cJSON_IsArray(reminders)) {
        int32_t count = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, reminders) {
            if (count >= (int32_t)LANLAN_CACHE_REMINDER_CAPACITY) break;
            if (!parse_reminder(item, &batch->reminders[count])) {
                ok = false;
                break;
            }
            ++count;
        }
        batch->reminder_count = ok ? count : 0;
    }
    cJSON_Delete(root);
    return ok;
}

/* ------------------------------------------------------------- sync cycle -- */

static bool apply_batch(lanlan_sync_apply_mode_t mode, const lanlan_sync_batch_t *batch) {
    if (!s_hooks.apply) return false;
    return s_hooks.apply(mode, batch, s_hooks.user);
}

/* Applies the envelope clock and offset to the application. */
static void report_envelope_clock(const lanlan_sync_batch_t *batch) {
    if (batch->utc_offset_minutes >= LANLAN_TIME_OFFSET_MIN
        && batch->utc_offset_minutes <= LANLAN_TIME_OFFSET_MAX) {
        s_offset_minutes = batch->utc_offset_minutes;
    }
    if (s_hooks.clock && batch->has_server_time) {
        s_hooks.clock(batch->server_epoch, batch->utc_offset_minutes,
                      lanlan_time_clock_trusted(true, batch->server_epoch), s_hooks.user);
    }
}

static lanlan_sync_batch_t *work_batch(void) {
    return (lanlan_sync_batch_t *)s_hooks.work;
}

static lanlan_sync_error_t sync_changes(uint32_t start_cursor, uint32_t *applied_cursor,
                                        bool *needs_snapshot) {
    uint32_t cursor = start_cursor;
    *needs_snapshot = false;
    *applied_cursor = start_cursor;
    for (int page = 0; page < LANLAN_SYNC_MAX_PAGES; ++page) {
        char path[128];
        snprintf(path, sizeof(path), "/api/v1/sync/changes?cursor=%u&limit=%d", (unsigned)cursor,
                 LANLAN_SYNC_PAGE_LIMIT);
        http_reply_t reply;
        lanlan_sync_error_t error = LANLAN_SYNC_ERROR_NONE;
        esp_err_t err = http_run(path, NULL, 0, s_body, sizeof(s_body), &reply, &error);
        if (err != ESP_OK) return error;
        if (reply.oversize) return LANLAN_SYNC_ERROR_OVERSIZE;
        if (reply.status == 401) {
            /* The credential is gone: stop retrying and surface the state. */
            s_credential_rejected = true;
            state_set(LANLAN_SYNC_STATE_CREDENTIAL_REJECTED, LANLAN_SYNC_ERROR_NONE);
            return LANLAN_SYNC_ERROR_NONE;
        }
        if (reply.status == 409) {
            /* The cursor is newer than the log: resynchronize from scratch. */
            *needs_snapshot = true;
            return LANLAN_SYNC_ERROR_NONE;
        }
        if (reply.status < 200 || reply.status >= 300) {
            ESP_LOGW(TAG, "sync changes status=%d", reply.status);
            return LANLAN_SYNC_ERROR_HTTP_STATUS;
        }
        if (!reply.complete) return LANLAN_SYNC_ERROR_MALFORMED;
        lanlan_sync_batch_t *batch = work_batch();
        if (!batch) return LANLAN_SYNC_ERROR_INTERNAL;
        parse_envelope_time(s_body, reply.length, batch);
        bool has_more = false;
        if (!parse_changes(s_body, reply.length, batch, &has_more)) {
            return LANLAN_SYNC_ERROR_MALFORMED;
        }
        report_envelope_clock(batch);
        if (!apply_batch(LANLAN_SYNC_APPLY_MERGE, batch)) return LANLAN_SYNC_ERROR_STORAGE;
        cursor = batch->cursor;
        s_cursor = cursor;
        *applied_cursor = cursor;
        http_ack(cursor);
        if (!has_more) return LANLAN_SYNC_ERROR_NONE;
    }
    /* More pages remain: the next cycle continues from the persisted cursor. */
    ESP_LOGI(TAG, "page budget reached; continuing next cycle from cursor=%u",
             (unsigned)cursor);
    return LANLAN_SYNC_ERROR_NONE;
}

/* Reads the snapshot's total record count, used to jump straight to the newest
 * page instead of paging through the whole history. */
static double parse_snapshot_total(const char *body, size_t length) {
    cJSON *root = cJSON_ParseWithLength(body, length);
    if (!root) return 0;
    double total = 0;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "total");
    if (cJSON_IsNumber(item)) total = cJSON_GetNumberValue(item);
    cJSON_Delete(root);
    return total;
}

static lanlan_sync_error_t sync_snapshot(void) {
    lanlan_sync_batch_t *batch = work_batch();
    if (!batch) return LANLAN_SYNC_ERROR_INTERNAL;

    /* First request: one record only, which still returns the reminder list,
     * the cursor and the total, so the newest page can be fetched directly
     * instead of paging through the whole history. The reminder array is only
     * sent for offset 0, so it is copied out before the buffer is reused. */
    http_reply_t reply;
    lanlan_sync_error_t error = LANLAN_SYNC_ERROR_NONE;
    esp_err_t err = http_run("/api/v1/sync/snapshot?offset=0&limit=1", NULL, 0, s_body,
                             sizeof(s_body), &reply, &error);
    if (err != ESP_OK) return error;
    if (reply.oversize) return LANLAN_SYNC_ERROR_OVERSIZE;
    if (reply.status == 401) {
        s_credential_rejected = true;
        state_set(LANLAN_SYNC_STATE_CREDENTIAL_REJECTED, LANLAN_SYNC_ERROR_NONE);
        return LANLAN_SYNC_ERROR_NONE;
    }
    if (reply.status < 200 || reply.status >= 300) return LANLAN_SYNC_ERROR_HTTP_STATUS;
    if (!reply.complete) return LANLAN_SYNC_ERROR_MALFORMED;

    parse_envelope_time(s_body, reply.length, batch);
    batch->record_count = 0;
    batch->revoked_count = 0;
    if (!parse_snapshot_reminders(s_body, reply.length, batch)) {
        return LANLAN_SYNC_ERROR_MALFORMED;
    }
    s_head_reminder_count = batch->reminder_count;
    if (s_head_reminder_count > 0) {
        memcpy(s_head_reminders, batch->reminders,
               (size_t)s_head_reminder_count * sizeof(s_head_reminders[0]));
    }
    double total_value = parse_snapshot_total(s_body, reply.length);

    size_t offset = 0;
    if (total_value > (double)LANLAN_CACHE_RECORD_CAPACITY) {
        offset = (size_t)(total_value - (double)LANLAN_CACHE_RECORD_CAPACITY);
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/v1/sync/snapshot?offset=%u&limit=%u", (unsigned)offset,
             (unsigned)LANLAN_CACHE_RECORD_CAPACITY);
    err = http_run(path, NULL, 0, s_body, sizeof(s_body), &reply, &error);
    if (err != ESP_OK) return error;
    if (reply.oversize) return LANLAN_SYNC_ERROR_OVERSIZE;
    if (reply.status < 200 || reply.status >= 300) return LANLAN_SYNC_ERROR_HTTP_STATUS;
    if (!reply.complete) return LANLAN_SYNC_ERROR_MALFORMED;

    parse_envelope_time(s_body, reply.length, batch);
    bool has_more = false;
    if (!parse_changes(s_body, reply.length, batch, &has_more)) {
        return LANLAN_SYNC_ERROR_MALFORMED;
    }
    if (s_head_reminder_count > 0) {
        memcpy(batch->reminders, s_head_reminders,
               (size_t)s_head_reminder_count * sizeof(s_head_reminders[0]));
        batch->reminder_count = s_head_reminder_count;
    } else {
        batch->reminder_count = 0;
    }
    report_envelope_clock(batch);
    if (!apply_batch(LANLAN_SYNC_APPLY_REPLACE, batch)) return LANLAN_SYNC_ERROR_STORAGE;
    s_cursor = batch->cursor;
    http_ack(s_cursor);
    return LANLAN_SYNC_ERROR_NONE;
}

static void log_heap(const char *stage) {
    ESP_LOGI(TAG, "heap %s: free=%u largest_internal=%u", stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void sync_cycle(void) {
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    bool ready = lanlan_config_ready(&s_config, &s_cred);
    xSemaphoreGive(s_config_lock);

    if (!ready) {
        state_set(LANLAN_SYNC_STATE_CONFIG_REQUIRED, LANLAN_SYNC_ERROR_NO_CREDENTIALS);
        log_heap("after sync attempt (not provisioned)");
        return;
    }
    if (s_credential_rejected) return;

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    lanlan_cred_t cred = s_cred;
    xSemaphoreGive(s_config_lock);

    state_set(LANLAN_SYNC_STATE_WIFI_CONNECTING, LANLAN_SYNC_ERROR_NONE);
    esp_err_t err = wifi_start(&cred);
    /* wifi_start() needs the live credentials; it is the only consumer. */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi start failed: %s", esp_err_to_name(err));
        state_set(LANLAN_SYNC_STATE_OFFLINE, LANLAN_SYNC_ERROR_WIFI);
        log_heap("after sync attempt (wifi init)");
        return;
    }
    if (!wifi_connect()) {
        state_set(LANLAN_SYNC_STATE_OFFLINE, LANLAN_SYNC_ERROR_WIFI);
        log_heap("after sync attempt (wifi connect)");
        return;
    }

    state_set(LANLAN_SYNC_STATE_TIME_PENDING, LANLAN_SYNC_ERROR_NONE);
    /* SNTP is best-effort: the service's server_time also anchors the clock. */
    (void)sntp_sync();

    state_set(LANLAN_SYNC_STATE_SYNCING, LANLAN_SYNC_ERROR_NONE);
    uint32_t applied = s_cursor;
    bool needs_snapshot = false;
    lanlan_sync_error_t error = sync_changes(s_cursor, &applied, &needs_snapshot);
    if (needs_snapshot) {
        ESP_LOGW(TAG, "cursor invalid; falling back to the snapshot");
        error = sync_snapshot();
    }
    if (error != LANLAN_SYNC_ERROR_NONE) {
        lanlan_sync_state_t state = LANLAN_SYNC_STATE_OFFLINE;
        if (error == LANLAN_SYNC_ERROR_HTTP_STATUS) state = LANLAN_SYNC_STATE_SERVER_ERROR;
        if (error == LANLAN_SYNC_ERROR_MALFORMED || error == LANLAN_SYNC_ERROR_OVERSIZE) {
            state = LANLAN_SYNC_STATE_SERVER_ERROR;
        }
        if (error == LANLAN_SYNC_ERROR_STORAGE) state = LANLAN_SYNC_STATE_STORAGE_ERROR;
        state_set(state, error);
        s_fail_count++;
        log_heap("after sync attempt (failed)");
        return;
    }
    /* A 401 already moved the state to CREDENTIAL_REJECTED; do not overwrite it
     * with a success. */
    if (s_credential_rejected) {
        log_heap("after sync attempt (credential rejected)");
        return;
    }

    time_t now = time(NULL);
    portENTER_CRITICAL(&s_state_mux);
    s_last_ok_epoch = (int64_t)now;
    portEXIT_CRITICAL(&s_state_mux);
    s_ok_count++;
    s_backoff_seconds = 0;
    state_set(LANLAN_SYNC_STATE_OK, LANLAN_SYNC_ERROR_NONE);
    log_heap("after sync attempt (ok)");
}

/* ---------------------------------------------------------------- worker -- */

static uint32_t next_backoff_seconds(uint32_t current) {
    if (current == 0) return LANLAN_SYNC_BACKOFF_BASE_SECONDS;
    uint32_t next = current * 2u;
    if (next > LANLAN_SYNC_BACKOFF_MAX_SECONDS) next = LANLAN_SYNC_BACKOFF_MAX_SECONDS;
    return next;
}

static void sync_task(void *argument) {
    (void)argument;
    for (;;) {
        uint32_t wait_seconds = s_backoff_seconds;
        if (wait_seconds == 0) wait_seconds = LANLAN_SYNC_IDLE_INTERVAL_US / 1000000;
        TickType_t wait = pdMS_TO_TICKS((uint32_t)(wait_seconds * 1000u));
        EventBits_t bits = xEventGroupWaitBits(s_events, LANLAN_SYNC_BIT_TRIGGER, pdTRUE, pdFALSE,
                                              wait);
        if (bits & LANLAN_SYNC_BIT_TRIGGER) {
            s_backoff_seconds = 0;
        }
        if (s_credential_rejected) {
            /* Retrying with a rejected credential cannot succeed. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        sync_cycle();
        if (s_state == LANLAN_SYNC_STATE_OK) {
            s_backoff_seconds = 0;
        } else if (s_state != LANLAN_SYNC_STATE_CONFIG_REQUIRED) {
            s_backoff_seconds = next_backoff_seconds(s_backoff_seconds);
        }
    }
}

/* ------------------------------------------------------------------ API -- */

esp_err_t lanlan_sync_init(const lanlan_config_t *config, const lanlan_cred_t *cred,
                           const lanlan_sync_hooks_t *hooks) {
    if (!config || !cred || !hooks || !hooks->apply || !hooks->work) return ESP_ERR_INVALID_ARG;
    if (hooks->work_bytes < LANLAN_SYNC_WORK_BYTES) return ESP_ERR_INVALID_SIZE;
    s_hooks = *hooks;
    s_config = *config;
    s_cred = *cred;
    s_offset_minutes = config->utc_offset_minutes;
    s_cursor = hooks->cursor;
    copy_str(s_base_url, sizeof(s_base_url), config->base_url);
    if (!s_config_lock) s_config_lock = xSemaphoreCreateMutex();
    if (!s_events) s_events = xEventGroupCreate();
    if (!s_config_lock || !s_events) return ESP_ERR_NO_MEM;
    if (s_started) return ESP_OK;
    if (xTaskCreate(sync_task, "lanlan_sync", LANLAN_SYNC_TASK_STACK, NULL,
                    LANLAN_SYNC_TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    log_heap("at start");
    return ESP_OK;
}

void lanlan_sync_set_credentials(const lanlan_config_t *config, const lanlan_cred_t *cred) {
    if (!config || !cred || !s_config_lock) return;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    bool url_changed = strcmp(s_config.base_url, config->base_url) != 0;
    s_config = *config;
    s_cred = *cred;
    copy_str(s_base_url, sizeof(s_base_url), config->base_url);
    s_offset_minutes = config->utc_offset_minutes;
    xSemaphoreGive(s_config_lock);
    if (url_changed) {
        /* A different service has a different log; start its cursor at 0. */
        s_cursor = 0;
    }
    /* New material invalidates the 401 lockout. */
    bool was_rejected = s_credential_rejected;
    s_credential_rejected = false;
    s_backoff_seconds = 0;
    if (was_rejected) {
        state_set(LANLAN_SYNC_STATE_IDLE, LANLAN_SYNC_ERROR_NONE);
        if (s_netif_ready) {
            esp_wifi_disconnect();
            xEventGroupClearBits(s_events, LANLAN_SYNC_BIT_CONNECTED);
        }
    }
    lanlan_sync_request_now();
}

void lanlan_sync_request_now(void) {
    if (s_events) xEventGroupSetBits(s_events, LANLAN_SYNC_BIT_TRIGGER);
}

void lanlan_sync_stop(void) {
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    s_started = false;
}

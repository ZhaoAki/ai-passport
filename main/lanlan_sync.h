/* Cyber Lanlan networking layer: NVS configuration blobs, Wi-Fi station, SNTP
 * clock, and the HTTPS sync client that feeds the bounded cache.
 *
 * Design references:
 *   docs/applications/cyber-lanlan.md          sections 3, 4, 8, 9, 10
 *   docs/applications/cyber-lanlan-service.md  sections 6.1, 6.2, 6.3
 *
 * Ownership and concurrency:
 *   - main.c owns every NVS value, including the `cfg_v1` and `cred_v1` blobs
 *     encoded by this module's helpers. This module only reads copies.
 *   - this module never touches LVGL. It runs one worker task; the LVGL task
 *     only calls the small accessors below, which never block on the network.
 *   - a decoded batch is never applied to the cache directly: the worker hands
 *     it to the `apply` hook, which owns the staging buffer, the cache and the
 *     single NVS commit. The cursor advances only after that hook returns true.
 *
 * The raw credential bytes (Wi-Fi password, device token) are never logged.
 * lanlan_secret_status() exists so the console can report presence without
 * revealing a single character. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "lanlan_cache.h"

/* ------------------------------------------------------- configuration -- */

#define LANLAN_CONFIG_FORMAT_VERSION 1u
#define LANLAN_CRED_FORMAT_VERSION 1u

/* Bounds of the two NVS blobs. The structs below must fit these limits; the
 * sizes are asserted at compile time in lanlan_sync.c. */
#define LANLAN_CONFIG_URL_BYTES 112
#define LANLAN_CONFIG_BLOB_BYTES 128
#define LANLAN_CRED_TOKEN_BYTES 96
#define LANLAN_CRED_DEVICE_ID_BYTES 48
#define LANLAN_CRED_SSID_BYTES 33   /* IEEE 802.11 SSID: 32 bytes + NUL */
#define LANLAN_CRED_PASS_BYTES 65   /* WPA2 passphrase: 63 bytes + NUL */
#define LANLAN_CRED_BLOB_BYTES 256

/* Screen-off policy bounds. The defaults are the design's 30 s / 90 s; both are
 * configurable and persisted, and neither is deep sleep. */
#define LANLAN_DIM_DEFAULT_SECONDS 30
#define LANLAN_SCREEN_OFF_DEFAULT_SECONDS 90
#define LANLAN_DIM_MIN_SECONDS 5
#define LANLAN_DIM_MAX_SECONDS 600
#define LANLAN_SCREEN_OFF_MIN_SECONDS 30
#define LANLAN_SCREEN_OFF_MAX_SECONDS 3600

/* NVS key names in the `lanlan` namespace, so every writer agrees. */
#define LANLAN_NVS_NAMESPACE "lanlan"
#define LANLAN_NVS_KEY_CONFIG "cfg_v1"
#define LANLAN_NVS_KEY_CRED "cred_v1"
#define LANLAN_NVS_KEY_CACHE "cache_v1"
#define LANLAN_NVS_KEY_REMINDER "rem_v1"
#define LANLAN_NVS_KEY_CAREGIVER "care_v1"

/* `cfg_v1`: schema version, UTC offset, mute, reminder sound, dim and
 * screen-off timeouts, service base URL. */
typedef struct {
    uint16_t version;
    int16_t utc_offset_minutes;
    uint8_t mute;
    uint8_t reminder_sound;
    uint16_t dim_seconds;
    uint16_t screen_off_seconds;
    char base_url[LANLAN_CONFIG_URL_BYTES];
} lanlan_config_t;

/* `cred_v1`: device credential and the Wi-Fi station credentials. The device
 * id is stored for diagnostics only; authentication uses the token. */
typedef struct {
    uint16_t version;
    uint16_t reserved;
    char device_token[LANLAN_CRED_TOKEN_BYTES];
    char device_id[LANLAN_CRED_DEVICE_ID_BYTES];
    char wifi_ssid[LANLAN_CRED_SSID_BYTES];
    char wifi_password[LANLAN_CRED_PASS_BYTES];
} lanlan_cred_t;

void lanlan_config_defaults(lanlan_config_t *config);
void lanlan_cred_defaults(lanlan_cred_t *cred);
/* Clamps every field to its documented bound. Returns true when a value moved. */
bool lanlan_config_clamp(lanlan_config_t *config);
/* Fixes up a freshly decoded blob: terminates every string inside its buffer,
 * clamps the numbers, and returns false when the blob is unusable so the
 * caller can fall back to the defaults. */
bool lanlan_config_sanitize(lanlan_config_t *config);
bool lanlan_cred_sanitize(lanlan_cred_t *cred);

/* Little-endian codecs for the NVS blobs. encode returns the byte length, or 0
 * when the output buffer is too small. decode returns false on a truncated,
 * oversized, or version-mismatched value and leaves `out` untouched. */
size_t lanlan_config_encode(const lanlan_config_t *config, uint8_t *out, size_t out_size);
size_t lanlan_cred_encode(const lanlan_cred_t *cred, uint8_t *out, size_t out_size);
bool lanlan_config_decode(const uint8_t *data, size_t size, lanlan_config_t *out);
bool lanlan_cred_decode(const uint8_t *data, size_t size, lanlan_cred_t *out);

/* True when the service URL, the token, the SSID and the password are present,
 * i.e. a sync attempt is meaningful. */
bool lanlan_config_ready(const lanlan_config_t *config, const lanlan_cred_t *cred);

/* "<set, N bytes>" / "<unset>": presence only, never a character of the value.
 * The result is a valid, terminated ASCII string. */
void lanlan_secret_status(const char *secret, char *out, size_t out_size);

/* Normalizes a service base URL: requires http:// or https://, strips trailing
 * slashes and a trailing path-free "/", and rejects embedded credentials.
 * Returns false and writes an empty string when the input is unusable. */
bool lanlan_url_normalize(const char *input, char *out, size_t out_size);
/* True when the URL is plain HTTP. Only a LAN development URL may use it. */
bool lanlan_url_is_insecure(const char *base_url);
/* "+08:00" -> 480, "-05:30" -> -330, "Z" -> 0. Rejects anything else. */
bool lanlan_timezone_parse(const char *text, int16_t *offset_minutes);
/* "+08:00" form; returns the byte length or 0 when the buffer is too small. */
size_t lanlan_timezone_format(int16_t offset_minutes, char *out, size_t out_size);

/* --------------------------------------------------------------- state -- */

typedef enum {
    LANLAN_SYNC_STATE_IDLE = 0,           /* never attempted, or waiting */
    LANLAN_SYNC_STATE_CONFIG_REQUIRED,    /* no SSID/URL/token provisioned */
    LANLAN_SYNC_STATE_WIFI_CONNECTING,
    LANLAN_SYNC_STATE_TIME_PENDING,       /* online, waiting for SNTP */
    LANLAN_SYNC_STATE_SYNCING,
    LANLAN_SYNC_STATE_OK,
    LANLAN_SYNC_STATE_OFFLINE,            /* DNS, connect or timeout */
    LANLAN_SYNC_STATE_CREDENTIAL_REJECTED,/* 401; retrying is pointless */
    LANLAN_SYNC_STATE_SERVER_ERROR,       /* 5xx, or a malformed response */
    LANLAN_SYNC_STATE_STORAGE_ERROR       /* the cache could not be persisted */
} lanlan_sync_state_t;

typedef enum {
    LANLAN_SYNC_ERROR_NONE = 0,
    LANLAN_SYNC_ERROR_NO_CREDENTIALS,
    LANLAN_SYNC_ERROR_WIFI,
    LANLAN_SYNC_ERROR_DNS,
    LANLAN_SYNC_ERROR_CONNECT,
    LANLAN_SYNC_ERROR_TIMEOUT,
    LANLAN_SYNC_ERROR_TRANSPORT,
    LANLAN_SYNC_ERROR_HTTP_STATUS,
    LANLAN_SYNC_ERROR_MALFORMED,
    LANLAN_SYNC_ERROR_OVERSIZE,
    LANLAN_SYNC_ERROR_STORAGE,
    LANLAN_SYNC_ERROR_INTERNAL
} lanlan_sync_error_t;

/* Stable ASCII token for logs, console output and the UI's string table. Never
 * contains a secret or a server-provided string. */
const char *lanlan_sync_state_name(lanlan_sync_state_t state);
const char *lanlan_sync_error_name(lanlan_sync_error_t error);

typedef enum {
    /* Merge the batch into the existing cache and advance the cursor. */
    LANLAN_SYNC_APPLY_MERGE = 0,
    /* Full resynchronization: replace the cache contents, then advance. */
    LANLAN_SYNC_APPLY_REPLACE
} lanlan_sync_apply_mode_t;

/* `work` is the single working buffer for one decoded batch. The glue layer
 * reuses the same block to serialize the cache for the NVS commit: the batch is
 * fully applied to the staging cache before the image is written, so the two
 * uses never overlap. It must be at least LANLAN_SYNC_WORK_BYTES. */
#define LANLAN_SYNC_WORK_BYTES \
    ((size_t)(sizeof(lanlan_sync_batch_t) > LANLAN_CACHE_BLOB_BYTES \
                  ? sizeof(lanlan_sync_batch_t) \
                  : LANLAN_CACHE_BLOB_BYTES))

typedef struct {
    void *work;
    size_t work_bytes;
    /* Cursor restored from NVS at start-up; 0 means "send everything". */
    uint32_t cursor;
    /* Called on the sync worker with a decoded batch. Must apply the batch to
     * the staging cache and persist the batch plus the new cursor in a single
     * NVS commit. Return true only when the commit succeeded. */
    bool (*apply)(lanlan_sync_apply_mode_t mode, const lanlan_sync_batch_t *batch, void *user);
    /* Clock update from SNTP or from a service response, in UTC seconds.
     * `trusted` already applies lanlan_time_clock_trusted(). */
    void (*clock)(int64_t server_epoch, int16_t utc_offset_minutes, bool trusted, void *user);
    /* Maps a caregiver user id to the 0/1 index the record struct stores and
     * the names come from. `is_new` is set when the id was seen for the first
     * time and the glue layer should persist its table with the next commit.
     * Returns 0xFF when the id cannot be mapped. */
    uint8_t (*caregiver)(const uint8_t id[LANLAN_ID_BYTES], bool *is_new, void *user);
    void *user;
} lanlan_sync_hooks_t;

/* Starts the networking worker. Copies config and credentials into its own
 * storage, so the caller keeps owning the NVS values. Fails when the work
 * buffer is too small or the worker cannot be created. */
esp_err_t lanlan_sync_init(const lanlan_config_t *config, const lanlan_cred_t *cred,
                           const lanlan_sync_hooks_t *hooks);
/* Pushes new credentials after serial provisioning, clears a 401 lockout and
 * starts a sync immediately. */
void lanlan_sync_set_credentials(const lanlan_config_t *config, const lanlan_cred_t *cred);
/* Asks for a sync now. Safe from any task, including a console handler; never
 * blocks and never returns an error to report. */
void lanlan_sync_request_now(void);
/* Stops the worker and releases its buffers. Used only for completeness; the
 * application never tears the network down. */
void lanlan_sync_stop(void);

/* ---------------------------------------------------------- accessors -- */

lanlan_sync_state_t lanlan_sync_state(void);
lanlan_sync_error_t lanlan_sync_last_error(void);
/* UTC epoch of the last successful sync, or 0 when the device never synced. */
int64_t lanlan_sync_last_ok_epoch(void);
/* Cursor persisted with the last applied batch. */
uint32_t lanlan_sync_cursor(void);
/* Current backoff in seconds; 0 while healthy. */
uint32_t lanlan_sync_backoff_seconds(void);
/* True once the service rejected the credential with HTTP 401. */
bool lanlan_sync_credential_rejected(void);
/* Counters for the settings/status page. */
void lanlan_sync_counters(uint32_t *ok, uint32_t *failed);

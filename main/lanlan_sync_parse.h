/* Cyber Lanlan sync response decoding.
 *
 * Pure logic: turns a `/api/v1/sync/changes` or `/api/v1/sync/snapshot` body
 * into a lanlan_sync_batch_t plus the page fields the caller needs, and
 * classifies an HTTP status. No ESP-IDF, no cJSON, no LVGL, no allocation, so
 * the whole wire format is covered by tests/test_lanlan_sync_parse.c on the
 * host.
 *
 * Field rules (docs/applications/cyber-lanlan-service.md section 6):
 *   - changes:  `cursor` and `records` are mandatory; `reminders` and `revoked`
 *               are optional arrays; `members`, `server_time`, `timezone` and
 *               `utc_offset_minutes` are optional; `device_id` is ignored.
 *   - snapshot: `cursor` and `records` are mandatory; `has_more`, `offset`,
 *               `limit`, `total`, `reminders` (first page) and `members` are
 *               optional.
 * Unknown top-level and unknown member fields are ignored. An absent, empty or
 * malformed `members` array is "no update", never a batch failure. `members` is
 * applied through the hook BEFORE any record of the page is mapped, so a record
 * can never be labelled with a stale caregiver directory. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lanlan_cache.h"
#include "lanlan_caregiver.h"

typedef enum {
    LANLAN_SYNC_PARSE_OK = 0,
    LANLAN_SYNC_PARSE_MALFORMED,     /* JSON syntax, escapes, numbers, depth */
    LANLAN_SYNC_PARSE_MISSING_FIELD, /* a mandatory field is absent */
    LANLAN_SYNC_PARSE_WRONG_TYPE,    /* a field is present with the wrong type */
    LANLAN_SYNC_PARSE_TOO_LARGE,     /* a token or string past its byte limit */
    LANLAN_SYNC_PARSE_INVALID_VALUE  /* right type, unusable value (bad id, ...) */
} lanlan_sync_parse_status_t;

const char *lanlan_sync_parse_status_name(lanlan_sync_parse_status_t status);

/* Applied with the `members` array of the page, before the records. */
typedef void (*lanlan_sync_members_fn)(const lanlan_caregiver_member_t *members, int count,
                                       void *user);
/* Maps a record's caregiver user id to the 0/1 slot the record struct stores. */
typedef uint8_t (*lanlan_sync_caregiver_fn)(const char *id, void *user);

typedef struct {
    lanlan_sync_members_fn members;      /* optional */
    lanlan_sync_caregiver_fn caregiver;  /* optional */
    void *user;
} lanlan_sync_parse_hooks_t;

typedef enum {
    LANLAN_SYNC_PAGE_CHANGES = 0,
    LANLAN_SYNC_PAGE_SNAPSHOT
} lanlan_sync_page_t;

/* Page fields that are not part of the batch. Every field is optional; the
 * has_* flag says whether the body carried it. */
typedef struct {
    bool has_offset;
    int64_t offset;
    bool has_limit;
    int64_t limit;
    bool has_total;
    int64_t total;
    bool has_has_more;
    bool has_more;
} lanlan_sync_page_info_t;

/* Parses one page into `batch`, which is cleared first and left cleared on
 * failure. `info` may be NULL. The hooks may be NULL. */
/* `utc_offset_fallback` is used for records whose page does not carry a usable
 * `utc_offset_minutes`; the sync worker passes its configured family offset. */
lanlan_sync_parse_status_t lanlan_sync_parse_page(lanlan_sync_page_t page, const char *body,
                                                  size_t length, int16_t utc_offset_fallback,
                                                  const lanlan_sync_parse_hooks_t *hooks,
                                                  lanlan_sync_batch_t *batch,
                                                  lanlan_sync_page_info_t *info);

/* ------------------------------------------------------------- HTTP status -- */

typedef enum {
    LANLAN_SYNC_HTTP_OK = 0,       /* 2xx: parse the body */
    LANLAN_SYNC_HTTP_REJECTED,     /* 401: the credential is gone, stop retrying */
    LANLAN_SYNC_HTTP_RESYNC,       /* 409 cursor_invalid: fall back to /sync/snapshot */
    LANLAN_SYNC_HTTP_CLIENT_ERROR, /* another 4xx: back off and retry */
    LANLAN_SYNC_HTTP_RETRY,        /* 5xx and 429: back off and retry */
    LANLAN_SYNC_HTTP_FATAL         /* anything else: back off and retry */
} lanlan_sync_http_t;

/* Pure classification of one HTTP status plus the parsed `error.code`
 * (`error_code` may be NULL or empty). Transport failures and timeouts carry no
 * status at all and are classified by the ESP-IDF layer instead. */
lanlan_sync_http_t lanlan_sync_http_classify(int status, const char *error_code);
const char *lanlan_sync_http_name(lanlan_sync_http_t result);

/* Extracts `error.code` from a service error body
 * ({"error": {"code": "...", "message": "...", "field": "..."}}). Returns false
 * when the body has no usable code; `out` is always NUL-terminated. */
bool lanlan_sync_parse_error_code(const char *body, size_t length, char *out, size_t out_size);

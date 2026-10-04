/* Cyber Lanlan bounded device cache: fixed-size records, tombstones and
 * reminders with ONE canonical little-endian binary codec, a staging/live
 * two-buffer commit, CRC32 integrity and eviction.
 *
 * Design rules (docs/applications/cyber-lanlan.md section 3 and
 * docs/applications/cyber-lanlan-service.md section 7):
 *   - the 40 newest records by occurred_at, then id, newest first;
 *   - at most 32 revocation tombstones kept as a ring of 16-byte ids;
 *   - at most 16 reminders plus the last rung local date per reminder;
 *   - a sync batch is merged into a staging cache and the caller commits it
 *     atomically together with the new cursor; on any failure the live cache,
 *     the tombstones, the reminders and the cursor are untouched.
 *
 * Memory is caller-provided and static: this module never allocates. The exact
 * byte budgets are asserted in tests/test_lanlan_cache.c and, where the C
 * compiler can evaluate them, with _Static_assert in lanlan_cache.c:
 *
 *   sizeof(lanlan_record_t)         <= 176 bytes
 *   LANLAN_CACHE_RECORD_ENTRY_BYTES <= 160 bytes
 *   LANLAN_CACHE_BLOB_BYTES         <= 8192 bytes
 *   sizeof(lanlan_cache_t)          <= 8192 bytes (one cache)
 *   live + staging                  <= 16384 bytes of static RAM
 *
 * The serialized size is bounded by the FIXED arrays, not by the live counts:
 * a device with an empty cache still encodes a fixed-size blob, so the NVS
 * value length never changes while the format version stays the same. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lanlan_record.h"

/* -------------------------------------------------------------- capacity -- */

#define LANLAN_CACHE_MAX_RECORDS 40
#define LANLAN_CACHE_MAX_TOMBSTONES 32
#define LANLAN_CACHE_MAX_REMINDERS 16
/* Aliases used by the view layer and the tests. */
#define LANLAN_CACHE_RECORD_CAPACITY LANLAN_CACHE_MAX_RECORDS
#define LANLAN_CACHE_TOMBSTONE_CAPACITY LANLAN_CACHE_MAX_TOMBSTONES
#define LANLAN_CACHE_REMINDER_CAPACITY LANLAN_CACHE_MAX_REMINDERS

/* Current on-disk schema version. A blob with another version is reported as
 * LANLAN_CACHE_VERSION_MISMATCH and must be rebuilt from the service. */
#define LANLAN_CACHE_FORMAT_VERSION 1u

/* Fixed-size serialized entry sizes. Every field is encoded explicitly, so
 * these numbers are independent of compiler padding on the host and on the
 * ESP32-C3. A fixed blob also means the NVS value length is constant. */
#define LANLAN_CACHE_ID_BYTES LANLAN_ID_BYTES
#define LANLAN_CACHE_HEADER_BYTES 32u
#define LANLAN_CACHE_CRC_BYTES 4u
#define LANLAN_CACHE_RECORD_ENTRY_BYTES 160u
#define LANLAN_CACHE_REMINDER_ENTRY_BYTES 64u
#define LANLAN_CACHE_TOMBSTONE_ENTRY_BYTES LANLAN_CACHE_ID_BYTES

/* Canonical serialized sizes. The blob is: header -> fixed record array ->
 * fixed tombstone array -> fixed reminder array -> CRC32 trailer. */
#define LANLAN_CACHE_RECORDS_BYTES \
    (LANLAN_CACHE_MAX_RECORDS * LANLAN_CACHE_RECORD_ENTRY_BYTES)
#define LANLAN_CACHE_TOMBSTONES_BYTES \
    (LANLAN_CACHE_MAX_TOMBSTONES * LANLAN_CACHE_TOMBSTONE_ENTRY_BYTES)
#define LANLAN_CACHE_REMINDERS_BYTES \
    (LANLAN_CACHE_MAX_REMINDERS * LANLAN_CACHE_REMINDER_ENTRY_BYTES)
#define LANLAN_CACHE_BLOB_BYTES                                                          \
    (LANLAN_CACHE_HEADER_BYTES + LANLAN_CACHE_RECORDS_BYTES + LANLAN_CACHE_TOMBSTONES_BYTES \
     + LANLAN_CACHE_REMINDERS_BYTES + LANLAN_CACHE_CRC_BYTES)
/* Name kept for callers that used the old constant. */
#define LANLAN_CACHE_MAX_BLOB_BYTES LANLAN_CACHE_BLOB_BYTES
/* Byte size of the staging buffer the atomic commit needs: one more cache
 * struct. Live plus staging is the whole static RAM cost of this module. */
#define LANLAN_CACHE_STAGING_BYTES ((size_t)sizeof(lanlan_cache_t))
#define LANLAN_CACHE_MAX_LIVE_BYTES ((size_t)sizeof(lanlan_cache_t))

typedef enum {
    LANLAN_CACHE_OK = 0,              /* decoded or committed */
    LANLAN_CACHE_EMPTY,               /* valid but no records/reminders/tombstones */
    LANLAN_CACHE_CORRUPT,             /* bad magic, bad CRC, or failed a bounds check */
    LANLAN_CACHE_VERSION_MISMATCH,    /* readable header, unsupported format version */
    LANLAN_CACHE_TRUNCATED            /* shorter than the canonical blob length */
} lanlan_cache_status_t;

const char *lanlan_cache_status_name(lanlan_cache_status_t status);
/* True when the caller should show the "cache rebuilt" notice and resync. */
bool lanlan_cache_status_needs_rebuild(lanlan_cache_status_t status);

/* ------------------------------------------------------------- data model -- */

typedef struct {
    /* LANLAN_REMINDER_NO_RUNG when this reminder never rang. */
    int64_t last_rung_day;
    uint8_t id[LANLAN_CACHE_ID_BYTES];
    uint32_t version;
    char time_local[6];          /* "HH:MM" or "" when no schedule is configured */
    char custom_name[LANLAN_RECORD_CUSTOM_BYTES];
    lanlan_category_t category;
    lanlan_subitem_t subitem;
    bool enabled;                /* every reminder starts disabled */
} lanlan_reminder_t;

/* A reminder that never rang uses this day index. It is far below any real
 * local date and far above any int64 underflow region. */
#define LANLAN_REMINDER_NO_RUNG ((int64_t)(-1))

typedef struct {
    uint32_t record_count;
    uint32_t tombstone_count;
    uint32_t reminder_count;
    uint32_t cursor;             /* persisted service cursor for this snapshot */
    lanlan_record_t records[LANLAN_CACHE_MAX_RECORDS];
    uint8_t tombstones[LANLAN_CACHE_MAX_TOMBSTONES][LANLAN_CACHE_ID_BYTES];
    lanlan_reminder_t reminders[LANLAN_CACHE_MAX_REMINDERS];
} lanlan_cache_t;

/* Decoded batch, exactly the compact sync payload of
 * docs/applications/cyber-lanlan-service.md section 6.1. */
typedef struct {
    uint32_t cursor;             /* cursor to persist with this batch */
    int32_t record_count;
    int32_t reminder_count;
    int32_t revoked_count;
    lanlan_record_t records[LANLAN_CACHE_MAX_RECORDS];
    lanlan_reminder_t reminders[LANLAN_CACHE_MAX_REMINDERS];
    uint8_t revoked[LANLAN_CACHE_MAX_RECORDS][LANLAN_ID_BYTES];
    int16_t utc_offset_minutes;  /* family timezone offset in effect at server_time */
    int64_t server_epoch;        /* parsed server_time; 0 when absent */
    bool has_server_time;
} lanlan_sync_batch_t;

/* Result of lanlan_cache_decode(): the status plus the decoded cache. The cache
 * is only meaningful when the status is OK or EMPTY. */
typedef struct {
    lanlan_cache_status_t status;
    lanlan_cache_t cache;
} lanlan_cache_decode_result_t;

/* Report of records dropped by the capacity bound. */
typedef struct {
    int32_t count;
    uint8_t ids[LANLAN_CACHE_MAX_RECORDS][LANLAN_ID_BYTES];
} lanlan_cache_drop_report_t;

typedef struct {
    int32_t count;
    uint8_t ids[LANLAN_CACHE_MAX_RECORDS][LANLAN_ID_BYTES];
} lanlan_cache_revoked_report_t;

/* ---------------------------------------------------------------- basics -- */

void lanlan_cache_clear(lanlan_cache_t *cache);
/* qsort comparator over lanlan_record_t*: newest first by occurred_epoch, then
 * by the 16-byte id in memory order. Exposed so tests can pin the order. */
int lanlan_cache_record_compare(const void *left, const void *right);
/* Sorts records newest-first (occurred_epoch descending, then id) and drops
 * anything past the capacity, reporting it in drops (may be NULL). */
void lanlan_cache_sort_records(lanlan_cache_t *cache, lanlan_cache_drop_report_t *drops);
int lanlan_cache_find_record(const lanlan_cache_t *cache, const uint8_t id[LANLAN_ID_BYTES]);
int lanlan_cache_find_reminder(const lanlan_cache_t *cache, const uint8_t id[LANLAN_ID_BYTES]);
bool lanlan_cache_is_tombstoned(const lanlan_cache_t *cache, const uint8_t id[LANLAN_ID_BYTES]);
/* Bound-checked accessors; return a pointer to an all-zero record/reminder for an
 * out-of-range index, so the view layer never dereferences a stale slot. */
const lanlan_record_t *lanlan_cache_record_at(const lanlan_cache_t *cache, int index);
const lanlan_reminder_t *lanlan_cache_reminder_at(const lanlan_cache_t *cache, int index);
/* Inserts a tombstone, keeping the ring bounded: the oldest entry is dropped
 * when the ring is full and the id is appended otherwise. Duplicates ignored. */
void lanlan_cache_add_tombstone(lanlan_cache_t *cache, const uint8_t id[LANLAN_ID_BYTES]);
/* True when the in-memory cache satisfies every bound and relationship the
 * codec and the UI rely on. */
bool lanlan_cache_is_consistent(const lanlan_cache_t *cache);

/* ---------------------------------------------------------------- codec -- */

/* Serializes the cache into out. Returns LANLAN_CACHE_BLOB_BYTES, or 0 when
 * out_size is smaller. This function defines the canonical byte order; the
 * decoder reads exactly this layout and nothing else. */
size_t lanlan_cache_encode(const lanlan_cache_t *cache, uint8_t *out, size_t out_size);
/* Decodes a canonical blob. Never reads outside [data, data + size):
 *   - NULL data or size < canonical length -> TRUNCATED (rebuildable)
 *   - bad magic / bad CRC / count > max / non-zero unused slot / invalid entry
 *       -> CORRUPT
 *   - version != FORMAT_VERSION -> VERSION_MISMATCH
 * A failed decode leaves result->cache zeroed. */
lanlan_cache_status_t lanlan_cache_decode(const uint8_t *data, size_t size,
                                          lanlan_cache_decode_result_t *result);

/* ------------------------------------------------- batch merge and commit -- */

/* Merges a decoded batch into `staging` (which must be a distinct buffer, not
 * `live`). Revoked ids remove matching records and are recorded as tombstones
 * so a later batch cannot resurrect them; records whose id is tombstoned are
 * dropped from the batch. Records are merged by id keeping the highest version,
 * evicted at the 40-record bound. Returns OK, or why the batch was rejected.
 * On failure NOTHING is published: `staging` is left unusable and the caller
 * must keep the previous cache. */
lanlan_cache_status_t lanlan_cache_apply_batch(const lanlan_cache_t *live,
                                               const lanlan_sync_batch_t *batch,
                                               lanlan_cache_t *staging,
                                               lanlan_cache_drop_report_t *drops,
                                               lanlan_cache_revoked_report_t *removed);
/* Atomically publishes staging to live and advances the cursor. Returns true
 * only when `confirm` is true and staging is consistent; otherwise live and
 * cursor are untouched and the caller keeps the previous cache and cursor. */
bool lanlan_cache_commit(lanlan_cache_t *live, const lanlan_cache_t *staging, uint32_t *cursor,
                         uint32_t new_cursor, bool confirm);

/* --------------------------------------------------------------- eviction -- */

/* True when the cache holds more records than the capacity bound allows. */
bool lanlan_cache_needs_evict(const lanlan_cache_t *cache);
/* Drops the oldest records until the bound holds and reports the dropped ids.
 * Returns the number dropped (0 when nothing was over the bound). */
int lanlan_cache_evict(lanlan_cache_t *cache, lanlan_cache_drop_report_t *drops);

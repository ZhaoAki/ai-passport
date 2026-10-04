/* Cyber Lanlan bounded cache implementation. See lanlan_cache.h.
 *
 * ------------------------------------------------------------------ codec --
 *
 * ONE canonical blob layout, used by both the encoder and the decoder. The
 * arrays are written at their FIXED capacities, so the offsets below are
 * constants and the blob length never depends on the live counts:
 *
 *   offset                    size  field
 *   0                         2     magic 'L','C'
 *   2                         1     format version
 *   3                         1     record count
 *   4                         1     tombstone count
 *   5                         1     reminder count
 *   6                         4     cursor (uint32, little-endian)
 *   10                        22    reserved, zero
 *   32                        6400  record entries, 40 x 160 bytes
 *   6432                      512   tombstone ids, 32 x 16 bytes
 *   6944                      1024  reminder entries, 16 x 64 bytes
 *   7968                      4     CRC32 trailer over everything before it
 *   total                     7972  bytes (LANLAN_CACHE_BLOB_BYTES)
 *
 * The trailer CRC is checked before a single entry is decoded, so a damaged NVS
 * value is reported as CORRUPT instead of feeding the UI half-decoded data.
 * The record and reminder entries carry their own length fields, so the CRC is
 * the only whole-blob integrity mechanism needed.
 *
 * Record entry (160 bytes), long buffers first so they pack without padding:
 *   0    16  id
 *   16   48  note preview, NUL padded
 *   64   24  custom name, NUL padded
 *   88   8   amount_value (binary64, little-endian)
 *   96   8   occurred_epoch (int64)
 *   104  4   version
 *   108  4   seq
 *   112  2   occurred_tz_offset_min (int16)
 *   114  2   duration_minutes
 *   116  1   category
 *   117  1   subitem
 *   118  1   time confidence
 *   119  1   status
 *   120  1   unit
 *   121  1   flags: bit0 amount_known, bit1 note_truncated
 *   122  1   created_by
 *   123  1   performed_by
 *   124  1   note preview length in bytes
 *   125  1   reserved, zero
 *   126  2   custom name length in bytes
 *   128  8   created_epoch (int64)
 *   136  24  reserved, zero
 *
 * Reminder entry (64 bytes):
 *   0    8   last_rung_day (int64)
 *   8    16  id
 *   24   24  custom name, NUL padded
 *   48   4   version
 *   52   6   time_local "HH:MM" + NUL
 *   58   1   category
 *   59   1   subitem
 *   60   1   enabled
 *   61   1   time_local length (0 or 5)
 *   62   2   reserved, zero
 */
#include "lanlan_cache.h"
#include "lanlan_time.h"

#include <stdlib.h>
#include <string.h>

#define LANLAN_CACHE_MAGIC_0 'L'
#define LANLAN_CACHE_MAGIC_1 'C'

/* Record entry field offsets. */
#define REC_NOTE_OFFSET 16u
#define REC_CUSTOM_OFFSET 64u
#define REC_AMOUNT_OFFSET 88u
#define REC_OCCURRED_OFFSET 96u
#define REC_VERSION_OFFSET 104u
#define REC_SEQ_OFFSET 108u
#define REC_TZ_OFFSET 112u
#define REC_DURATION_OFFSET 114u
#define REC_CATEGORY_OFFSET 116u
#define REC_SUBITEM_OFFSET 117u
#define REC_CONFIDENCE_OFFSET 118u
#define REC_STATUS_OFFSET 119u
#define REC_UNIT_OFFSET 120u
#define REC_FLAGS_OFFSET 121u
#define REC_CREATED_BY_OFFSET 122u
#define REC_PERFORMED_BY_OFFSET 123u
#define REC_NOTE_LEN_OFFSET 124u
#define REC_CUSTOM_LEN_OFFSET 126u
#define REC_CREATED_OFFSET 128u
#define REC_AMOUNT_FLAG 0x01u
#define REC_NOTE_CUT_FLAG 0x02u

/* Reminder entry field offsets. */
#define REM_RUNG_OFFSET 0u
#define REM_CUSTOM_OFFSET 24u
#define REM_VERSION_OFFSET 48u
#define REM_TIME_OFFSET 52u
#define REM_TIME_FIELD_BYTES 6u
#define REM_CATEGORY_OFFSET 58u
#define REM_SUBITEM_OFFSET 59u
#define REM_ENABLED_OFFSET 60u
#define REM_TIME_LEN_OFFSET 61u

/* Region offsets: constants, because every array is written at full capacity. */
#define RECORDS_OFFSET LANLAN_CACHE_HEADER_BYTES
#define TOMBSTONES_OFFSET (RECORDS_OFFSET + LANLAN_CACHE_RECORDS_BYTES)
#define REMINDERS_OFFSET (TOMBSTONES_OFFSET + LANLAN_CACHE_TOMBSTONES_BYTES)
#define CRC_TRAILER_OFFSET (REMINDERS_OFFSET + LANLAN_CACHE_REMINDERS_BYTES)

/* ------------------------------------------------------- budget asserts -- */

_Static_assert(sizeof(lanlan_record_t) <= 176,
               "sizeof(lanlan_record_t) must stay within the 176-byte budget");
_Static_assert(LANLAN_CACHE_RECORD_ENTRY_BYTES <= 160,
               "the serialized record entry must stay within the 160-byte budget");
_Static_assert(LANLAN_CACHE_BLOB_BYTES <= 8192,
               "the canonical blob must fit the ~8 KB cache budget");
_Static_assert(sizeof(lanlan_cache_t) <= 8192,
               "one live cache must stay within the 8192-byte budget");
_Static_assert(2 * sizeof(lanlan_cache_t) <= 16384,
               "live plus staging must stay within 16384 bytes of static RAM");
_Static_assert(REC_NOTE_OFFSET + LANLAN_RECORD_NOTE_PREVIEW_BYTES
                   <= LANLAN_CACHE_RECORD_ENTRY_BYTES,
               "the record entry must hold the note preview");
_Static_assert(REC_CUSTOM_OFFSET + LANLAN_RECORD_CUSTOM_BYTES
                   <= LANLAN_CACHE_RECORD_ENTRY_BYTES,
               "the record entry must hold the custom name");
_Static_assert(REM_CUSTOM_OFFSET + LANLAN_RECORD_CUSTOM_BYTES
                   <= LANLAN_CACHE_REMINDER_ENTRY_BYTES,
               "the reminder entry must hold the custom name");
_Static_assert(sizeof(double) == 8, "the amount field requires a 64-bit double");
_Static_assert(CRC_TRAILER_OFFSET + LANLAN_CACHE_CRC_BYTES == LANLAN_CACHE_BLOB_BYTES,
               "the CRC trailer must end the canonical blob");

/* ------------------------------------------------------- scalar helpers -- */

static void put_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put_u32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)((value >> (8u * i)) & 0xFFu);
}

static void put_i64(uint8_t *out, int64_t value) {
    uint64_t bits = (uint64_t)value;
    for (unsigned i = 0; i < 8; ++i) out[i] = (uint8_t)((bits >> (8u * i)) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)in[i] << (8u * i);
    return value;
}

static int64_t get_i64(const uint8_t *in) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)in[i] << (8u * i);
    return (int64_t)value;
}

static void put_double(uint8_t *out, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    for (unsigned i = 0; i < 8; ++i) out[i] = (uint8_t)((bits >> (8u * i)) & 0xFFu);
}

static double get_double(const uint8_t *in) {
    uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= (uint64_t)in[i] << (8u * i);
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), computed bitwise so
 * the code carries no 1 KB table into flash. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t crc32_of(const uint8_t *data, size_t length) {
    return crc32_update(0u, data, length);
}

/* --------------------------------------------------------------- basics -- */

const char *lanlan_cache_status_name(lanlan_cache_status_t status) {
    switch (status) {
    case LANLAN_CACHE_OK: return "ok";
    case LANLAN_CACHE_EMPTY: return "empty";
    case LANLAN_CACHE_CORRUPT: return "corrupt";
    case LANLAN_CACHE_VERSION_MISMATCH: return "version_mismatch";
    case LANLAN_CACHE_TRUNCATED: return "truncated";
    }
    return "unknown";
}

bool lanlan_cache_status_needs_rebuild(lanlan_cache_status_t status) {
    return status == LANLAN_CACHE_CORRUPT || status == LANLAN_CACHE_VERSION_MISMATCH
           || status == LANLAN_CACHE_TRUNCATED;
}

void lanlan_cache_clear(lanlan_cache_t *cache) {
    if (cache) memset(cache, 0, sizeof(*cache));
}

int lanlan_cache_record_compare(const void *left, const void *right) {
    const lanlan_record_t *a = (const lanlan_record_t *)left;
    const lanlan_record_t *b = (const lanlan_record_t *)right;
    /* Newest first: the larger occurred_epoch sorts earlier, and equal times
     * fall back to the id so the order is total and stable across restarts. */
    if (a->occurred_epoch != b->occurred_epoch) return a->occurred_epoch > b->occurred_epoch ? -1 : 1;
    int order = memcmp(a->id, b->id, LANLAN_ID_BYTES);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

/* Reminders are ordered by id; the service order is a change order, not a
 * display order, so the list must not shuffle when an unrelated row changes. */
static int reminder_compare(const void *left, const void *right) {
    const lanlan_reminder_t *a = (const lanlan_reminder_t *)left;
    const lanlan_reminder_t *b = (const lanlan_reminder_t *)right;
    int order = memcmp(a->id, b->id, LANLAN_ID_BYTES);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

/* Recomputes record_count from the array and returns it, reporting the entries
 * dropped past the capacity so eviction is never silent. */
static uint32_t normalize_record_count(lanlan_cache_t *cache, lanlan_cache_drop_report_t *drops) {
    if (cache->record_count > LANLAN_CACHE_MAX_RECORDS) {
        /* The array is already sorted newest-first, so the entries past the
         * capacity are the oldest ones and are what the report describes. */
        if (drops) {
            for (uint32_t i = LANLAN_CACHE_MAX_RECORDS; i < cache->record_count
                                                    && drops->count < LANLAN_CACHE_MAX_RECORDS;
                 ++i) {
                memcpy(drops->ids[drops->count++], cache->records[i].id, LANLAN_ID_BYTES);
            }
        }
        cache->record_count = LANLAN_CACHE_MAX_RECORDS;
    }
    return cache->record_count;
}

void lanlan_cache_sort_records(lanlan_cache_t *cache, lanlan_cache_drop_report_t *drops) {
    if (!cache) return;
    if (cache->record_count > LANLAN_CACHE_MAX_RECORDS) {
        /* Sort the whole set first so the entries reported as dropped are
         * really the oldest ones, not merely the tail of the input order. */
        qsort(cache->records, cache->record_count, sizeof(cache->records[0]),
              lanlan_cache_record_compare);
        normalize_record_count(cache, drops);
    } else {
        qsort(cache->records, cache->record_count, sizeof(cache->records[0]),
              lanlan_cache_record_compare);
    }
}

int lanlan_cache_find_record(const lanlan_cache_t *cache, const uint8_t id[LANLAN_ID_BYTES]) {
    if (!cache) return -1;
    for (uint32_t i = 0; i < cache->record_count && i < LANLAN_CACHE_MAX_RECORDS; ++i) {
        if (lanlan_record_id_equal(cache->records[i].id, id)) return (int)i;
    }
    return -1;
}

int lanlan_cache_find_reminder(const lanlan_cache_t *cache, const uint8_t id[LANLAN_ID_BYTES]) {
    if (!cache) return -1;
    for (uint32_t i = 0; i < cache->reminder_count && i < LANLAN_CACHE_MAX_REMINDERS; ++i) {
        if (lanlan_record_id_equal(cache->reminders[i].id, id)) return (int)i;
    }
    return -1;
}

bool lanlan_cache_is_tombstoned(const lanlan_cache_t *cache, const uint8_t id[LANLAN_ID_BYTES]) {
    if (!cache) return false;
    for (uint32_t i = 0; i < cache->tombstone_count && i < LANLAN_CACHE_MAX_TOMBSTONES; ++i) {
        if (lanlan_record_id_equal(cache->tombstones[i], id)) return true;
    }
    return false;
}

/* Both accessors return a zeroed static slot for an out-of-range index so the
 * view layer can render "no record" without a NULL check on every use. */
static const lanlan_record_t *empty_record(void) {
    static const lanlan_record_t s_empty;
    return &s_empty;
}

static const lanlan_reminder_t *empty_reminder(void) {
    static const lanlan_reminder_t s_empty = {.last_rung_day = LANLAN_REMINDER_NO_RUNG};
    return &s_empty;
}

const lanlan_record_t *lanlan_cache_record_at(const lanlan_cache_t *cache, int index) {
    if (!cache || index < 0 || (uint32_t)index >= cache->record_count
        || (uint32_t)index >= LANLAN_CACHE_MAX_RECORDS) {
        return empty_record();
    }
    return &cache->records[index];
}

const lanlan_reminder_t *lanlan_cache_reminder_at(const lanlan_cache_t *cache, int index) {
    if (!cache || index < 0 || (uint32_t)index >= cache->reminder_count
        || (uint32_t)index >= LANLAN_CACHE_MAX_REMINDERS) {
        return empty_reminder();
    }
    return &cache->reminders[index];
}

void lanlan_cache_add_tombstone(lanlan_cache_t *cache, const uint8_t id[LANLAN_ID_BYTES]) {
    if (!cache || lanlan_record_id_is_zero(id)) return;
    if (lanlan_cache_is_tombstoned(cache, id)) return;
    if (cache->tombstone_count < LANLAN_CACHE_MAX_TOMBSTONES) {
        memcpy(cache->tombstones[cache->tombstone_count++], id, LANLAN_ID_BYTES);
        return;
    }
    /* Ring full: drop the oldest tombstone so the newest revocations survive.
     * 32 entries is far more than the revocations two caregivers produce
     * between syncs, and a tombstone is only needed until the service snapshot
     * no longer offers the record. */
    memmove(cache->tombstones[0], cache->tombstones[1],
            (LANLAN_CACHE_MAX_TOMBSTONES - 1) * LANLAN_ID_BYTES);
    memcpy(cache->tombstones[LANLAN_CACHE_MAX_TOMBSTONES - 1], id, LANLAN_ID_BYTES);
}

/* ------------------------------------------------------------- validation -- */

static bool reminder_is_valid(const lanlan_reminder_t *reminder) {
    if (!reminder) return false;
    if (lanlan_record_id_is_zero(reminder->id)) return false;
    if (reminder->version == 0) return false;
    if (reminder->category < 0 || reminder->category >= LANLAN_CAT_COUNT) return false;
    if (reminder->subitem < 0 || reminder->subitem >= LANLAN_SUB_COUNT) return false;
    if (reminder->time_local[0] != '\0') {
        if (strlen(reminder->time_local) != 5) return false;
        if (!lanlan_time_parse_hhmm(reminder->time_local, NULL, NULL)) return false;
    }
    if (strnlen(reminder->custom_name, sizeof(reminder->custom_name))
        >= sizeof(reminder->custom_name)) {
        return false;
    }
    if (lanlan_utf8_length(reminder->custom_name) > LANLAN_RECORD_CUSTOM_CHARS) return false;
    return true;
}

/* Proves the in-memory cache satisfies every bound the codec and the UI rely
 * on. This is the gate the commit path uses, so a corrupt staging buffer is
 * never published even if the merge logic had a bug. */
bool lanlan_cache_is_consistent(const lanlan_cache_t *cache) {
    if (!cache) return false;
    if (cache->record_count > LANLAN_CACHE_MAX_RECORDS) return false;
    if (cache->tombstone_count > LANLAN_CACHE_MAX_TOMBSTONES) return false;
    if (cache->reminder_count > LANLAN_CACHE_MAX_REMINDERS) return false;
    for (uint32_t i = 0; i < cache->record_count; ++i) {
        if (lanlan_record_is_valid(&cache->records[i]) != LANLAN_RECORD_OK) return false;
        for (uint32_t j = i + 1; j < cache->record_count; ++j) {
            if (lanlan_record_id_equal(cache->records[i].id, cache->records[j].id)) return false;
        }
        if (lanlan_cache_is_tombstoned(cache, cache->records[i].id)) return false;
    }
    for (uint32_t i = 0; i < cache->tombstone_count; ++i) {
        if (lanlan_record_id_is_zero(cache->tombstones[i])) return false;
        for (uint32_t j = i + 1; j < cache->tombstone_count; ++j) {
            if (lanlan_record_id_equal(cache->tombstones[i], cache->tombstones[j])) return false;
        }
    }
    for (uint32_t i = 0; i < cache->reminder_count; ++i) {
        if (!reminder_is_valid(&cache->reminders[i])) return false;
        for (uint32_t j = i + 1; j < cache->reminder_count; ++j) {
            if (lanlan_record_id_equal(cache->reminders[i].id, cache->reminders[j].id)) return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- codec -- */

static void encode_record(const lanlan_record_t *record, uint8_t *out) {
    memset(out, 0, LANLAN_CACHE_RECORD_ENTRY_BYTES);
    memcpy(out, record->id, LANLAN_ID_BYTES);
    put_u32(out + REC_VERSION_OFFSET, record->version);
    put_u32(out + REC_SEQ_OFFSET, record->seq);
    put_i64(out + REC_OCCURRED_OFFSET, record->occurred_epoch);
    put_u16(out + REC_TZ_OFFSET, (uint16_t)record->occurred_tz_offset_min);
    put_u16(out + REC_DURATION_OFFSET, record->duration_minutes);
    out[REC_CATEGORY_OFFSET] = (uint8_t)record->category;
    out[REC_SUBITEM_OFFSET] = (uint8_t)record->subitem;
    out[REC_CONFIDENCE_OFFSET] = (uint8_t)record->time_confidence;
    out[REC_STATUS_OFFSET] = (uint8_t)record->status;
    out[REC_UNIT_OFFSET] = (uint8_t)record->unit;
    out[REC_FLAGS_OFFSET] = (uint8_t)((record->amount_known ? REC_AMOUNT_FLAG : 0u)
                                      | (record->note_truncated ? REC_NOTE_CUT_FLAG : 0u));
    out[REC_CREATED_BY_OFFSET] = record->created_by;
    out[REC_PERFORMED_BY_OFFSET] = record->performed_by;
    put_double(out + REC_AMOUNT_OFFSET, record->amount_value);
    size_t note_len = strnlen(record->note, sizeof(record->note));
    if (note_len >= sizeof(record->note)) note_len = sizeof(record->note) - 1;
    size_t custom_len = strnlen(record->custom_name, sizeof(record->custom_name));
    if (custom_len >= sizeof(record->custom_name)) custom_len = sizeof(record->custom_name) - 1;
    memcpy(out + REC_CUSTOM_OFFSET, record->custom_name, custom_len);
    memcpy(out + REC_NOTE_OFFSET, record->note, note_len);
    out[REC_NOTE_LEN_OFFSET] = (uint8_t)note_len;
    put_u16(out + REC_CUSTOM_LEN_OFFSET, (uint16_t)custom_len);
    put_i64(out + REC_CREATED_OFFSET, record->created_epoch);
}

/* Decodes one entry. The caller has proven the whole region is in bounds and
 * the fixed entry size means this function cannot read past its own 160 bytes.
 * Returns OK only when every field passes the same validation the service
 * applies. */
static lanlan_cache_status_t decode_record(const uint8_t *in, lanlan_record_t *record) {
    memset(record, 0, sizeof(*record));
    memcpy(record->id, in, LANLAN_ID_BYTES);
    record->version = get_u32(in + REC_VERSION_OFFSET);
    record->seq = get_u32(in + REC_SEQ_OFFSET);
    record->occurred_epoch = get_i64(in + REC_OCCURRED_OFFSET);
    record->occurred_tz_offset_min = (int16_t)get_u16(in + REC_TZ_OFFSET);
    record->duration_minutes = get_u16(in + REC_DURATION_OFFSET);
    record->category = (lanlan_category_t)in[REC_CATEGORY_OFFSET];
    record->subitem = (lanlan_subitem_t)in[REC_SUBITEM_OFFSET];
    record->time_confidence = (lanlan_time_confidence_t)in[REC_CONFIDENCE_OFFSET];
    record->status = (lanlan_record_status_t)in[REC_STATUS_OFFSET];
    record->unit = (lanlan_unit_t)in[REC_UNIT_OFFSET];
    record->amount_known = (in[REC_FLAGS_OFFSET] & REC_AMOUNT_FLAG) != 0;
    record->note_truncated = (in[REC_FLAGS_OFFSET] & REC_NOTE_CUT_FLAG) != 0;
    record->created_by = in[REC_CREATED_BY_OFFSET];
    record->performed_by = in[REC_PERFORMED_BY_OFFSET];
    record->amount_value = get_double(in + REC_AMOUNT_OFFSET);
    record->created_epoch = get_i64(in + REC_CREATED_OFFSET);
    uint8_t note_len = in[REC_NOTE_LEN_OFFSET];
    if (note_len >= sizeof(record->note)) return LANLAN_CACHE_CORRUPT;
    memcpy(record->note, in + REC_NOTE_OFFSET, note_len);
    record->note[note_len] = '\0';
    uint16_t custom_len = get_u16(in + REC_CUSTOM_LEN_OFFSET);
    if (custom_len >= sizeof(record->custom_name)) return LANLAN_CACHE_CORRUPT;
    memcpy(record->custom_name, in + REC_CUSTOM_OFFSET, custom_len);
    record->custom_name[custom_len] = '\0';
    /* Reserved bytes must be zero: a non-zero value means the blob came from a
     * different writer or is damaged. */
    if (in[REC_NOTE_LEN_OFFSET + 1] != 0) return LANLAN_CACHE_CORRUPT;
    for (size_t i = REC_CUSTOM_LEN_OFFSET + 2; i < REC_CUSTOM_OFFSET; ++i) {
        if (in[i] != 0) return LANLAN_CACHE_CORRUPT;
    }
    for (size_t i = REC_CREATED_OFFSET + 8u; i < LANLAN_CACHE_RECORD_ENTRY_BYTES; ++i) {
        if (in[i] != 0) return LANLAN_CACHE_CORRUPT;
    }
    if (lanlan_record_is_valid(record) != LANLAN_RECORD_OK) return LANLAN_CACHE_CORRUPT;
    return LANLAN_CACHE_OK;
}

static void encode_reminder(const lanlan_reminder_t *reminder, uint8_t *out) {
    memset(out, 0, LANLAN_CACHE_REMINDER_ENTRY_BYTES);
    memcpy(out, reminder->id, LANLAN_ID_BYTES);
    put_u32(out + REM_VERSION_OFFSET, reminder->version);
    out[REM_CATEGORY_OFFSET] = (uint8_t)reminder->category;
    out[REM_SUBITEM_OFFSET] = (uint8_t)reminder->subitem;
    out[REM_ENABLED_OFFSET] = reminder->enabled ? 1u : 0u;
    size_t time_len = strlen(reminder->time_local);
    if (time_len > REM_TIME_FIELD_BYTES - 1) time_len = 0;
    memcpy(out + REM_TIME_OFFSET, reminder->time_local, time_len);
    out[REM_TIME_LEN_OFFSET] = (uint8_t)time_len;
    put_i64(out + REM_RUNG_OFFSET, reminder->last_rung_day);
    size_t custom_len = strnlen(reminder->custom_name, sizeof(reminder->custom_name));
    if (custom_len >= sizeof(reminder->custom_name)) custom_len = sizeof(reminder->custom_name) - 1;
    memcpy(out + REM_CUSTOM_OFFSET, reminder->custom_name, custom_len);
}

static lanlan_cache_status_t decode_reminder(const uint8_t *in, lanlan_reminder_t *reminder) {
    memset(reminder, 0, sizeof(*reminder));
    memcpy(reminder->id, in, LANLAN_ID_BYTES);
    reminder->version = get_u32(in + REM_VERSION_OFFSET);
    reminder->category = (lanlan_category_t)in[REM_CATEGORY_OFFSET];
    reminder->subitem = (lanlan_subitem_t)in[REM_SUBITEM_OFFSET];
    reminder->enabled = in[REM_ENABLED_OFFSET] != 0;
    uint8_t time_len = in[REM_TIME_LEN_OFFSET];
    if (time_len != 0 && time_len != 5) return LANLAN_CACHE_CORRUPT;
    memcpy(reminder->time_local, in + REM_TIME_OFFSET, time_len);
    reminder->time_local[time_len] = '\0';
    reminder->last_rung_day = get_i64(in + REM_RUNG_OFFSET);
    /* The custom name is a fixed field terminated by padding; the write path
     * keeps it within its 24-byte budget and unset bytes must be zero. */
    size_t custom_len = strnlen((const char *)(in + REM_CUSTOM_OFFSET),
                                sizeof(reminder->custom_name));
    if (custom_len >= sizeof(reminder->custom_name)) return LANLAN_CACHE_CORRUPT;
    memcpy(reminder->custom_name, in + REM_CUSTOM_OFFSET, custom_len);
    reminder->custom_name[custom_len] = '\0';
    for (size_t i = REM_CUSTOM_OFFSET + custom_len; i < 48u; ++i) {
        if (in[i] != 0) return LANLAN_CACHE_CORRUPT;
    }
    for (size_t i = 62u; i < LANLAN_CACHE_REMINDER_ENTRY_BYTES; ++i) {
        if (in[i] != 0) return LANLAN_CACHE_CORRUPT;
    }
    if (!reminder_is_valid(reminder)) return LANLAN_CACHE_CORRUPT;
    return LANLAN_CACHE_OK;
}

size_t lanlan_cache_encode(const lanlan_cache_t *cache, uint8_t *out, size_t out_size) {
    if (!cache || !out || out_size < LANLAN_CACHE_BLOB_BYTES) return 0;
    lanlan_cache_t normalized = *cache;
    if (normalized.record_count > LANLAN_CACHE_MAX_RECORDS) {
        normalized.record_count = LANLAN_CACHE_MAX_RECORDS;
    }
    if (normalized.tombstone_count > LANLAN_CACHE_MAX_TOMBSTONES) {
        normalized.tombstone_count = LANLAN_CACHE_MAX_TOMBSTONES;
    }
    if (normalized.reminder_count > LANLAN_CACHE_MAX_REMINDERS) {
        normalized.reminder_count = LANLAN_CACHE_MAX_REMINDERS;
    }
    memset(out, 0, LANLAN_CACHE_BLOB_BYTES);
    out[0] = LANLAN_CACHE_MAGIC_0;
    out[1] = LANLAN_CACHE_MAGIC_1;
    out[2] = (uint8_t)LANLAN_CACHE_FORMAT_VERSION;
    out[3] = (uint8_t)normalized.record_count;
    out[4] = (uint8_t)normalized.tombstone_count;
    out[5] = (uint8_t)normalized.reminder_count;
    put_u32(out + 6, normalized.cursor);
    uint8_t *records = out + RECORDS_OFFSET;
    for (uint32_t i = 0; i < normalized.record_count; ++i) {
        encode_record(&normalized.records[i],
                      records + (size_t)i * LANLAN_CACHE_RECORD_ENTRY_BYTES);
    }
    uint8_t *tombstones = out + TOMBSTONES_OFFSET;
    for (uint32_t i = 0; i < normalized.tombstone_count; ++i) {
        memcpy(tombstones + (size_t)i * LANLAN_CACHE_TOMBSTONE_ENTRY_BYTES,
               normalized.tombstones[i], LANLAN_ID_BYTES);
    }
    uint8_t *reminders = out + REMINDERS_OFFSET;
    for (uint32_t i = 0; i < normalized.reminder_count; ++i) {
        encode_reminder(&normalized.reminders[i],
                        reminders + (size_t)i * LANLAN_CACHE_REMINDER_ENTRY_BYTES);
    }
    /* One CRC over the header and every region; the fixed offsets mean a
     * damaged count cannot move what the CRC covers. */
    put_u32(out + CRC_TRAILER_OFFSET, crc32_of(out, CRC_TRAILER_OFFSET));
    return LANLAN_CACHE_BLOB_BYTES;
}

lanlan_cache_status_t lanlan_cache_decode(const uint8_t *data, size_t size,
                                          lanlan_cache_decode_result_t *result) {
    if (!result) return LANLAN_CACHE_CORRUPT;
    memset(result, 0, sizeof(*result));
    if (!data || size < LANLAN_CACHE_HEADER_BYTES) {
        result->status = LANLAN_CACHE_TRUNCATED;
        return result->status;
    }
    if (size < LANLAN_CACHE_BLOB_BYTES) {
        /* A short value is reported apart from damage: the caller rebuilds, and
         * the two cases are distinguished for diagnostics. */
        result->status = LANLAN_CACHE_TRUNCATED;
        return result->status;
    }
    if (data[0] != LANLAN_CACHE_MAGIC_0 || data[1] != LANLAN_CACHE_MAGIC_1) {
        result->status = LANLAN_CACHE_CORRUPT;
        return result->status;
    }
    /* The version is checked before anything else the format might have moved. */
    if (data[2] != (uint8_t)LANLAN_CACHE_FORMAT_VERSION) {
        result->status = LANLAN_CACHE_VERSION_MISMATCH;
        return result->status;
    }
    if (crc32_of(data, CRC_TRAILER_OFFSET) != get_u32(data + CRC_TRAILER_OFFSET)) {
        result->status = LANLAN_CACHE_CORRUPT;
        return result->status;
    }
    uint32_t record_count = data[3];
    uint32_t tombstone_count = data[4];
    uint32_t reminder_count = data[5];
    if (record_count > LANLAN_CACHE_MAX_RECORDS || tombstone_count > LANLAN_CACHE_MAX_TOMBSTONES
        || reminder_count > LANLAN_CACHE_MAX_REMINDERS) {
        result->status = LANLAN_CACHE_CORRUPT;
        return result->status;
    }
    result->cache.record_count = record_count;
    result->cache.tombstone_count = tombstone_count;
    result->cache.reminder_count = reminder_count;
    result->cache.cursor = get_u32(data + 6);

    const uint8_t *records = data + RECORDS_OFFSET;
    for (uint32_t i = 0; i < LANLAN_CACHE_MAX_RECORDS; ++i) {
        const uint8_t *entry = records + (size_t)i * LANLAN_CACHE_RECORD_ENTRY_BYTES;
        if (i < record_count) {
            if (decode_record(entry, &result->cache.records[i]) != LANLAN_CACHE_OK) {
                memset(&result->cache, 0, sizeof(result->cache));
                result->status = LANLAN_CACHE_CORRUPT;
                return result->status;
            }
        } else {
            /* Unused slots must be zero; otherwise the encoder and decoder
             * disagree about the layout. */
            for (size_t b = 0; b < LANLAN_CACHE_RECORD_ENTRY_BYTES; ++b) {
                if (entry[b] != 0) {
                    memset(&result->cache, 0, sizeof(result->cache));
                    result->status = LANLAN_CACHE_CORRUPT;
                    return result->status;
                }
            }
        }
    }
    const uint8_t *tombstones = data + TOMBSTONES_OFFSET;
    for (uint32_t i = 0; i < LANLAN_CACHE_MAX_TOMBSTONES; ++i) {
        const uint8_t *entry = tombstones + (size_t)i * LANLAN_CACHE_TOMBSTONE_ENTRY_BYTES;
        bool zero = true;
        for (size_t b = 0; b < LANLAN_CACHE_TOMBSTONE_ENTRY_BYTES; ++b) {
            if (entry[b] != 0) {
                zero = false;
                break;
            }
        }
        if (i < tombstone_count) {
            if (zero) { /* an all-zero id is not a valid tombstone */
                memset(&result->cache, 0, sizeof(result->cache));
                result->status = LANLAN_CACHE_CORRUPT;
                return result->status;
            }
            memcpy(result->cache.tombstones[i], entry, LANLAN_ID_BYTES);
        } else if (!zero) {
            memset(&result->cache, 0, sizeof(result->cache));
            result->status = LANLAN_CACHE_CORRUPT;
            return result->status;
        }
    }
    const uint8_t *reminders = data + REMINDERS_OFFSET;
    for (uint32_t i = 0; i < LANLAN_CACHE_MAX_REMINDERS; ++i) {
        const uint8_t *entry = reminders + (size_t)i * LANLAN_CACHE_REMINDER_ENTRY_BYTES;
        if (i < reminder_count) {
            if (decode_reminder(entry, &result->cache.reminders[i]) != LANLAN_CACHE_OK) {
                memset(&result->cache, 0, sizeof(result->cache));
                result->status = LANLAN_CACHE_CORRUPT;
                return result->status;
            }
        } else {
            for (size_t b = 0; b < LANLAN_CACHE_REMINDER_ENTRY_BYTES; ++b) {
                if (entry[b] != 0) {
                    memset(&result->cache, 0, sizeof(result->cache));
                    result->status = LANLAN_CACHE_CORRUPT;
                    return result->status;
                }
            }
        }
    }

    /* Relationship checks that need the whole cache: ordering, uniqueness and
     * "no live record is tombstoned". */
    for (uint32_t i = 1; i < record_count; ++i) {
        if (lanlan_cache_record_compare(&result->cache.records[i - 1],
                                        &result->cache.records[i]) > 0) {
            memset(&result->cache, 0, sizeof(result->cache));
            result->status = LANLAN_CACHE_CORRUPT;
            return result->status;
        }
    }
    if (!lanlan_cache_is_consistent(&result->cache)) {
        memset(&result->cache, 0, sizeof(result->cache));
        result->status = LANLAN_CACHE_CORRUPT;
        return result->status;
    }
    uint32_t total = record_count + tombstone_count + reminder_count;
    result->status = total == 0 ? LANLAN_CACHE_EMPTY : LANLAN_CACHE_OK;
    return result->status;
}

/* -------------------------------------------------------- merge machinery -- */

/* Removes every staged record whose id is in the revoked list and reports the
 * ids that were actually present. */
static void apply_revocations(lanlan_cache_t *staging, const lanlan_sync_batch_t *batch,
                              lanlan_cache_revoked_report_t *removed) {
    for (int32_t r = 0; r < batch->revoked_count; ++r) {
        const uint8_t *id = batch->revoked[r];
        for (uint32_t i = 0; i < staging->record_count; ++i) {
            if (!lanlan_record_id_equal(staging->records[i].id, id)) continue;
            if (removed && removed->count < LANLAN_CACHE_MAX_RECORDS) {
                memcpy(removed->ids[removed->count++], id, LANLAN_ID_BYTES);
            }
            memmove(&staging->records[i], &staging->records[i + 1],
                    (staging->record_count - i - 1) * sizeof(staging->records[0]));
            --staging->record_count;
            break;
        }
        /* The tombstone is recorded even when the record was not cached, so a
         * later partial batch cannot resurrect it. */
        lanlan_cache_add_tombstone(staging, id);
    }
}

static void merge_records(lanlan_cache_t *staging, const lanlan_sync_batch_t *batch) {
    for (int32_t r = 0; r < batch->record_count; ++r) {
        const lanlan_record_t *incoming = &batch->records[r];
        if (lanlan_cache_is_tombstoned(staging, incoming->id)) continue;
        int existing = lanlan_cache_find_record(staging, incoming->id);
        if (existing >= 0) {
            /* Append-only revisions: keep the highest version the device has, so
             * an out-of-order or replayed batch cannot downgrade state. */
            if (incoming->version > staging->records[existing].version) {
                staging->records[existing] = *incoming;
            }
            continue;
        }
        if (staging->record_count >= LANLAN_CACHE_MAX_RECORDS) {
            /* Full cache: only accept an incoming record that beats the current
             * oldest one. The sort below drops the loser and reports it, so the
             * eviction is visible instead of silent. */
            lanlan_record_t oldest = staging->records[0];
            for (uint32_t i = 1; i < LANLAN_CACHE_MAX_RECORDS; ++i) {
                if (lanlan_cache_record_compare(&staging->records[i], &oldest) > 0) {
                    oldest = staging->records[i];
                }
            }
            if (lanlan_cache_record_compare(incoming, &oldest) >= 0) continue;
        }
        staging->records[staging->record_count++] = *incoming;
    }
}

static void merge_reminders(lanlan_cache_t *staging, const lanlan_sync_batch_t *batch) {
    for (int32_t r = 0; r < batch->reminder_count; ++r) {
        const lanlan_reminder_t *incoming = &batch->reminders[r];
        int existing = lanlan_cache_find_reminder(staging, incoming->id);
        if (existing >= 0) {
            if (incoming->version >= staging->reminders[existing].version) {
                /* Preserve the locally persisted rung instance: the server has
                 * no column for it and a refresh must not re-arm the reminder. */
                int64_t last_rung = staging->reminders[existing].last_rung_day;
                staging->reminders[existing] = *incoming;
                staging->reminders[existing].last_rung_day = last_rung;
            }
            continue;
        }
        if (staging->reminder_count >= LANLAN_CACHE_MAX_REMINDERS) continue;
        staging->reminders[staging->reminder_count++] = *incoming;
        staging->reminders[staging->reminder_count - 1].last_rung_day = LANLAN_REMINDER_NO_RUNG;
    }
}

lanlan_cache_status_t lanlan_cache_apply_batch(const lanlan_cache_t *live,
                                               const lanlan_sync_batch_t *batch,
                                               lanlan_cache_t *staging,
                                               lanlan_cache_drop_report_t *drops,
                                               lanlan_cache_revoked_report_t *removed) {
    if (!live || !batch || !staging || staging == live) return LANLAN_CACHE_CORRUPT;
    if (batch->record_count < 0 || batch->record_count > LANLAN_CACHE_MAX_RECORDS
        || batch->reminder_count < 0 || batch->reminder_count > LANLAN_CACHE_MAX_REMINDERS
        || batch->revoked_count < 0 || batch->revoked_count > LANLAN_CACHE_MAX_RECORDS) {
        return LANLAN_CACHE_CORRUPT;
    }
    /* Every incoming entry must be valid before anything is merged, so one bad
     * entry rejects the whole batch instead of half-applying it. */
    for (int32_t i = 0; i < batch->record_count; ++i) {
        if (lanlan_record_is_valid(&batch->records[i]) != LANLAN_RECORD_OK) {
            return LANLAN_CACHE_CORRUPT;
        }
    }
    for (int32_t i = 0; i < batch->reminder_count; ++i) {
        if (!reminder_is_valid(&batch->reminders[i])) return LANLAN_CACHE_CORRUPT;
    }
    if (!lanlan_cache_is_consistent(live)) return LANLAN_CACHE_CORRUPT;

    lanlan_cache_t work = *live;
    if (drops) memset(drops, 0, sizeof(*drops));
    if (removed) memset(removed, 0, sizeof(*removed));

    apply_revocations(&work, batch, removed);
    merge_records(&work, batch);
    merge_reminders(&work, batch);
    lanlan_cache_sort_records(&work, drops);
    qsort(work.reminders, work.reminder_count, sizeof(work.reminders[0]), reminder_compare);
    work.cursor = batch->cursor;
    if (!lanlan_cache_is_consistent(&work)) return LANLAN_CACHE_CORRUPT;
    *staging = work;
    return LANLAN_CACHE_OK;
}

bool lanlan_cache_commit(lanlan_cache_t *live, const lanlan_cache_t *staging, uint32_t *cursor,
                         uint32_t new_cursor, bool confirm) {
    if (!live || !staging || staging == live || !confirm) return false;
    if (!lanlan_cache_is_consistent(staging)) return false;
    *live = *staging;
    live->cursor = new_cursor;
    if (cursor) *cursor = new_cursor;
    return true;
}

/* --------------------------------------------------------------- eviction -- */

bool lanlan_cache_needs_evict(const lanlan_cache_t *cache) {
    return cache && cache->record_count > LANLAN_CACHE_MAX_RECORDS;
}

int lanlan_cache_evict(lanlan_cache_t *cache, lanlan_cache_drop_report_t *drops) {
    if (!cache) return 0;
    if (drops) memset(drops, 0, sizeof(*drops));
    if (cache->record_count <= LANLAN_CACHE_MAX_RECORDS) return 0;
    int dropped = (int)(cache->record_count - LANLAN_CACHE_MAX_RECORDS);
    lanlan_cache_sort_records(cache, drops);
    if (cache->record_count != LANLAN_CACHE_MAX_RECORDS) {
        /* Defensive: never report a success that did not trim the cache. */
        return 0;
    }
    return dropped;
}

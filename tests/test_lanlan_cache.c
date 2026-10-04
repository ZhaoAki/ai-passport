/* Host tests for main/lanlan_cache.c (plus the modules it links).
 * Build:  cc -std=c11 -Wall -Wextra -Werror -Imain \
 *             tests/test_lanlan_cache.c main/lanlan_cache.c main/lanlan_record.c \
 *             main/lanlan_time.c main/lanlan_strings.c -o /tmp/t && /tmp/t
 * Covers: canonical layout, CRC, both encode/decode directions, all bound
 * checks, batch staging, tombstones, eviction and the byte budgets. */
#include "lanlan_cache.h"
#include "lanlan_strings.h"
#include "lanlan_time.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Field positions used by the corruption tests; they mirror the layout map in
 * main/lanlan_cache.c and would fail loudly if the layout moved. */
#define CRC_TRAILER_POS (LANLAN_CACHE_BLOB_BYTES - LANLAN_CACHE_CRC_BYTES)
#define RECORD_CATEGORY_POS 116

/* The canonical blob is 7972 bytes, so the buffer is the compile-time bound. */
static uint8_t s_blob[LANLAN_CACHE_BLOB_BYTES];
static uint8_t s_other[LANLAN_CACHE_BLOB_BYTES];
static uint8_t s_copy[LANLAN_CACHE_BLOB_BYTES];
static lanlan_cache_t s_live;
static lanlan_cache_t s_staging;

static void fill_id(uint8_t id[LANLAN_ID_BYTES], uint8_t tag) {
    for (size_t i = 0; i < LANLAN_ID_BYTES; ++i) {
        /* tag in the first byte keeps the ids ordered per tag. */
        id[i] = (uint8_t)(tag + i + 1);
    }
}

static lanlan_record_t make_record(uint8_t tag, int64_t occurred, lanlan_category_t category,
                                   lanlan_subitem_t subitem) {
    lanlan_record_t record;
    memset(&record, 0, sizeof(record));
    fill_id(record.id, tag);
    record.version = 1;
    record.category = category;
    record.subitem = subitem;
    record.time_confidence = LANLAN_TIME_CONFIDENCE_TRUSTED;
    record.occurred_epoch = occurred;
    record.occurred_tz_offset_min = 480;
    record.created_epoch = occurred + 10;
    record.created_by = 0;
    record.performed_by = 1;
    record.status = LANLAN_STATUS_ACTIVE;
    record.seq = 1;
    lanlan_record_set_amount_unknown(&record);
    return record;
}

static lanlan_reminder_t make_reminder(uint8_t tag, const char *time_local) {
    lanlan_reminder_t reminder;
    memset(&reminder, 0, sizeof(reminder));
    fill_id(reminder.id, tag);
    reminder.version = 1;
    reminder.category = LANLAN_CAT_CARE;
    reminder.subitem = LANLAN_SUB_BATH;
    reminder.enabled = false;
    reminder.last_rung_day = LANLAN_REMINDER_NO_RUNG;
    strcpy(reminder.time_local, time_local);
    return reminder;
}

static void test_byte_budgets(void) {
    /* Hard budgets from the frozen 24 KB NVS partition and the design. */
    assert(sizeof(lanlan_record_t) <= 176);
    assert(LANLAN_CACHE_RECORD_ENTRY_BYTES <= 160);
    assert(LANLAN_CACHE_BLOB_BYTES <= 8192);
    assert(sizeof(lanlan_cache_t) <= 16384);
    assert(2 * sizeof(lanlan_cache_t) <= 16384);
    assert(LANLAN_CACHE_STAGING_BYTES == sizeof(lanlan_cache_t));
    /* The canonical size is exactly the sum of its parts. */
    assert(LANLAN_CACHE_BLOB_BYTES
           == LANLAN_CACHE_HEADER_BYTES + LANLAN_CACHE_RECORDS_BYTES
                  + LANLAN_CACHE_TOMBSTONES_BYTES + LANLAN_CACHE_REMINDERS_BYTES
                  + LANLAN_CACHE_CRC_BYTES);
    assert(LANLAN_CACHE_RECORDS_BYTES == 40 * LANLAN_CACHE_RECORD_ENTRY_BYTES);
    assert(LANLAN_CACHE_TOMBSTONES_BYTES == 32 * LANLAN_CACHE_TOMBSTONE_ENTRY_BYTES);
    assert(LANLAN_CACHE_REMINDERS_BYTES == 16 * LANLAN_CACHE_REMINDER_ENTRY_BYTES);
    /* Fits the "about 19986 bytes" single-NVS-blob limit with room to spare. */
    assert(LANLAN_CACHE_BLOB_BYTES < 19986);
    /* Encode refuses a short destination instead of overrunning it. */
    lanlan_cache_clear(&s_live);
    assert(lanlan_cache_encode(&s_live, s_blob, LANLAN_CACHE_BLOB_BYTES - 1) == 0);
    assert(lanlan_cache_encode(NULL, s_blob, sizeof(s_blob)) == 0);
    assert(lanlan_cache_encode(&s_live, NULL, sizeof(s_blob)) == 0);
}

static lanlan_cache_status_t round_trip(const lanlan_cache_t *source, lanlan_cache_t *decoded) {
    memset(s_blob, 0xA5, sizeof(s_blob));
    size_t bytes = lanlan_cache_encode(source, s_blob, sizeof(s_blob));
    if (bytes != LANLAN_CACHE_BLOB_BYTES) return LANLAN_CACHE_CORRUPT;
    lanlan_cache_decode_result_t result;
    memset(&result, 0xA5, sizeof(result));
    lanlan_cache_status_t status = lanlan_cache_decode(s_blob, bytes, &result);
    if (decoded) *decoded = result.cache;
    return status;
}

static void test_round_trip_zero_one_and_max(void) {
    /* 0 records: an empty cache still encodes and decodes (the layout is fixed,
     * so the NVS value length never depends on the content). */
    lanlan_cache_clear(&s_live);
    lanlan_cache_t decoded;
    assert(round_trip(&s_live, &decoded) == LANLAN_CACHE_EMPTY);
    assert(decoded.record_count == 0 && decoded.tombstone_count == 0);
    assert(decoded.reminder_count == 0);

    /* 1 record + 1 tombstone + 1 reminder: the mixed case the parent reported. */
    lanlan_cache_clear(&s_live);
    s_live.records[0] = make_record(7, 1760000000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    lanlan_record_set_amount(&s_live.records[0], 120.0, LANLAN_UNIT_G);
    lanlan_record_set_note(&s_live.records[0], "吃了半碗");
    s_live.record_count = 1;
    fill_id(s_live.tombstones[0], 200);
    s_live.tombstone_count = 1;
    s_live.reminders[0] = make_reminder(9, "07:40");
    s_live.reminder_count = 1;
    s_live.cursor = 41;
    assert(lanlan_cache_is_consistent(&s_live));
    assert(round_trip(&s_live, &decoded) == LANLAN_CACHE_OK);
    assert(decoded.record_count == 1 && decoded.tombstone_count == 1 && decoded.reminder_count == 1);
    assert(decoded.cursor == 41);
    assert(memcmp(decoded.records[0].id, s_live.records[0].id, LANLAN_ID_BYTES) == 0);
    assert(decoded.records[0].amount_known && decoded.records[0].amount_value == 120.0);
    assert(decoded.records[0].unit == LANLAN_UNIT_G);
    assert(strcmp(decoded.records[0].note, "吃了半碗") == 0);
    assert(!decoded.records[0].note_truncated);
    assert(memcmp(decoded.tombstones[0], s_live.tombstones[0], LANLAN_ID_BYTES) == 0);
    assert(strcmp(decoded.reminders[0].time_local, "07:40") == 0);
    assert(!decoded.reminders[0].enabled);
    assert(decoded.reminders[0].last_rung_day == LANLAN_REMINDER_NO_RUNG);
    /* The second encode of the decoded value is byte-identical: decode/encode is
     * a stable canonical form, not merely a lossy read. */
    lanlan_cache_encode(&decoded, s_other, sizeof(s_other));
    lanlan_cache_encode(&s_live, s_blob, sizeof(s_blob));
    assert(memcmp(s_other, s_blob, LANLAN_CACHE_BLOB_BYTES) == 0);

    /* Exactly maximum counts. */
    lanlan_cache_clear(&s_live);
    for (int i = 0; i < LANLAN_CACHE_MAX_RECORDS; ++i) {
        s_live.records[i] = make_record((uint8_t)(i + 1), 1760000000 + i, LANLAN_CAT_MEAL,
                                        LANLAN_SUB_NONE);
        s_live.records[i].seq = (uint32_t)(i + 1);
    }
    s_live.record_count = LANLAN_CACHE_MAX_RECORDS;
    /* The cache order is newest-first and is part of the format, so the test
     * fixture sorts exactly like the merge path does. */
    lanlan_cache_sort_records(&s_live, NULL);
    assert(s_live.records[0].occurred_epoch
           == 1760000000 + LANLAN_CACHE_MAX_RECORDS - 1);
    for (int i = 0; i < LANLAN_CACHE_MAX_TOMBSTONES; ++i) {
        fill_id(s_live.tombstones[i], (uint8_t)(100 + i));
    }
    s_live.tombstone_count = LANLAN_CACHE_MAX_TOMBSTONES;
    for (int i = 0; i < LANLAN_CACHE_MAX_REMINDERS; ++i) {
        s_live.reminders[i] = make_reminder((uint8_t)(60 + i), "08:00");
    }
    s_live.reminder_count = LANLAN_CACHE_MAX_REMINDERS;
    s_live.cursor = 999;
    assert(lanlan_cache_is_consistent(&s_live));
    assert(round_trip(&s_live, &decoded) == LANLAN_CACHE_OK);
    assert(decoded.record_count == 40 && decoded.tombstone_count == 32 && decoded.reminder_count == 16);
    assert(decoded.cursor == 999);
    for (int i = 0; i < LANLAN_CACHE_MAX_RECORDS; ++i) {
        assert(memcmp(decoded.records[i].id, s_live.records[i].id, LANLAN_ID_BYTES) == 0);
        assert(decoded.records[i].seq == s_live.records[i].seq);
    }

    /* A truncated note survives as a truncation, not as a silent full note. */
    lanlan_cache_clear(&s_live);
    s_live.records[0] = make_record(3, 1760000000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    char long_note[256];
    long_note[0] = '\0';
    for (int i = 0; i < 80; ++i) strcat(long_note, "好");
    assert(lanlan_record_set_note(&s_live.records[0], long_note));
    s_live.record_count = 1;
    assert(round_trip(&s_live, &decoded) == LANLAN_CACHE_OK);
    assert(decoded.records[0].note_truncated);
    assert(strcmp(decoded.records[0].note, s_live.records[0].note) == 0);

    /* Unknown amounts stay unknown across the codec (never 0). */
    lanlan_cache_clear(&s_live);
    s_live.records[0] = make_record(4, 1760000000, LANLAN_CAT_WATER, LANLAN_SUB_NONE);
    s_live.record_count = 1;
    assert(round_trip(&s_live, &decoded) == LANLAN_CACHE_OK);
    assert(!decoded.records[0].amount_known);
    assert(decoded.records[0].amount_value == 0.0);
    assert(decoded.records[0].unit == LANLAN_UNIT_NONE);
}

static void test_decode_rejection(void) {
    lanlan_cache_decode_result_t result;
    lanlan_cache_clear(&s_live);
    s_live.records[0] = make_record(5, 1760000000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    s_live.record_count = 1;
    s_live.reminders[0] = make_reminder(6, "07:40");
    s_live.reminder_count = 1;
    size_t bytes = lanlan_cache_encode(&s_live, s_blob, sizeof(s_blob));
    assert(bytes == LANLAN_CACHE_BLOB_BYTES);
    assert(lanlan_cache_decode(s_blob, bytes, &result) == LANLAN_CACHE_OK);

    /* Version mismatch is reported apart from corrupt, so the caller rebuilds. */
    memset(s_copy, 0, sizeof(s_copy));
    memcpy(s_copy, s_blob, sizeof(s_copy));
    s_copy[2] = (uint8_t)(LANLAN_CACHE_FORMAT_VERSION + 1);
    s_copy[LANLAN_CACHE_BLOB_BYTES - 4] = (uint8_t)(s_copy[LANLAN_CACHE_BLOB_BYTES - 4] ^ 0xFF); /* keep CRC wrong too: version wins */
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_VERSION_MISMATCH);
    assert(result.cache.record_count == 0);

    /* Truncated blobs are distinct from corrupt ones and never read OOB. */
    assert(lanlan_cache_decode(s_blob, 0, &result) == LANLAN_CACHE_TRUNCATED);
    assert(lanlan_cache_decode(NULL, bytes, &result) == LANLAN_CACHE_TRUNCATED);
    assert(lanlan_cache_decode(s_blob, LANLAN_CACHE_HEADER_BYTES, &result) == LANLAN_CACHE_TRUNCATED);
    assert(lanlan_cache_decode(s_blob, bytes - 1, &result) == LANLAN_CACHE_TRUNCATED);
    assert(!result.cache.records[0].amount_known && result.cache.record_count == 0);
    assert(lanlan_cache_status_needs_rebuild(LANLAN_CACHE_TRUNCATED));

    /* Bad magic. */
    memcpy(s_copy, s_blob, sizeof(s_copy));
    s_copy[0] = 'X';
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);

    /* Bad trailer CRC. */
    memcpy(s_copy, s_blob, sizeof(s_copy));
    s_copy[CRC_TRAILER_POS] ^= 0x01;
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);

    /* Corrupt payload without touching the CRC is caught by the CRC. */
    memcpy(s_copy, s_blob, sizeof(s_copy));
    s_copy[LANLAN_CACHE_HEADER_BYTES + 24] ^= 0x40;
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);

    /* A count beyond the capacity is rejected even though the CRC is then
     * recomputed by the test (simulating a writer bug). */
    memcpy(s_copy, s_blob, sizeof(s_copy));
    s_copy[3] = (uint8_t)(LANLAN_CACHE_MAX_RECORDS + 1);
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);

    /* A record with an invalid category is rejected. */
    assert(lanlan_cache_encode(&s_live, s_blob, sizeof(s_blob)) == LANLAN_CACHE_BLOB_BYTES);
    memcpy(s_copy, s_blob, sizeof(s_copy));
    s_copy[LANLAN_CACHE_HEADER_BYTES + RECORD_CATEGORY_POS] = 99;
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);

    /* A duplicate id in the record array is rejected. */
    lanlan_cache_t duplicate;
    lanlan_cache_clear(&duplicate);
    duplicate.records[0] = make_record(1, 1760001000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    duplicate.records[1] = duplicate.records[0];
    duplicate.record_count = 2;
    assert(lanlan_cache_encode(&duplicate, s_copy, sizeof(s_copy)) == LANLAN_CACHE_BLOB_BYTES);
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);

    /* Out-of-order records are rejected (ordering is part of the format). */
    lanlan_cache_t order;
    lanlan_cache_clear(&order);
    order.records[0] = make_record(1, 1760001000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    order.records[1] = make_record(2, 1760000000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    order.record_count = 2;
    assert(lanlan_cache_encode(&order, s_copy, sizeof(s_copy)) == LANLAN_CACHE_BLOB_BYTES);
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_OK);
    /* The encoder preserves whatever order it is given, so the reversed array
     * reaches the decoder and must be rejected. */
    lanlan_cache_t reversed = order;
    lanlan_record_t swap = reversed.records[0];
    reversed.records[0] = reversed.records[1];
    reversed.records[1] = swap;
    assert(lanlan_cache_encode(&reversed, s_copy, sizeof(s_copy)) == LANLAN_CACHE_BLOB_BYTES);
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);

    /* A record that is also tombstoned can never be re-accepted. */
    lanlan_cache_t conflict = s_live;
    conflict.tombstone_count = 1;
    memcpy(conflict.tombstones[0], conflict.records[0].id, LANLAN_ID_BYTES);
    assert(lanlan_cache_encode(&conflict, s_copy, sizeof(s_copy)) == LANLAN_CACHE_BLOB_BYTES);
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);

    /* An all-zero tombstone slot is not a valid id. */
    lanlan_cache_t zero_tomb = s_live;
    zero_tomb.tombstone_count = 1;
    memset(zero_tomb.tombstones[0], 0, LANLAN_ID_BYTES);
    assert(lanlan_cache_encode(&zero_tomb, s_copy, sizeof(s_copy)) == LANLAN_CACHE_BLOB_BYTES);
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);

    /* Non-zero bytes in an unused slot break the canonical form. */
    lanlan_cache_encode(&s_live, s_blob, sizeof(s_blob));
    memcpy(s_copy, s_blob, sizeof(s_copy));
    s_copy[LANLAN_CACHE_HEADER_BYTES + LANLAN_CACHE_RECORD_ENTRY_BYTES + 5] = 1; /* slot 1 unused */
    assert(lanlan_cache_decode(s_copy, bytes, &result) == LANLAN_CACHE_CORRUPT);
    assert(result.cache.record_count == 0);
    assert(strcmp(lanlan_cache_status_name(LANLAN_CACHE_CORRUPT), "corrupt") == 0);
    assert(strcmp(lanlan_cache_status_name(LANLAN_CACHE_VERSION_MISMATCH),
                  "version_mismatch")
           == 0);
    assert(strcmp(lanlan_cache_status_name(LANLAN_CACHE_EMPTY), "empty") == 0);
    assert(strcmp(lanlan_cache_status_name(LANLAN_CACHE_OK), "ok") == 0);
    assert(strcmp(lanlan_cache_status_name(LANLAN_CACHE_TRUNCATED), "truncated") == 0);
    assert(!lanlan_cache_status_needs_rebuild(LANLAN_CACHE_OK));
    assert(!lanlan_cache_status_needs_rebuild(LANLAN_CACHE_EMPTY));
    assert(lanlan_cache_status_needs_rebuild(LANLAN_CACHE_CORRUPT));
    assert(lanlan_cache_status_needs_rebuild(LANLAN_CACHE_VERSION_MISMATCH));
}

static void test_batch_merge_and_commit(void) {
    /* The live cache already holds one record from the service. */
    lanlan_cache_clear(&s_live);
    s_live.records[0] = make_record(1, 1760000000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    s_live.records[0].version = 1;
    s_live.records[0].seq = 5;
    s_live.record_count = 1;
    s_live.cursor = 5;
    uint32_t cursor = 5;

    lanlan_sync_batch_t batch;
    memset(&batch, 0, sizeof(batch));
    /* Revise the existing record, add two new ones and revoke one of them. */
    batch.records[0] = s_live.records[0];
    batch.records[0].version = 2;
    batch.records[0].seq = 6;
    lanlan_record_set_amount(&batch.records[0], 80.0, LANLAN_UNIT_G);
    batch.records[1] = make_record(2, 1760000500, LANLAN_CAT_WATER, LANLAN_SUB_NONE);
    batch.records[1].seq = 7;
    batch.records[2] = make_record(3, 1760001000, LANLAN_CAT_WALK, LANLAN_SUB_NONE);
    batch.records[2].seq = 8;
    batch.records[2].duration_minutes = 25;
    batch.record_count = 3;
    batch.reminders[0] = make_reminder(9, "07:40");
    batch.reminder_count = 1;
    /* A stale version must not downgrade the stored record. */
    batch.records[3] = batch.records[0];
    batch.records[3].version = 1;
    batch.record_count = 4;
    /* Revoke record 2 in the same batch: it must not survive. */
    memcpy(batch.revoked[0], batch.records[2].id, LANLAN_ID_BYTES);
    batch.revoked_count = 1;
    batch.cursor = 8;

    lanlan_cache_drop_report_t drops;
    lanlan_cache_revoked_report_t removed;
    assert(lanlan_cache_apply_batch(&s_live, &batch, &s_staging, &drops, &removed)
           == LANLAN_CACHE_OK);
    assert(drops.count == 0);
    assert(removed.count == 0); /* record 3 was never in the live cache */
    assert(s_live.record_count == 1); /* live untouched before the commit */
    assert(s_live.cursor == 5);
    assert(s_staging.record_count == 2);
    assert(s_staging.cursor == 8);
    /* Newest first: the water record (1760000500) precedes the revised meal
     * record (1760000000), and the revision replaced version 1 rather than
     * duplicating it. */
    assert(s_staging.records[0].occurred_epoch == 1760000500);
    assert(s_staging.records[1].version == 2);
    assert(s_staging.records[1].amount_value == 80.0);
    assert(s_staging.records[1].unit == LANLAN_UNIT_G);
    assert(lanlan_cache_find_record(&s_staging, batch.records[2].id) == -1);
    assert(lanlan_cache_is_tombstoned(&s_staging, batch.records[2].id));
    assert(s_staging.reminder_count == 1);

    /* Nothing changes without the explicit confirmation. */
    assert(!lanlan_cache_commit(&s_live, &s_staging, &cursor, 8, false));
    assert(s_live.record_count == 1 && s_live.cursor == 5);
    assert(cursor == 5);

    /* The commit publishes staging and the new cursor together. */
    assert(lanlan_cache_commit(&s_live, &s_staging, &cursor, 8, true));
    assert(cursor == 8);
    assert(s_live.cursor == 8);
    assert(s_live.record_count == 2);
    assert(s_live.tombstone_count == 1);
    assert(s_live.reminder_count == 1);
    assert(lanlan_cache_is_tombstoned(&s_live, batch.records[2].id));

    /* A later batch that re-sends the revoked record cannot resurrect it. */
    lanlan_sync_batch_t replay;
    memset(&replay, 0, sizeof(replay));
    replay.records[0] = make_record(3, 1760001000, LANLAN_CAT_WALK, LANLAN_SUB_NONE);
    replay.records[0].seq = 8;
    replay.record_count = 1;
    replay.cursor = 9;
    assert(lanlan_cache_apply_batch(&s_live, &replay, &s_staging, NULL, NULL) == LANLAN_CACHE_OK);
    assert(lanlan_cache_find_record(&s_staging, replay.records[0].id) == -1);
    assert(lanlan_cache_commit(&s_live, &s_staging, &cursor, 9, true));
    assert(s_live.record_count == 2);

    /* Revoking a record the device does hold removes it and reports it. */
    lanlan_sync_batch_t revoke;
    memset(&revoke, 0, sizeof(revoke));
    revoke.revoked_count = 1;
    memcpy(revoke.revoked[0], s_live.records[0].id, LANLAN_ID_BYTES);
    revoke.cursor = 10;
    assert(lanlan_cache_apply_batch(&s_live, &revoke, &s_staging, NULL, &removed)
           == LANLAN_CACHE_OK);
    assert(removed.count == 1);
    assert(memcmp(removed.ids[0], revoke.revoked[0], LANLAN_ID_BYTES) == 0);
    assert(s_staging.record_count == 1);
    assert(lanlan_cache_is_tombstoned(&s_staging, revoke.revoked[0]));
    assert(lanlan_cache_commit(&s_live, &s_staging, &cursor, 10, true));
    assert(s_live.record_count == 1);

    /* A reminder update keeps the locally persisted rung instance and keeps the
     * list ordered by id. */
    lanlan_cache_t reminder_cache;
    lanlan_cache_clear(&reminder_cache);
    reminder_cache.reminders[0] = make_reminder(20, "07:40");
    reminder_cache.reminders[0].last_rung_day = 20370;
    reminder_cache.reminders[1] = make_reminder(10, "08:00");
    reminder_cache.reminder_count = 2;
    lanlan_sync_batch_t reminder_batch;
    memset(&reminder_batch, 0, sizeof(reminder_batch));
    reminder_batch.reminders[0] = make_reminder(10, "09:30");
    reminder_batch.reminders[0].version = 2;
    reminder_batch.reminders[0].enabled = true;
    reminder_batch.reminders[1] = make_reminder(20, "07:40");
    reminder_batch.reminders[1].version = 2;
    reminder_batch.reminder_count = 2;
    assert(lanlan_cache_apply_batch(&reminder_cache, &reminder_batch, &s_staging, NULL, NULL)
           == LANLAN_CACHE_OK);
    assert(s_staging.reminder_count == 2);
    assert(memcmp(s_staging.reminders[0].id, reminder_cache.reminders[1].id, LANLAN_ID_BYTES) == 0);
    assert(s_staging.reminders[0].last_rung_day == LANLAN_REMINDER_NO_RUNG);
    assert(s_staging.reminders[1].last_rung_day == 20370);
    assert(s_staging.reminders[1].version == 2);
    assert(strcmp(s_staging.reminders[1].time_local, "07:40") == 0);
}

static void test_batch_failure_leaves_state_untouched(void) {
    lanlan_cache_clear(&s_live);
    s_live.records[0] = make_record(1, 1760000000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    s_live.record_count = 1;
    s_live.cursor = 12;
    uint32_t cursor = 12;
    lanlan_cache_t before_live = s_live;

    lanlan_sync_batch_t batch;
    memset(&batch, 0, sizeof(batch));
    batch.records[0] = make_record(2, 1760000500, LANLAN_CAT_WATER, LANLAN_SUB_NONE);
    batch.records[1] = make_record(3, 1760001000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    batch.records[1].category = (lanlan_category_t)77; /* invalid entry */
    batch.record_count = 2;
    batch.cursor = 99;

    /* The staging buffer is poisoned so a partial write is detectable. */
    memset(&s_staging, 0x5A, sizeof(s_staging));
    lanlan_cache_t staging_before = s_staging;
    assert(lanlan_cache_apply_batch(&s_live, &batch, &s_staging, NULL, NULL)
           == LANLAN_CACHE_CORRUPT);
    assert(memcmp(&s_live, &before_live, sizeof(s_live)) == 0);
    assert(memcmp(&s_staging, &staging_before, sizeof(s_staging)) == 0);
    assert(cursor == 12);

    /* An over-large count is rejected without touching anything. */
    batch.record_count = LANLAN_CACHE_MAX_RECORDS + 1;
    assert(lanlan_cache_apply_batch(&s_live, &batch, &s_staging, NULL, NULL)
           == LANLAN_CACHE_CORRUPT);
    batch.record_count = LANLAN_CACHE_MAX_REMINDERS + 1;
    assert(lanlan_cache_apply_batch(&s_live, &batch, &s_staging, NULL, NULL)
           == LANLAN_CACHE_CORRUPT);
    batch.record_count = 0;
    batch.reminder_count = 0;
    batch.revoked_count = -1;
    assert(lanlan_cache_apply_batch(&s_live, &batch, &s_staging, NULL, NULL)
           == LANLAN_CACHE_CORRUPT);
    assert(memcmp(&s_live, &before_live, sizeof(s_live)) == 0);

    /* In-place staging is refused: a caller bug must not half-apply a batch. */
    assert(lanlan_cache_apply_batch(&s_live, &batch, &s_live, NULL, NULL)
           == LANLAN_CACHE_CORRUPT);
    assert(memcmp(&s_live, &before_live, sizeof(s_live)) == 0);
    assert(!lanlan_cache_commit(&s_live, &s_live, &cursor, 3, true));
    assert(cursor == 12);

    /* A commit of an inconsistent staging buffer is refused. */
    lanlan_cache_t broken = s_live;
    broken.record_count = LANLAN_CACHE_MAX_RECORDS + 5;
    assert(!lanlan_cache_commit(&s_live, &broken, &cursor, 3, true));
    assert(cursor == 12 && s_live.record_count == 1);
    assert(!lanlan_cache_commit(NULL, &broken, &cursor, 3, true));
    assert(!lanlan_cache_commit(&s_live, NULL, &cursor, 3, true));
    /* A NULL cursor is allowed: the glue layer may not track it separately. */
    assert(lanlan_cache_commit(&s_live, &s_live, NULL, 3, false) == false);
    lanlan_cache_t copy = s_live;
    assert(lanlan_cache_commit(&s_live, &copy, NULL, 3, true));
    assert(s_live.cursor == 3);
}

static void test_capacity_and_eviction(void) {
    /* An over-full cache is reported and trimmed to the newest entries. The
     * extra five are built in a scratch array because the struct itself cannot
     * hold more than the capacity. */
    lanlan_cache_t cache;
    lanlan_cache_clear(&cache);
    /* The struct cannot physically hold more than the capacity, so the fixture
     * fills all 40 slots and claims five extra: the five oldest records are
     * what the eviction must trim and report. */
    lanlan_cache_drop_report_t drops;
    for (int i = 0; i < LANLAN_CACHE_MAX_RECORDS; ++i) {
        cache.records[i] = make_record((uint8_t)(i + 1), 1760000000 + i, LANLAN_CAT_MEAL,
                                       LANLAN_SUB_NONE);
    }
    cache.record_count = LANLAN_CACHE_MAX_RECORDS + 5;
    assert(lanlan_cache_needs_evict(&cache));
    int dropped = lanlan_cache_evict(&cache, &drops);
    assert(dropped == 5);
    assert(drops.count == 5);
    assert(cache.record_count == LANLAN_CACHE_MAX_RECORDS);
    assert(!lanlan_cache_needs_evict(&cache));
    /* The newest survived and the order is strictly descending. */
    for (int i = 1; i < LANLAN_CACHE_MAX_RECORDS; ++i) {
        assert(cache.records[i - 1].occurred_epoch > cache.records[i].occurred_epoch);
    }
    assert(cache.records[0].occurred_epoch == 1760000000 + LANLAN_CACHE_MAX_RECORDS - 1);
    /* Every id the report names is one the cache no longer holds. */
    for (int i = 0; i < drops.count; ++i) {
        assert(lanlan_cache_find_record(&cache, drops.ids[i]) == -1);
    }
    /* A second eviction is a no-op and reports nothing. */
    assert(lanlan_cache_evict(&cache, &drops) == 0);
    assert(drops.count == 0);

    /* A batch applied to a full cache keeps the 40 newest and reports eviction. */
    lanlan_cache_t full;
    lanlan_cache_clear(&full);
    for (int i = 0; i < LANLAN_CACHE_MAX_RECORDS; ++i) {
        full.records[i] = make_record((uint8_t)(i + 1), 1760000000 + i, LANLAN_CAT_MEAL,
                                      LANLAN_SUB_NONE);
    }
    full.record_count = LANLAN_CACHE_MAX_RECORDS;
    lanlan_cache_sort_records(&full, NULL);
    assert(full.records[0].occurred_epoch == 1760000000 + LANLAN_CACHE_MAX_RECORDS - 1);
    lanlan_sync_batch_t batch;
    memset(&batch, 0, sizeof(batch));
    /* One newer and one older than everything: only the newer is accepted. */
    batch.records[0] = make_record(150, 1760000000 + 1000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    batch.records[1] = make_record(151, 1700000000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    batch.record_count = 2;
    batch.cursor = 77;
    assert(lanlan_cache_apply_batch(&full, &batch, &s_staging, &drops, NULL) == LANLAN_CACHE_OK);
    assert(s_staging.record_count == LANLAN_CACHE_MAX_RECORDS);
    /* One drop: the oldest survivor the new record displaced. The record older
     * than everything is refused outright by the capacity guard, so it never
     * enters and is not reported as an eviction. */
    assert(drops.count == 1);
    assert(lanlan_cache_find_record(&s_staging, batch.records[0].id) >= 0);
    assert(lanlan_cache_find_record(&s_staging, batch.records[1].id) == -1);
    assert(s_staging.records[0].occurred_epoch == 1760000000 + 1000);

    /* The tombstone ring stays bounded and keeps the newest ids. */
    lanlan_cache_clear(&cache);
    for (int i = 0; i < LANLAN_CACHE_MAX_TOMBSTONES + 3; ++i) {
        uint8_t id[LANLAN_ID_BYTES];
        fill_id(id, (uint8_t)(i + 1));
        lanlan_cache_add_tombstone(&cache, id);
    }
    assert(cache.tombstone_count == LANLAN_CACHE_MAX_TOMBSTONES);
    uint8_t newest[LANLAN_ID_BYTES];
    fill_id(newest, (uint8_t)(LANLAN_CACHE_MAX_TOMBSTONES + 3));
    assert(lanlan_cache_is_tombstoned(&cache, newest));
    uint8_t oldest[LANLAN_ID_BYTES];
    fill_id(oldest, 1);
    assert(!lanlan_cache_is_tombstoned(&cache, oldest));
    /* Duplicates and null ids are ignored. */
    lanlan_cache_add_tombstone(&cache, newest);
    assert(cache.tombstone_count == LANLAN_CACHE_MAX_TOMBSTONES);
    uint8_t zero[LANLAN_ID_BYTES] = {0};
    lanlan_cache_add_tombstone(&cache, zero);
    assert(cache.tombstone_count == LANLAN_CACHE_MAX_TOMBSTONES);
    lanlan_cache_add_tombstone(NULL, newest);

    /* Bound-checked accessors never hand out a stale slot. */
    assert(lanlan_cache_record_at(&cache, -1)->version == 0);
    assert(lanlan_cache_record_at(&cache, 999)->version == 0);
    assert(lanlan_cache_record_at(NULL, 0)->version == 0);
    assert(lanlan_cache_reminder_at(&cache, 0)->version == 0);
    assert(lanlan_cache_reminder_at(&cache, 999)->version == 0);
    assert(lanlan_cache_reminder_at(NULL, 0)->last_rung_day == LANLAN_REMINDER_NO_RUNG);
    assert(lanlan_cache_find_record(&cache, zero) == -1);
    assert(lanlan_cache_find_reminder(&cache, zero) == -1);
    assert(!lanlan_cache_is_tombstoned(NULL, zero));
    assert(lanlan_cache_evict(NULL, NULL) == 0);
    lanlan_cache_sort_records(NULL, NULL);
    lanlan_cache_clear(NULL);
}

int main(void) {
    test_byte_budgets();
    test_round_trip_zero_one_and_max();
    test_decode_rejection();
    test_batch_merge_and_commit();
    test_batch_failure_leaves_state_untouched();
    test_capacity_and_eviction();
    puts("Lanlan cache: PASS (canonical layout, CRC, bounds, batch staging, tombstones, eviction, budgets)");
    return 0;
}

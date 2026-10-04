/* Host tests for main/lanlan_sync_parse.c.
 * Build:  cc -std=c11 -Wall -Wextra -Werror -Imain \
 *             tests/test_lanlan_sync_parse.c main/lanlan_sync_parse.c main/lanlan_json.c \
 *             main/lanlan_record.c main/lanlan_time.c main/lanlan_strings.c \
 *             -o /tmp/t && /tmp/t
 * Covers both wire shapes with realistic payloads, every documented
 * tolerance rule, the caregiver ordering guarantee, the unknown-amount rule and
 * the HTTP status classification table. */
#include "lanlan_sync_parse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lanlan_json.h"
#include "lanlan_record.h"
#include "lanlan_strings.h"
#include "lanlan_time.h"

/* ------------------------------------------------------------- hook capture -- */

typedef struct {
    int member_calls;
    int member_count;
    char member_ids[4][64];
    char member_names[4][64];
    int caregiver_calls;
    char caregiver_ids[8][64];
    int caregiver_before_members; /* 1 when a record was mapped before the directory */
} hook_log_t;

static void on_members(const lanlan_caregiver_member_t *members, int count, void *user) {
    hook_log_t *log = (hook_log_t *)user;
    log->member_calls++;
    log->member_count = count;
    for (int i = 0; i < count && i < 4; ++i) {
        snprintf(log->member_ids[i], sizeof(log->member_ids[i]), "%s", members[i].id);
        snprintf(log->member_names[i], sizeof(log->member_names[i]), "%s",
                 members[i].display_name ? members[i].display_name : "");
    }
}

static uint8_t on_caregiver(const char *id, void *user) {
    hook_log_t *log = (hook_log_t *)user;
    if (log->member_calls == 0) log->caregiver_before_members = 1;
    if (log->caregiver_calls < 8) {
        snprintf(log->caregiver_ids[log->caregiver_calls], sizeof(log->caregiver_ids[0]), "%s",
                 id);
    }
    log->caregiver_calls++;
    for (int i = 0; i < log->member_count; ++i) {
        if (strcmp(log->member_ids[i], id) == 0) return (uint8_t)i;
    }
    return 0;
}

static lanlan_sync_parse_hooks_t hooks_for(hook_log_t *log) {
    lanlan_sync_parse_hooks_t hooks = {
        .members = on_members,
        .caregiver = on_caregiver,
        .user = log,
    };
    return hooks;
}

/* ---------------------------------------------------------------- fixtures -- */

#define HEHE_ID "01622bdd-ff11-4904-b406-c22003d0cdcb"
#define YANG_ID "7c37b012-fa6e-427c-9812-580a1282d6f9"
#define MEAL_ID "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0"
#define WALK_ID "1a2b3c4d-5e6f-4a1b-8c2d-3e4f5a6b7c8d"
#define REM_ID  "2b3c4d5e-6f7a-4b2c-9d3e-4f5a6b7c8d9e"
#define GONE_ID "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"

/* The exact `changes` shape the service produces, including the fields the
 * device ignores (device_id, timezone, username, sched, upd) and a null name,
 * a null note and a null dur. */
static const char CHANGES_BODY[] =
    "{"
    "\"server_time\":\"2026-10-04T12:00:00Z\","
    "\"timezone\":\"Asia/Shanghai\","
    "\"utc_offset_minutes\":480,"
    "\"cursor\":412,"
    "\"has_more\":false,"
    "\"device_id\":\"ignored-device-id\","
    "\"members\":["
    "{\"id\":\"" HEHE_ID "\",\"display_name\":\"\\u8d6b\\u8d6b\",\"username\":\"hehe\"},"
    "{\"id\":\"" YANG_ID "\",\"display_name\":\"\\u7f8a\\u7f8a\",\"username\":\"yangyang\"}"
    "],"
    "\"records\":["
    "{\"id\":\"" MEAL_ID "\",\"v\":2,\"cat\":\"meal\",\"sub\":null,\"name\":null,"
    "\"at\":\"2026-10-04T11:40:00Z\",\"tz\":\"Asia/Shanghai\",\"tc\":\"trusted\","
    "\"by\":\"" HEHE_ID "\",\"perf\":\"" YANG_ID "\",\"unit\":\"g\",\"dur\":null,"
    "\"note\":\"\\u665a\\u996d\",\"st\":\"active\",\"seq\":411,\"amt\":120},"
    "{\"id\":\"" WALK_ID "\",\"v\":1,\"cat\":\"walk\",\"sub\":\"\",\"name\":\"\","
    "\"at\":\"2026-10-04T10:00:00Z\",\"tc\":\"estimated\","
    "\"by\":\"" YANG_ID "\",\"perf\":\"" YANG_ID "\",\"dur\":25,\"note\":null,"
    "\"st\":\"active\",\"seq\":412}"
    "],"
    "\"reminders\":["
    "{\"id\":\"" REM_ID "\",\"v\":1,\"cat\":\"meal\",\"sub\":null,\"name\":null,"
    "\"en\":0,\"sched\":\"daily\",\"t\":\"08:00\",\"upd\":\"2026-10-01T00:00:00Z\",\"seq\":410}"
    "],"
    "\"revoked\":[\"" GONE_ID "\"]"
    "}";

/* The `snapshot` shape: no `revoked`, and page fields the device needs. */
static const char SNAPSHOT_BODY[] =
    "{"
    "\"server_time\":\"2026-10-04T12:00:00Z\","
    "\"timezone\":\"Asia/Shanghai\","
    "\"utc_offset_minutes\":480,"
    "\"offset\":1,\"limit\":2,\"total\":3,\"has_more\":true,"
    "\"cursor\":470,"
    "\"members\":[{\"id\":\"" HEHE_ID "\",\"display_name\":\"\\u8d6b\\u8d6b\"}],"
    "\"records\":["
    "{\"id\":\"" WALK_ID "\",\"v\":1,\"cat\":\"walk\",\"at\":\"2026-10-04T10:00:00Z\","
    "\"tc\":\"trusted\",\"by\":\"" HEHE_ID "\",\"perf\":\"" HEHE_ID "\",\"dur\":25,"
    "\"st\":\"active\",\"seq\":470}"
    "],"
    "\"reminders\":[]"
    "}";

/* -------------------------------------------------------------- the parser -- */

static void test_changes_full_payload(void) {
    hook_log_t log;
    memset(&log, 0, sizeof(log));
    lanlan_sync_parse_hooks_t hooks = hooks_for(&log);
    lanlan_sync_batch_t batch;
    lanlan_sync_page_info_t info;

    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, CHANGES_BODY, strlen(CHANGES_BODY),
                                  -60, &hooks, &batch, &info)
           == LANLAN_SYNC_PARSE_OK);

    assert(batch.cursor == 412);
    assert(batch.utc_offset_minutes == 480);
    assert(batch.record_count == 2);
    assert(batch.reminder_count == 1);
    assert(batch.revoked_count == 1);
    assert(batch.has_server_time);
    /* Computed independently of the parser's own RFC 3339 helper. */
    int64_t expected_epoch = lanlan_time_days_from_civil(2026, 10, 4) * 86400 + 12 * 3600;
    assert(batch.server_epoch == expected_epoch);

    /* Page info: changes carries has_more, not the snapshot paging fields. */
    assert(info.has_has_more && !info.has_more);
    assert(!info.has_offset && !info.has_limit && !info.has_total);

    /* Record 0: revision 2, a known amount, a note preview and both caregiver
     * roles resolved through the directory. */
    const lanlan_record_t *meal = &batch.records[0];
    assert(meal->version == 2);
    assert(meal->category == LANLAN_CAT_MEAL);
    assert(meal->subitem == LANLAN_SUB_NONE);
    assert(meal->status == LANLAN_STATUS_ACTIVE);
    assert(meal->time_confidence == LANLAN_TIME_CONFIDENCE_TRUSTED);
    assert(meal->amount_known);
    assert(meal->unit == LANLAN_UNIT_G);
    assert(meal->amount_value > 119.9 && meal->amount_value < 120.1);
    assert(meal->duration_minutes == 0);
    assert(meal->seq == 411);
    assert(meal->created_by == 0);
    assert(meal->performed_by == 1);
    assert(meal->occurred_tz_offset_min == 480);
    char text[64];
    assert(lanlan_record_note_preview(meal, text, sizeof(text)) > 0);
    assert(strcmp(text, "\xE6\x99\x9A\xE9\xA5\xAD") == 0);

    /* Record 1: no `amt` at all, so the amount stays unknown and a duration is
     * read instead. */
    const lanlan_record_t *walk = &batch.records[1];
    assert(walk->category == LANLAN_CAT_WALK);
    assert(!lanlan_record_amount_is_known(walk));
    assert(!walk->amount_known);
    assert(walk->unit == LANLAN_UNIT_NONE);
    assert(walk->duration_minutes == 25);
    assert(walk->time_confidence == LANLAN_TIME_CONFIDENCE_ESTIMATED);
    assert(walk->created_by == 1 && walk->performed_by == 1);
    assert(walk->seq == 412);
    /* A null note reads as "no note", never as a missing record. */
    assert(lanlan_record_note_preview(walk, text, sizeof(text)) > 0);
    assert(strcmp(text, LANLAN_STR_DETAIL_NO_NOTE) == 0);

    /* Reminder: disabled, scheduled, never rung. */
    const lanlan_reminder_t *reminder = &batch.reminders[0];
    assert(reminder->category == LANLAN_CAT_MEAL);
    assert(!reminder->enabled);
    assert(strcmp(reminder->time_local, "08:00") == 0);
    assert(reminder->last_rung_day == LANLAN_REMINDER_NO_RUNG);

    /* Revoked ids are collected as raw bytes. */
    const uint8_t expected[LANLAN_ID_BYTES] = {0xaa, 0xaa, 0xaa, 0xaa, 0xbb, 0xbb, 0x4c, 0xcc,
                                               0x8d, 0xdd, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee};
    assert(memcmp(batch.revoked[0], expected, LANLAN_ID_BYTES) == 0);

    /* The directory arrived once, in service order, with the real names, and
     * every record was mapped after it. */
    assert(log.member_calls == 1);
    assert(log.member_count == 2);
    assert(strcmp(log.member_ids[0], HEHE_ID) == 0);
    assert(strcmp(log.member_ids[1], YANG_ID) == 0);
    assert(strcmp(log.member_names[0], "\xE8\xB5\xAB\xE8\xB5\xAB") == 0);
    assert(strcmp(log.member_names[1], "\xE7\xBE\x8A\xE7\xBE\x8A") == 0);
    assert(log.caregiver_calls == 4); /* by+perf for two records */
    assert(log.caregiver_before_members == 0);
}

static void test_snapshot_payload(void) {
    hook_log_t log;
    memset(&log, 0, sizeof(log));
    lanlan_sync_parse_hooks_t hooks = hooks_for(&log);
    lanlan_sync_batch_t batch;
    lanlan_sync_page_info_t info;

    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_SNAPSHOT, SNAPSHOT_BODY, strlen(SNAPSHOT_BODY),
                                  0, &hooks, &batch, &info)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.cursor == 470);
    assert(batch.record_count == 1);
    assert(batch.reminder_count == 0);
    assert(batch.revoked_count == 0); /* the snapshot has no revocation list */
    assert(batch.utc_offset_minutes == 480);
    assert(batch.records[0].category == LANLAN_CAT_WALK);
    assert(batch.records[0].duration_minutes == 25);
    assert(info.has_offset && info.offset == 1);
    assert(info.has_limit && info.limit == 2);
    assert(info.has_total && info.total == 3);
    assert(info.has_has_more && info.has_more);
    assert(log.member_calls == 1 && log.member_count == 1);
    assert(log.caregiver_before_members == 0);

    /* The crash-recovery read: offset 0 with limit 1, which is how the device
     * learns the total and the reminder list before fetching the newest page. */
    const char *head =
        "{\"cursor\":470,\"total\":3,\"has_more\":true,\"records\":[{\"id\":\"" MEAL_ID
        "\",\"cat\":\"meal\",\"at\":\"2026-10-04T11:40:00Z\",\"tc\":\"trusted\","
        "\"st\":\"active\",\"seq\":3}],\"reminders\":[{\"id\":\"" REM_ID "\",\"cat\":\"meal\","
        "\"en\":1,\"t\":\"07:30\"}],\"members\":[]}";
    memset(&log, 0, sizeof(log));
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_SNAPSHOT, head, strlen(head), 0, &hooks, &batch,
                                  &info)
           == LANLAN_SYNC_PARSE_OK);
    assert(info.has_total && info.total == 3);
    assert(batch.reminder_count == 1);
    assert(batch.reminders[0].enabled);
    assert(strcmp(batch.reminders[0].time_local, "07:30") == 0);
    /* An empty members array is "no update": the hook is not called. */
    assert(log.member_calls == 0);
}

/* ------------------------------------------------------- field requirements -- */

static void expect_page_status(lanlan_sync_page_t page, const char *body,
                               lanlan_sync_parse_status_t expected) {
    lanlan_sync_batch_t batch;
    lanlan_sync_parse_status_t actual =
        lanlan_sync_parse_page(page, body, body ? strlen(body) : 0, 0, NULL, &batch, NULL);
    if (actual != expected) {
        printf("page status: expected %s, got %s for: %s\n",
               lanlan_sync_parse_status_name(expected), lanlan_sync_parse_status_name(actual),
               body ? body : "(null)");
    }
    assert(actual == expected);
    /* A rejected page must not leave a half-filled batch behind. */
    if (expected != LANLAN_SYNC_PARSE_OK) {
        assert(batch.record_count == 0 && batch.reminder_count == 0 && batch.revoked_count == 0
               && batch.cursor == 0);
    }
}

static void test_required_fields(void) {
    /* cursor is mandatory on both endpoints. */
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"records\":[]}",
                       LANLAN_SYNC_PARSE_MISSING_FIELD);
    expect_page_status(LANLAN_SYNC_PAGE_SNAPSHOT, "{\"records\":[]}",
                       LANLAN_SYNC_PARSE_MISSING_FIELD);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":null,\"records\":[]}",
                       LANLAN_SYNC_PARSE_MISSING_FIELD);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":\"412\",\"records\":[]}",
                       LANLAN_SYNC_PARSE_WRONG_TYPE);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":-1,\"records\":[]}",
                       LANLAN_SYNC_PARSE_INVALID_VALUE);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":4294967296,\"records\":[]}",
                       LANLAN_SYNC_PARSE_INVALID_VALUE);

    /* records is mandatory and must be an array. */
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":1}",
                       LANLAN_SYNC_PARSE_MISSING_FIELD);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":1,\"records\":null}",
                       LANLAN_SYNC_PARSE_MISSING_FIELD);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":1,\"records\":5}",
                       LANLAN_SYNC_PARSE_WRONG_TYPE);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":1,\"records\":{}}",
                       LANLAN_SYNC_PARSE_WRONG_TYPE);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":1,\"records\":\"x\"}",
                       LANLAN_SYNC_PARSE_WRONG_TYPE);

    /* The top level must be an object, and the syntax must be valid JSON. */
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "[1,2]", LANLAN_SYNC_PARSE_WRONG_TYPE);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":1,", LANLAN_SYNC_PARSE_MALFORMED);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "not json", LANLAN_SYNC_PARSE_MALFORMED);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "{\"cursor\":1,\"records\":[]}{}",
                       LANLAN_SYNC_PARSE_MALFORMED);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, "", LANLAN_SYNC_PARSE_MALFORMED);
    expect_page_status(LANLAN_SYNC_PAGE_CHANGES, NULL, LANLAN_SYNC_PARSE_MALFORMED);
}

static void test_optional_arrays_and_unknown_fields(void) {
    /* reminders and revoked are genuinely optional. */
    lanlan_sync_batch_t batch;
    const char *body = "{\"cursor\":9,\"records\":[],\"unknown\":{\"deep\":[1,2,3]},\"x\":true}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, body, strlen(body), 0, NULL, &batch,
                                  NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.cursor == 9 && batch.record_count == 0 && batch.reminder_count == 0
           && batch.revoked_count == 0);

    /* Device_id and timezone are ignored, and an unusable utc_offset_minutes
     * falls back to the caller's configured offset. */
    const char *offset = "{\"cursor\":9,\"records\":[],\"device_id\":\"d\","
                         "\"timezone\":\"Asia/Shanghai\",\"utc_offset_minutes\":9999}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, offset, strlen(offset), -300, NULL,
                                  &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.utc_offset_minutes == -300);

    const char *missing_offset = "{\"cursor\":9,\"records\":[]}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, missing_offset,
                                  strlen(missing_offset), 60, NULL, &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.utc_offset_minutes == 60);

    /* Records inherit that offset even though "tz" is an IANA name. */
    const char *with_record =
        "{\"cursor\":1,\"utc_offset_minutes\":-330,\"records\":[{\"id\":\"" MEAL_ID
        "\",\"cat\":\"meal\",\"at\":\"2026-10-04T11:40:00Z\",\"tc\":\"trusted\","
        "\"st\":\"active\",\"seq\":5,\"tz\":\"America/New_York\"}]}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, with_record, strlen(with_record), 0,
                                  NULL, &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.record_count == 1);
    assert(batch.records[0].occurred_tz_offset_min == -330);

    /* A duplicate key keeps the LAST value, matching the JSON reader. */
    const char *duplicate = "{\"cursor\":1,\"cursor\":7,\"records\":[]}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, duplicate, strlen(duplicate), 0, NULL,
                                  &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.cursor == 7);

    /* A bad server_time is ignored rather than fatal; the clock simply stays
     * unanchored by this page. */
    const char *bad_time = "{\"cursor\":1,\"server_time\":\"yesterday\",\"records\":[]}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, bad_time, strlen(bad_time), 0, NULL,
                                  &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(!batch.has_server_time);
}

static void test_members_tolerance(void) {
    lanlan_sync_parse_hooks_t hooks;
    lanlan_sync_batch_t batch;
    hook_log_t log;

    /* Absent, empty, scalar and object forms are all "no update". */
    static const char *const no_update[] = {
        "{\"cursor\":1,\"records\":[]}",
        "{\"cursor\":1,\"records\":[],\"members\":[]}",
        "{\"cursor\":1,\"records\":[],\"members\":\"nope\"}",
        "{\"cursor\":1,\"records\":[],\"members\":null}",
        "{\"cursor\":1,\"records\":[],\"members\":{\"id\":\"x\"}}",
        "{\"cursor\":1,\"records\":[],\"members\":[1,2,3]}",
    };
    for (size_t i = 0; i < sizeof(no_update) / sizeof(no_update[0]); ++i) {
        memset(&log, 0, sizeof(log));
        hooks = hooks_for(&log);
        assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, no_update[i],
                                      strlen(no_update[i]), 0, &hooks, &batch, NULL)
               == LANLAN_SYNC_PARSE_OK);
        assert(log.member_calls == 0);
    }

    /* An extra unknown field inside a member is ignored, an entry without a
     * usable id is skipped, and a null display name is reported as absent. */
    const char *body =
        "{\"cursor\":1,\"records\":[],\"members\":["
        "{\"id\":\"" HEHE_ID "\",\"display_name\":\"\\u8d6b\\u8d6b\",\"role\":\"owner\","
        "\"extra\":[1,{\"a\":2}]},"
        "{\"display_name\":\"nameless\"},"
        "{\"id\":\"\",\"display_name\":\"empty\"},"
        "{\"id\":\"" YANG_ID "\",\"display_name\":null}"
        "]}";
    memset(&log, 0, sizeof(log));
    hooks = hooks_for(&log);
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, body, strlen(body), 0, &hooks, &batch,
                                  NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(log.member_calls == 1);
    assert(log.member_count == 2);
    assert(strcmp(log.member_ids[0], HEHE_ID) == 0);
    assert(strcmp(log.member_names[0], "\xE8\xB5\xAB\xE8\xB5\xAB") == 0);
    assert(strcmp(log.member_ids[1], YANG_ID) == 0);
    assert(log.member_names[1][0] == '\0');

    /* A malformed members entry never fails the page. */
    const char *broken =
        "{\"cursor\":5,\"records\":[],\"members\":[{\"id\":{\"nested\":1}}]}";
    memset(&log, 0, sizeof(log));
    hooks = hooks_for(&log);
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, broken, strlen(broken), 0, &hooks,
                                  &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.cursor == 5);
}

static void test_record_tolerance_and_rejection(void) {
    lanlan_sync_batch_t batch;

    /* A record missing its id, category, time or status is rejected, which
     * rejects the whole page: a batch is never half applied. */
    static const char *const broken_records[] = {
        "{\"cursor\":1,\"records\":[{\"cat\":\"meal\",\"at\":\"2026-10-04T11:40:00Z\","
        "\"seq\":1}]}",
        "{\"cursor\":1,\"records\":[{\"id\":\"" MEAL_ID "\",\"at\":\"2026-10-04T11:40:00Z\","
        "\"seq\":1}]}",
        "{\"cursor\":1,\"records\":[{\"id\":\"" MEAL_ID "\",\"cat\":\"meal\",\"seq\":1}]}",
        "{\"cursor\":1,\"records\":[{\"id\":\"not-a-uuid\",\"cat\":\"meal\","
        "\"at\":\"2026-10-04T11:40:00Z\",\"seq\":1}]}",
        "{\"cursor\":1,\"records\":[{\"id\":\"" MEAL_ID "\",\"cat\":\"nonsense\","
        "\"at\":\"2026-10-04T11:40:00Z\",\"seq\":1}]}",
        "{\"cursor\":1,\"records\":[{\"id\":\"" MEAL_ID "\",\"cat\":\"meal\","
        "\"at\":\"2026-10-04T11:40:00Z\",\"unit\":\"g\",\"seq\":1}]}",
        "{\"cursor\":1,\"records\":[{\"id\":\"" MEAL_ID "\",\"cat\":\"walk\","
        "\"at\":\"2026-10-04T11:40:00Z\",\"dur\":5000,\"seq\":1}]}",
        "{\"cursor\":1,\"records\":[{\"id\":\"" MEAL_ID "\",\"cat\":\"meal\","
        "\"at\":\"2026-10-04T11:40:00Z\",\"cat\":\"nonsense\",\"seq\":1}]}",
        /* A record without seq is refused: the cache needs the service order. */
        "{\"cursor\":1,\"records\":[{\"id\":\"" MEAL_ID "\",\"cat\":\"meal\","
        "\"at\":\"2026-10-04T11:40:00Z\",\"tc\":\"trusted\",\"st\":\"active\"}]}",
    };
    for (size_t i = 0; i < sizeof(broken_records) / sizeof(broken_records[0]); ++i) {
        lanlan_sync_parse_status_t status =
            lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, broken_records[i],
                                   strlen(broken_records[i]), 0, NULL, &batch, NULL);
        if (status != LANLAN_SYNC_PARSE_INVALID_VALUE) {
            printf("broken record %u: expected invalid_value, got %s\n", (unsigned)i,
                   lanlan_sync_parse_status_name(status));
        }
        assert(status == LANLAN_SYNC_PARSE_INVALID_VALUE);
        assert(batch.record_count == 0);
    }

    /* An amount above the service bound is stored as *unknown* rather than
     * inventing a number or rejecting the page: lanlan_record_set_amount()
     * refuses an out-of-range value and clears the unit with it. The service
     * rejects such a value at write time, so this is a defensive path. */
    const char *over_max = "{\"cursor\":1,\"records\":[{\"id\":\"" MEAL_ID
                           "\",\"cat\":\"meal\",\"at\":\"2026-10-04T11:40:00Z\","
                           "\"tc\":\"trusted\",\"st\":\"active\",\"seq\":1,"
                           "\"unit\":\"g\",\"amt\":10000}]}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, over_max, strlen(over_max), 0, NULL,
                                  &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.record_count == 1);
    assert(!batch.records[0].amount_known);
    assert(batch.records[0].unit == LANLAN_UNIT_NONE);

    /* Unknown record members are ignored, and a record whose unit arrives with
     * an unknown amount is still valid (the service never sends that, but the
     * device must not invent a zero). */
    const char *ok = "{\"cursor\":1,\"records\":[{\"id\":\"" MEAL_ID
                     "\",\"cat\":\"meal\",\"at\":\"2026-10-04T11:40:00Z\",\"tc\":\"trusted\","
                     "\"st\":\"active\",\"seq\":6,\"future\":[1,2]}],\"future_top\":1}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, ok, strlen(ok), 0, NULL, &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.record_count == 1);
    assert(!lanlan_record_amount_is_known(&batch.records[0]));
}

static void test_revoked_and_reminder_edges(void) {
    lanlan_sync_batch_t batch;
    /* Revoked entries must be usable ids; one bad entry rejects the page. */
    const char *bad = "{\"cursor\":1,\"records\":[],\"revoked\":[\"nope\"]}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, bad, strlen(bad), 0, NULL, &batch,
                                  NULL)
           == LANLAN_SYNC_PARSE_INVALID_VALUE);
    const char *not_string = "{\"cursor\":1,\"records\":[],\"revoked\":[7]}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, not_string, strlen(not_string), 0, NULL,
                                  &batch, NULL)
           == LANLAN_SYNC_PARSE_WRONG_TYPE);

    /* Reminders: a disabled one with a time, an enabled one without a time and
     * a bad "t" is rejected. */
    const char *reminders =
        "{\"cursor\":1,\"records\":[],\"reminders\":["
        "{\"id\":\"" REM_ID "\",\"cat\":\"care\",\"sub\":\"bath\",\"en\":0,\"t\":\"21:00\"},"
        "{\"id\":\"" GONE_ID "\",\"cat\":\"walk\",\"en\":1,\"t\":null,\"name\":\"\\u6d4b\"}"
        "]}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, reminders, strlen(reminders), 0, NULL,
                                  &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.reminder_count == 2);
    assert(batch.reminders[0].category == LANLAN_CAT_CARE);
    assert(batch.reminders[0].subitem == LANLAN_SUB_BATH);
    assert(!batch.reminders[0].enabled);
    assert(strcmp(batch.reminders[0].time_local, "21:00") == 0);
    assert(batch.reminders[1].category == LANLAN_CAT_WALK);
    assert(batch.reminders[1].enabled);
    assert(batch.reminders[1].time_local[0] == '\0');

    const char *bad_time = "{\"cursor\":1,\"records\":[],\"reminders\":[{\"id\":\"" REM_ID
                           "\",\"cat\":\"meal\",\"t\":\"8:00\"}]}";
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, bad_time, strlen(bad_time), 0, NULL,
                                  &batch, NULL)
           == LANLAN_SYNC_PARSE_INVALID_VALUE);
}

/* --------------------------------------------------------------- bounds -- */

static void test_capacity_and_size_bounds(void) {
    /* 41 records: the batch capacity is 40, so the extra one is dropped rather
     * than overflowing the batch. */
    static char payload[16384];
    size_t offset = 0;
    offset += (size_t)snprintf(payload + offset, sizeof(payload) - offset,
                               "{\"cursor\":7,\"records\":[");
    for (unsigned i = 0; i < LANLAN_CACHE_MAX_RECORDS + 1u; ++i) {
        char uuid[37];
        snprintf(uuid, sizeof(uuid), "%08x-1111-4222-8333-444444444444", 0x0f000000u + i);
        offset += (size_t)snprintf(payload + offset, sizeof(payload) - offset,
                                   "%s{\"id\":\"%s\",\"cat\":\"meal\","
                                   "\"at\":\"2026-10-04T11:40:%02uZ\",\"tc\":\"trusted\","
                                   "\"st\":\"active\",\"seq\":%u,\"amt\":%u,\"unit\":\"g\"}",
                                   i == 0 ? "" : ",", uuid, i % 60u, i + 1u, 10u + i);
        assert(offset < sizeof(payload));
    }
    snprintf(payload + offset, sizeof(payload) - offset, "]}");
    lanlan_sync_batch_t batch;
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, payload, strlen(payload), 0, NULL,
                                  &batch, NULL)
           == LANLAN_SYNC_PARSE_OK);
    assert(batch.record_count == (int32_t)LANLAN_CACHE_MAX_RECORDS);

    /* A string past the JSON limit is TOO_LARGE, not a crash and not a silent
     * truncation of the batch. */
    static char huge[LANLAN_JSON_MAX_STRING_BYTES + 256];
    memset(huge, 'a', sizeof(huge));
    size_t prefix = (size_t)snprintf(huge, sizeof(huge), "{\"cursor\":1,\"timezone\":\"");
    memset(huge + prefix, 'a', sizeof(huge) - prefix - 4);
    huge[sizeof(huge) - 3] = '"';
    huge[sizeof(huge) - 2] = '}';
    huge[sizeof(huge) - 1] = '\0';
    assert(lanlan_sync_parse_page(LANLAN_SYNC_PAGE_CHANGES, huge, strlen(huge), 0, NULL, &batch,
                                  NULL)
           == LANLAN_SYNC_PARSE_TOO_LARGE);
}

/* ------------------------------------------------------------------ HTTP -- */

static void test_http_classification(void) {
    assert(lanlan_sync_http_classify(200, NULL) == LANLAN_SYNC_HTTP_OK);
    assert(lanlan_sync_http_classify(201, NULL) == LANLAN_SYNC_HTTP_OK);
    assert(lanlan_sync_http_classify(204, NULL) == LANLAN_SYNC_HTTP_OK);
    assert(lanlan_sync_http_classify(401, NULL) == LANLAN_SYNC_HTTP_REJECTED);
    assert(lanlan_sync_http_classify(409, "cursor_invalid") == LANLAN_SYNC_HTTP_RESYNC);
    assert(lanlan_sync_http_classify(409, "other") == LANLAN_SYNC_HTTP_CLIENT_ERROR);
    assert(lanlan_sync_http_classify(409, NULL) == LANLAN_SYNC_HTTP_CLIENT_ERROR);
    assert(lanlan_sync_http_classify(400, NULL) == LANLAN_SYNC_HTTP_CLIENT_ERROR);
    assert(lanlan_sync_http_classify(403, NULL) == LANLAN_SYNC_HTTP_CLIENT_ERROR);
    assert(lanlan_sync_http_classify(404, NULL) == LANLAN_SYNC_HTTP_CLIENT_ERROR);
    assert(lanlan_sync_http_classify(422, NULL) == LANLAN_SYNC_HTTP_CLIENT_ERROR);
    assert(lanlan_sync_http_classify(429, NULL) == LANLAN_SYNC_HTTP_CLIENT_ERROR);
    assert(lanlan_sync_http_classify(500, NULL) == LANLAN_SYNC_HTTP_RETRY);
    assert(lanlan_sync_http_classify(502, NULL) == LANLAN_SYNC_HTTP_RETRY);
    assert(lanlan_sync_http_classify(503, NULL) == LANLAN_SYNC_HTTP_RETRY);
    assert(lanlan_sync_http_classify(302, NULL) == LANLAN_SYNC_HTTP_FATAL);
    assert(lanlan_sync_http_classify(100, NULL) == LANLAN_SYNC_HTTP_FATAL);
    assert(lanlan_sync_http_classify(0, NULL) == LANLAN_SYNC_HTTP_FATAL);

    assert(strcmp(lanlan_sync_http_name(LANLAN_SYNC_HTTP_OK), "ok") == 0);
    assert(strcmp(lanlan_sync_http_name(LANLAN_SYNC_HTTP_REJECTED), "credential_rejected") == 0);
    assert(strcmp(lanlan_sync_http_name(LANLAN_SYNC_HTTP_RESYNC), "resync") == 0);
    assert(strcmp(lanlan_sync_http_name(LANLAN_SYNC_HTTP_CLIENT_ERROR), "client_error") == 0);
    assert(strcmp(lanlan_sync_http_name(LANLAN_SYNC_HTTP_RETRY), "retry") == 0);
    assert(strcmp(lanlan_sync_http_name(LANLAN_SYNC_HTTP_FATAL), "fatal") == 0);
}

static void test_error_code_extraction(void) {
    char code[64];
    const char *body =
        "{\"error\":{\"code\":\"cursor_invalid\",\"message\":\"resynchronize\","
        "\"field\":\"cursor\"}}";
    assert(lanlan_sync_parse_error_code(body, strlen(body), code, sizeof(code)));
    assert(strcmp(code, "cursor_invalid") == 0);

    const char *plain = "{\"cursor\":412,\"records\":[]}";
    assert(!lanlan_sync_parse_error_code(plain, strlen(plain), code, sizeof(code)));
    assert(code[0] == '\0');

    const char *broken = "{\"error\":";
    assert(!lanlan_sync_parse_error_code(broken, strlen(broken), code, sizeof(code)));
    const char *no_code = "{\"error\":{\"message\":\"x\"}}";
    assert(!lanlan_sync_parse_error_code(no_code, strlen(no_code), code, sizeof(code)));
    const char *numeric = "{\"error\":{\"code\":7}}";
    assert(!lanlan_sync_parse_error_code(numeric, strlen(numeric), code, sizeof(code)));
    assert(!lanlan_sync_parse_error_code(NULL, 0, code, sizeof(code)));

    /* A small destination buffer truncates but still reports success. */
    char small[8];
    assert(lanlan_sync_parse_error_code(body, strlen(body), small, sizeof(small)));
    assert(small[0] != '\0');
    assert(strlen(small) <= sizeof(small) - 1);

    /* The classification consumes exactly that code. `code` was cleared by the
     * failed extractions above, so it is read again here. */
    char again[64];
    assert(lanlan_sync_parse_error_code(body, strlen(body), again, sizeof(again)));
    assert(strcmp(again, "cursor_invalid") == 0);
    assert(lanlan_sync_http_classify(409, again) == LANLAN_SYNC_HTTP_RESYNC);
}

static void test_status_names(void) {
    assert(strcmp(lanlan_sync_parse_status_name(LANLAN_SYNC_PARSE_OK), "ok") == 0);
    assert(strcmp(lanlan_sync_parse_status_name(LANLAN_SYNC_PARSE_MALFORMED), "malformed") == 0);
    assert(strcmp(lanlan_sync_parse_status_name(LANLAN_SYNC_PARSE_MISSING_FIELD),
                  "missing_field")
           == 0);
    assert(strcmp(lanlan_sync_parse_status_name(LANLAN_SYNC_PARSE_WRONG_TYPE), "wrong_type") == 0);
    assert(strcmp(lanlan_sync_parse_status_name(LANLAN_SYNC_PARSE_TOO_LARGE), "too_large") == 0);
    assert(strcmp(lanlan_sync_parse_status_name(LANLAN_SYNC_PARSE_INVALID_VALUE),
                  "invalid_value")
           == 0);
    assert(strcmp(lanlan_sync_parse_status_name((lanlan_sync_parse_status_t)99), "unknown") == 0);
}

int main(void) {
    test_changes_full_payload();
    test_snapshot_payload();
    test_required_fields();
    test_optional_arrays_and_unknown_fields();
    test_members_tolerance();
    test_record_tolerance_and_rejection();
    test_revoked_and_reminder_edges();
    test_capacity_and_size_bounds();
    test_http_classification();
    test_error_code_extraction();
    test_status_names();
    puts("Lanlan sync parse: PASS (changes, snapshot, required/optional fields, members order, "
         "unknown-amount, capacity, HTTP table, error code)");
    return 0;
}

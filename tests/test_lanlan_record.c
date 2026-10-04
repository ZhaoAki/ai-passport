/* Host tests for main/lanlan_record.c.
 * Build:  cc -std=c11 -Wall -Wextra -Werror -Imain \
 *             tests/test_lanlan_record.c main/lanlan_record.c main/lanlan_time.c \
 *             main/lanlan_strings.c -o /tmp/t && /tmp/t
 * The string table is needed because the record module takes its fixed labels
 * from main/lanlan_strings.h/.c. */
#include "lanlan_record.h"
#include "lanlan_strings.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint8_t next_id_byte;

static void fill_id(uint8_t id[LANLAN_ID_BYTES]) {
    for (size_t i = 0; i < LANLAN_ID_BYTES; ++i) id[i] = (uint8_t)(++next_id_byte);
}

static lanlan_record_t make_record(lanlan_category_t category, lanlan_subitem_t subitem) {
    lanlan_record_t record;
    memset(&record, 0, sizeof(record));
    assert(sizeof(lanlan_record_t) <= 176);
    fill_id(record.id);
    record.version = 1;
    record.category = category;
    record.subitem = subitem;
    record.time_confidence = LANLAN_TIME_CONFIDENCE_TRUSTED;
    record.occurred_epoch = 1760000000; /* 2025-10-09T08:53:20Z */
    record.occurred_tz_offset_min = 480;
    record.created_epoch = 1760000300;
    record.created_by = 0;
    record.performed_by = 1;
    record.duration_minutes = 0;
    record.status = LANLAN_STATUS_ACTIVE;
    record.seq = 12;
    lanlan_record_set_amount_unknown(&record);
    return record;
}

static void test_category_validation_matrix(void) {
    /* meal: unit in g, ml, scoop, cup, piece, bag; no sub-item; no custom name. */
    const lanlan_unit_t meal_units[] = {LANLAN_UNIT_G, LANLAN_UNIT_ML, LANLAN_UNIT_SCOOP,
                                        LANLAN_UNIT_CUP, LANLAN_UNIT_PIECE, LANLAN_UNIT_BAG};
    for (size_t i = 0; i < sizeof(meal_units) / sizeof(meal_units[0]); ++i) {
        assert(lanlan_record_validate_category(LANLAN_CAT_MEAL, LANLAN_SUB_NONE, meal_units[i],
                                               true, false)
               == LANLAN_RECORD_OK);
    }
    assert(lanlan_record_validate_category(LANLAN_CAT_MEAL, LANLAN_SUB_NONE, LANLAN_UNIT_BOWL, true,
                                           false)
           == LANLAN_RECORD_ERR_UNIT);
    assert(lanlan_record_validate_category(LANLAN_CAT_MEAL, LANLAN_SUB_NONE, LANLAN_UNIT_NONE, true,
                                           false)
           == LANLAN_RECORD_ERR_UNIT);
    assert(lanlan_record_validate_category(LANLAN_CAT_MEAL, LANLAN_SUB_NONE, LANLAN_UNIT_NONE,
                                           false, false)
           == LANLAN_RECORD_OK);
    assert(lanlan_record_validate_category(LANLAN_CAT_MEAL, LANLAN_SUB_BATH, LANLAN_UNIT_NONE,
                                           false, false)
           == LANLAN_RECORD_ERR_SUBITEM);

    /* water: only ml and bowl. */
    assert(lanlan_record_validate_category(LANLAN_CAT_WATER, LANLAN_SUB_NONE, LANLAN_UNIT_ML, true,
                                           false)
           == LANLAN_RECORD_OK);
    assert(lanlan_record_validate_category(LANLAN_CAT_WATER, LANLAN_SUB_NONE, LANLAN_UNIT_BOWL,
                                           true, false)
           == LANLAN_RECORD_OK);
    assert(lanlan_record_validate_category(LANLAN_CAT_WATER, LANLAN_SUB_NONE, LANLAN_UNIT_G, true,
                                           false)
           == LANLAN_RECORD_ERR_UNIT);
    assert(lanlan_record_validate_category(LANLAN_CAT_WATER, LANLAN_SUB_NONE, LANLAN_UNIT_ML,
                                           false, false)
           == LANLAN_RECORD_ERR_UNIT);

    /* care: fixed sub-items, no amount, custom name required only for other. */
    const lanlan_subitem_t care_items[] = {LANLAN_SUB_BATH, LANLAN_SUB_GROOMING, LANLAN_SUB_TEETH,
                                           LANLAN_SUB_COMB};
    for (size_t i = 0; i < sizeof(care_items) / sizeof(care_items[0]); ++i) {
        assert(lanlan_record_validate_category(LANLAN_CAT_CARE, care_items[i], LANLAN_UNIT_NONE,
                                               false, false)
               == LANLAN_RECORD_OK);
        assert(lanlan_record_validate_category(LANLAN_CAT_CARE, care_items[i], LANLAN_UNIT_G, true,
                                               false)
               == LANLAN_RECORD_ERR_UNIT);
    }
    assert(lanlan_record_validate_category(LANLAN_CAT_CARE, LANLAN_SUB_CARE_OTHER, LANLAN_UNIT_NONE,
                                           false, false)
           == LANLAN_RECORD_ERR_CUSTOM_NAME);
    assert(lanlan_record_validate_category(LANLAN_CAT_CARE, LANLAN_SUB_CARE_OTHER, LANLAN_UNIT_NONE,
                                           false, true)
           == LANLAN_RECORD_OK);
    assert(lanlan_record_validate_category(LANLAN_CAT_CARE, LANLAN_SUB_EAR, LANLAN_UNIT_NONE, false,
                                           false)
           == LANLAN_RECORD_ERR_SUBITEM);
    assert(lanlan_record_validate_category(LANLAN_CAT_CARE, LANLAN_SUB_NONE, LANLAN_UNIT_NONE,
                                           false, false)
           == LANLAN_RECORD_ERR_SUBITEM);

    /* cleaning: preset keys, no amount, custom name required only for other. */
    const lanlan_subitem_t cleaning_items[] = {LANLAN_SUB_EAR, LANLAN_SUB_PAW, LANLAN_SUB_PAD,
                                               LANLAN_SUB_LITTER};
    for (size_t i = 0; i < sizeof(cleaning_items) / sizeof(cleaning_items[0]); ++i) {
        assert(lanlan_record_validate_category(LANLAN_CAT_CLEANING, cleaning_items[i],
                                               LANLAN_UNIT_NONE, false, false)
               == LANLAN_RECORD_OK);
    }
    assert(lanlan_record_validate_category(LANLAN_CAT_CLEANING, LANLAN_SUB_CLEANING_OTHER,
                                           LANLAN_UNIT_NONE, false, false)
           == LANLAN_RECORD_ERR_CUSTOM_NAME);
    assert(lanlan_record_validate_category(LANLAN_CAT_CLEANING, LANLAN_SUB_CLEANING_OTHER,
                                           LANLAN_UNIT_NONE, false, true)
           == LANLAN_RECORD_OK);
    assert(lanlan_record_validate_category(LANLAN_CAT_CLEANING, LANLAN_SUB_BATH, LANLAN_UNIT_NONE,
                                           false, false)
           == LANLAN_RECORD_ERR_SUBITEM);

    /* walk: no sub-item, no amount, duration only. */
    assert(lanlan_record_validate_category(LANLAN_CAT_WALK, LANLAN_SUB_NONE, LANLAN_UNIT_NONE,
                                           false, false)
           == LANLAN_RECORD_OK);
    assert(lanlan_record_validate_category(LANLAN_CAT_WALK, LANLAN_SUB_NONE, LANLAN_UNIT_ML, true,
                                           false)
           == LANLAN_RECORD_ERR_UNIT);

    /* other: no sub-item, no amount, custom name required. */
    assert(lanlan_record_validate_category(LANLAN_CAT_OTHER, LANLAN_SUB_NONE, LANLAN_UNIT_NONE,
                                           false, true)
           == LANLAN_RECORD_OK);
    assert(lanlan_record_validate_category(LANLAN_CAT_OTHER, LANLAN_SUB_NONE, LANLAN_UNIT_NONE,
                                           false, false)
           == LANLAN_RECORD_ERR_CUSTOM_NAME);

    /* Out-of-range values are rejected before the table is consulted. */
    assert(lanlan_record_validate_category((lanlan_category_t)99, LANLAN_SUB_NONE, LANLAN_UNIT_NONE,
                                           false, false)
           == LANLAN_RECORD_ERR_CATEGORY);
    assert(lanlan_record_validate_category(LANLAN_CAT_MEAL, (lanlan_subitem_t)99, LANLAN_UNIT_NONE,
                                           false, false)
           == LANLAN_RECORD_ERR_SUBITEM);
    assert(lanlan_record_validate_category(LANLAN_CAT_MEAL, LANLAN_SUB_NONE, (lanlan_unit_t)99,
                                           false, false)
           == LANLAN_RECORD_ERR_UNIT);
}

static void test_wire_tokens(void) {
    assert(strcmp(lanlan_category_wire(LANLAN_CAT_CLEANING), "cleaning") == 0);
    assert(strcmp(lanlan_record_subitem_wire(LANLAN_CAT_CARE, LANLAN_SUB_CARE_OTHER), "other") == 0);
    assert(strcmp(lanlan_record_subitem_wire(LANLAN_CAT_CLEANING, LANLAN_SUB_CLEANING_OTHER),
                  "other")
           == 0);
    assert(strcmp(lanlan_record_subitem_wire(LANLAN_CAT_CLEANING, LANLAN_SUB_LITTER), "litter")
           == 0);
    /* The shared "other" key is disambiguated by the category: crossing them
     * would send a sub-item the server rejects. */
    assert(lanlan_record_subitem_wire(LANLAN_CAT_CLEANING, LANLAN_SUB_BATH) == NULL);
    assert(lanlan_record_subitem_wire(LANLAN_CAT_CARE, LANLAN_SUB_PAW) == NULL);
    assert(lanlan_record_subitem_wire(LANLAN_CAT_MEAL, LANLAN_SUB_BATH) == NULL);
    assert(lanlan_record_subitem_wire(LANLAN_CAT_CARE, LANLAN_SUB_NONE) == NULL);
    assert(strcmp(lanlan_unit_wire(LANLAN_UNIT_BOWL), "bowl") == 0);
    assert(lanlan_unit_wire(LANLAN_UNIT_NONE) == NULL);
}

static void test_unknown_amount_is_never_zero(void) {
    lanlan_record_t record = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    assert(!lanlan_record_amount_is_known(&record));
    /* A zero amount must not be interpreted as "known": the whole point of the
     * flag is that an unset quantity can never render as 0. */
    record.amount_known = true;
    record.amount_value = 0.0;
    assert(!lanlan_record_amount_is_known(&record));
    lanlan_record_set_amount_unknown(&record);
    assert(record.amount_value == 0.0 && record.unit == LANLAN_UNIT_NONE && !record.amount_known);

    /* Invalid values collapse to "unknown" instead of storing junk. */
    lanlan_record_set_amount(&record, 0.0, LANLAN_UNIT_G);
    assert(!lanlan_record_amount_is_known(&record) && record.unit == LANLAN_UNIT_NONE);
    lanlan_record_set_amount(&record, -5.0, LANLAN_UNIT_G);
    assert(!lanlan_record_amount_is_known(&record));
    lanlan_record_set_amount(&record, 10000.0, LANLAN_UNIT_G);
    assert(!lanlan_record_amount_is_known(&record));
    lanlan_record_set_amount(&record, NAN, LANLAN_UNIT_G);
    assert(!lanlan_record_amount_is_known(&record));
    lanlan_record_set_amount(&record, 12.5, LANLAN_UNIT_NONE);
    assert(!lanlan_record_amount_is_known(&record));

    lanlan_record_set_amount(&record, 120.0, LANLAN_UNIT_G);
    assert(lanlan_record_amount_is_known(&record));
    char text[32];
    size_t length = lanlan_record_format_amount(&record, text, sizeof(text));
    assert(length == strlen(text));
    assert(strcmp(text, "120 克") == 0);

    lanlan_record_set_amount_unknown(&record);
    assert(lanlan_record_format_amount(&record, text, sizeof(text)) == strlen(LANLAN_STR_QUANTITY_UNKNOWN));
    assert(strcmp(text, LANLAN_STR_QUANTITY_UNKNOWN) == 0);
    assert(strcmp(text, "0 克") != 0);
}

static void test_amount_and_duration_formatting(void) {
    lanlan_record_t record = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    char text[32];

    lanlan_record_set_amount(&record, 12.5, LANLAN_UNIT_ML);
    assert(lanlan_record_format_amount(&record, text, sizeof(text)) > 0);
    assert(strcmp(text, "12.5 毫升") == 0);

    lanlan_record_set_amount(&record, 12.04, LANLAN_UNIT_CUP);
    lanlan_record_format_amount(&record, text, sizeof(text));
    assert(strcmp(text, "12 杯") == 0);

    lanlan_record_set_amount(&record, 0.5, LANLAN_UNIT_SCOOP);
    lanlan_record_format_amount(&record, text, sizeof(text));
    assert(strcmp(text, "0.5 勺") == 0);

    /* A small output buffer degrades to a terminated prefix, never to garbage. */
    lanlan_record_set_amount(&record, 9999.0, LANLAN_UNIT_PIECE);
    size_t length = lanlan_record_format_amount(&record, text, 5);
    assert(length < 5 && text[length] == '\0');
    assert(lanlan_record_format_amount(&record, NULL, sizeof(text)) == 0);
    assert(lanlan_record_format_amount(&record, text, 0) == 0);

    lanlan_record_t walk = make_record(LANLAN_CAT_WALK, LANLAN_SUB_NONE);
    assert(lanlan_record_format_duration(&walk, text, sizeof(text)) == 0);
    walk.duration_minutes = 25;
    assert(lanlan_record_format_duration(&walk, text, sizeof(text)) > 0);
    assert(strcmp(text, "25 分钟") == 0);
    walk.duration_minutes = 1440;
    lanlan_record_format_duration(&walk, text, sizeof(text));
    assert(strcmp(text, "1440 分钟") == 0);
}

static void test_record_validation_limits(void) {
    lanlan_record_t record = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    assert(lanlan_record_is_valid(&record) == LANLAN_RECORD_OK);
    assert(lanlan_record_validate(&record) == LANLAN_RECORD_OK);

    /* Amount bounds: greater than 0 and at most 9999. */
    lanlan_record_set_amount(&record, 9999.0, LANLAN_UNIT_G);
    assert(lanlan_record_is_valid(&record) == LANLAN_RECORD_OK);
    record.amount_value = 9999.5;
    assert(lanlan_record_validate(&record) == LANLAN_RECORD_ERR_AMOUNT);
    record.amount_value = 0.0001;
    assert(lanlan_record_validate(&record) == LANLAN_RECORD_ERR_AMOUNT);
    lanlan_record_set_amount_unknown(&record);
    record.amount_value = 3.0; /* flag and value disagree */
    assert(lanlan_record_validate(&record) == LANLAN_RECORD_ERR_AMOUNT);
    lanlan_record_set_amount_unknown(&record);

    /* duration 0 = unset, 1440 max, walk only. */
    lanlan_record_t walk = make_record(LANLAN_CAT_WALK, LANLAN_SUB_NONE);
    walk.duration_minutes = 1440;
    assert(lanlan_record_is_valid(&walk) == LANLAN_RECORD_OK);
    walk.duration_minutes = 1441;
    assert(lanlan_record_validate(&walk) == LANLAN_RECORD_ERR_DURATION);
    walk.duration_minutes = 0;
    assert(lanlan_record_is_valid(&walk) == LANLAN_RECORD_OK);
    record.duration_minutes = 10;
    assert(lanlan_record_validate(&record) == LANLAN_RECORD_ERR_DURATION);

    /* revision, caregiver, time confidence, status, seq, time range. */
    lanlan_record_t other = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    other.version = 0;
    assert(lanlan_record_validate(&other) == LANLAN_RECORD_ERR_REVISION);
    other = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    other.created_by = 2;
    assert(lanlan_record_validate(&other) == LANLAN_RECORD_ERR_CAREGIVER);
    other = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    other.time_confidence = (lanlan_time_confidence_t)7;
    assert(lanlan_record_validate(&other) == LANLAN_RECORD_ERR_TIME_CONFIDENCE);
    other = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    other.status = (lanlan_record_status_t)3;
    assert(lanlan_record_validate(&other) == LANLAN_RECORD_ERR_STATUS);
    other = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    other.seq = 0;
    assert(lanlan_record_validate(&other) == LANLAN_RECORD_ERR_SEQ);
    other = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    other.occurred_epoch = 0;
    assert(lanlan_record_validate(&other) == LANLAN_RECORD_ERR_TIME_RANGE);
    other = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    other.occurred_tz_offset_min = (int16_t)2000;
    assert(lanlan_record_validate(&other) == LANLAN_RECORD_ERR_TIME_RANGE);

    /* Ids must be present for a decoded entry. */
    lanlan_record_t blank = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    memset(blank.id, 0, sizeof(blank.id));
    assert(lanlan_record_id_is_zero(blank.id));
    assert(lanlan_record_is_valid(&blank) == LANLAN_RECORD_ERR_REVISION);

    /* The custom-name and note fields are byte-bounded buffers: a value that
     * fills the buffer without a terminator is rejected. */
    lanlan_record_t custom = make_record(LANLAN_CAT_OTHER, LANLAN_SUB_NONE);
    strcpy(custom.custom_name, "1234567890123");
    assert(lanlan_record_validate(&custom) == LANLAN_RECORD_OK);
    assert(lanlan_record_is_valid(&custom) == LANLAN_RECORD_ERR_CUSTOM_NAME);
    strcpy(custom.custom_name, "123456789012");
    assert(lanlan_record_is_valid(&custom) == LANLAN_RECORD_OK);

    lanlan_record_t noted = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    memset(noted.note, 'a', sizeof(noted.note));
    assert(lanlan_record_is_valid(&noted) == LANLAN_RECORD_ERR_NOTE);
    noted.note[sizeof(noted.note) - 1] = '\0';
    assert(lanlan_record_is_valid(&noted) == LANLAN_RECORD_OK);
}

static void test_note_preview_bound(void) {
    /* The device keeps a bounded preview, and makes the cut explicit instead of
     * pretending the note is complete. */
    lanlan_record_t record = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    assert(LANLAN_RECORD_NOTE_PREVIEW_BYTES == 48);
    char text[128];

    assert(lanlan_record_set_note(&record, "") == false);
    assert(record.note_truncated == false);
    size_t length = lanlan_record_note_preview(&record, text, sizeof(text));
    assert(length == strlen(LANLAN_STR_DETAIL_NO_NOTE));
    assert(strcmp(text, LANLAN_STR_DETAIL_NO_NOTE) == 0);

    assert(lanlan_record_set_note(&record, "吃了半碗") == false);
    assert(strcmp(record.note, "吃了半碗") == 0);
    assert(!record.note_truncated);
    lanlan_record_note_preview(&record, text, sizeof(text));
    assert(strcmp(text, "吃了半碗") == 0);

    /* A long note is cut at a UTF-8 boundary and flagged. */
    char long_note[512];
    long_note[0] = '\0';
    for (int i = 0; i < 100; ++i) strcat(long_note, "好");
    assert(strlen(long_note) == 300);
    assert(lanlan_record_set_note(&record, long_note) == true);
    assert(record.note_truncated);
    assert(strlen(record.note) < LANLAN_RECORD_NOTE_PREVIEW_BYTES);
    assert(strlen(record.note) % 3 == 0); /* no partial 3-byte character */
    for (size_t i = 0; i < strlen(record.note); i += 3) {
        assert((unsigned char)record.note[i] == 0xE5); /* 好 in UTF-8 */
    }
    lanlan_record_note_preview(&record, text, sizeof(text));
    assert(strstr(text, record.note) == text);
    /* The marker is present, so the UI cannot render an incomplete note as if
     * it were the whole thing. */
    assert(strstr(text, LANLAN_RECORD_FIXED_LABELS[9]) != NULL);

    /* Control characters are removed, and a 4-byte character is never split. */
    assert(lanlan_record_set_note(&record, "a\nb\tc") == false);
    assert(strcmp(record.note, "abc") == 0);
    char emoji_note[64];
    emoji_note[0] = '\0';
    for (int i = 0; i < 14; ++i) strcat(emoji_note, "🐕");
    assert(strlen(emoji_note) == 56);
    assert(lanlan_record_set_note(&record, emoji_note) == true);
    assert(strlen(record.note) == 44); /* 11 complete 4-byte characters */
    assert(strcmp(record.note, "🐕🐕🐕🐕🐕🐕🐕🐕🐕🐕🐕") == 0);

    /* A small output buffer still communicates the truncation. */
    lanlan_record_set_note(&record, long_note);
    lanlan_record_note_preview(&record, text, 8);
    assert(strlen(text) < 8);
    assert(text[0] != '\0');
    assert(lanlan_record_note_preview(&record, NULL, sizeof(text)) == 0);

    /* Custom names share the same boundary-safe truncation. */
    lanlan_record_t care = make_record(LANLAN_CAT_CARE, LANLAN_SUB_BATH);
    assert(lanlan_record_set_custom_name(&care, "十二个字的自定义名字超过限制") == true);
    assert(strlen(care.custom_name) < LANLAN_RECORD_CUSTOM_BYTES);
    assert(strlen(care.custom_name) % 3 == 0);
    assert(lanlan_record_set_custom_name(&care, "短名字") == false);
    assert(strcmp(care.custom_name, "短名字") == 0);
}

static void test_utf8_truncation_and_control_sanitizing(void) {
    /* "懒懒" is 3 + 3 bytes: a 4-byte destination cannot hold both characters. */
    char out[16];
    size_t written = lanlan_utf8_copy_chars("懒懒", out, 4, 10);
    assert(written == 3);
    assert(strcmp(out, "懒") == 0);
    assert(lanlan_utf8_length(out) == 1);

    /* A cut in the middle of a 4-byte character drops the partial character. */
    written = lanlan_utf8_copy_chars("a🐕b", out, 4, 10);
    assert(written == 1 && strcmp(out, "a") == 0);
    /* 🐕 is 4 bytes: 1 + 4 + NUL needs 6, so dst_size 9 holds "a🐕b" in full. */
    written = lanlan_utf8_copy_chars("a🐕b", out, 9, 10);
    assert(strcmp(out, "a🐕b") == 0 && written == 6);
    written = lanlan_utf8_copy_chars("a🐕b", out, 6, 10);
    assert(strcmp(out, "a🐕") == 0 && written == 5);

    /* The character limit is characters, not bytes. */
    written = lanlan_utf8_copy_chars("懒懒懒懒懒", out, sizeof(out), 2);
    assert(strcmp(out, "懒懒") == 0 && written == 6);

    /* Control characters never reach a label; malformed bytes become U+FFFD. */
    written = lanlan_text_sanitize("a\nb\tc\r\x01", out, sizeof(out));
    assert(strcmp(out, "abc") == 0 && written == 3);
    const char bad[] = {'x', (char)0xFF, 'y', '\0'};
    lanlan_text_sanitize(bad, out, sizeof(out));
    assert(lanlan_utf8_length(out) == 3);
    assert(out[0] == 'x' && out[1] == (char)0xEF && out[2] == (char)0xBF && out[3] == (char)0xBD);
    assert(out[4] == 'y');

    /* The clamp helper leaves room for the terminator and never splits a
     * character. */
    assert(lanlan_utf8_clamp_bytes("懒", 4) == 3);
    assert(lanlan_utf8_clamp_bytes("懒", 3) == 0);
    assert(lanlan_utf8_clamp_bytes("懒", 2) == 0);
    assert(lanlan_utf8_clamp_bytes("ab", 3) == 2);
    assert(lanlan_utf8_clamp_bytes("", 7) == 0);
    assert(lanlan_utf8_clamp_bytes(NULL, 7) == 0);
    assert(lanlan_utf8_length(NULL) == 0);
    assert(lanlan_utf8_copy_chars(NULL, out, sizeof(out), 3) == 0);
    assert(lanlan_text_sanitize(NULL, out, sizeof(out)) == 0);
}

static void test_labels_and_time_formatting(void) {
    assert(strcmp(lanlan_category_label(LANLAN_CAT_MEAL), "喂食") == 0);
    assert(strcmp(lanlan_category_label(LANLAN_CAT_CLEANING), "清洁") == 0);
    assert(strcmp(lanlan_category_label(LANLAN_CAT_OTHER), "其他") == 0);
    assert(strcmp(lanlan_category_label((lanlan_category_t)77), "") == 0);

    char label[LANLAN_RECORD_CUSTOM_BYTES];
    lanlan_record_t care = make_record(LANLAN_CAT_CARE, LANLAN_SUB_BATH);
    lanlan_record_subitem_label(&care, label, sizeof(label));
    assert(strcmp(label, "洗澡") == 0);

    /* A custom name overrides the optional preset label, and is sanitized. */
    lanlan_record_t cleaning = make_record(LANLAN_CAT_CLEANING, LANLAN_SUB_EAR);
    lanlan_record_subitem_label(&cleaning, label, sizeof(label));
    assert(strcmp(label, "掏耳朵") == 0);
    strcpy(cleaning.custom_name, "擦\x02耳朵");
    lanlan_record_subitem_label(&cleaning, label, sizeof(label));
    assert(strcmp(label, "擦耳朵") == 0);

    /* No sub-item and no custom name falls back to the category label. */
    lanlan_record_t meal = make_record(LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    lanlan_record_subitem_label(&meal, label, sizeof(label));
    assert(strcmp(label, "喂食") == 0);

    /* Fallback labels must match the specification and the service's
     * authoritative display names for the hehe/yangyang accounts. */
    assert(strcmp(lanlan_caregiver_name(0), "赫赫") == 0);
    assert(strcmp(lanlan_caregiver_name(1), "羊羊") == 0);
    assert(strcmp(lanlan_caregiver_name(0), LANLAN_STR_CAREGIVERS_HEHE) == 0);
    assert(strcmp(lanlan_caregiver_name(1), LANLAN_STR_CAREGIVERS_YANGYANG) == 0);
    assert(strcmp(lanlan_caregiver_name(2), "-") == 0);

    /* 2025-10-09T08:53:20Z at +08:00 is 16:53 on 10月9日. */
    char text[16];
    assert(lanlan_record_format_local_time(&meal, text, sizeof(text)) == 5);
    assert(strcmp(text, "16:53") == 0);
    assert(lanlan_record_format_local_date(&meal, text, sizeof(text)) > 0);
    assert(strcmp(text, "10月9日") == 0);

    /* A UTC-5 offset can push the local date back one day. */
    meal.occurred_epoch = 1760000000; /* 08:53 UTC */
    meal.occurred_tz_offset_min = -300;
    lanlan_record_format_local_time(&meal, text, sizeof(text));
    assert(strcmp(text, "03:53") == 0);
    assert(lanlan_record_format_local_date(&meal, text, sizeof(text)) > 0);
    assert(strcmp(text, "10月9日") == 0);
    /* Detailed and compact views share the same formatted values. */
    assert(strcmp(lanlan_record_error_name(LANLAN_RECORD_ERR_UNIT), "unit") == 0);
    assert(strcmp(lanlan_record_error_name(LANLAN_RECORD_OK), "ok") == 0);
}

static void test_fixed_label_inventory(void) {
    /* The generated font inventory must cover every fixed label in this file;
     * the array exists so a new label cannot be added without a font update. */
    assert(sizeof(LANLAN_RECORD_FIXED_LABELS) / sizeof(LANLAN_RECORD_FIXED_LABELS[0]) == 10);
    for (size_t i = 0; i < 10; ++i) {
        assert(LANLAN_RECORD_FIXED_LABELS[i] != NULL);
        assert(LANLAN_RECORD_FIXED_LABELS[i][0] != '\0');
    }
    assert(strcmp(LANLAN_RECORD_FIXED_LABELS[7], LANLAN_STR_QUANTITY_DURATION_MINUTES) == 0);
    assert(strcmp(LANLAN_RECORD_FIXED_LABELS[8], LANLAN_STR_QUANTITY_UNKNOWN) == 0);
    assert(strcmp(LANLAN_RECORD_FIXED_LABELS[9], "（预览已截断）") == 0);
}

int main(void) {
    test_category_validation_matrix();
    test_wire_tokens();
    test_unknown_amount_is_never_zero();
    test_amount_and_duration_formatting();
    test_record_validation_limits();
    test_utf8_truncation_and_control_sanitizing();
    test_note_preview_bound();
    test_labels_and_time_formatting();
    test_fixed_label_inventory();
    puts("Lanlan record: PASS (validation matrix, unknown amount, limits, UTF-8, note preview, formatting)");
    return 0;
}

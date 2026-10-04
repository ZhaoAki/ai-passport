/* Host tests for main/lanlan_caregiver.c.
 * Build:  cc -std=c11 -Wall -Wextra -Werror -Imain \
 *             tests/test_lanlan_caregiver.c main/lanlan_caregiver.c \
 *             -o /tmp/t && /tmp/t
 * Covers: id-keyed slot assignment and first-seen stability, service member
 * ordering, UTF-8 truncation and rejection, control-character rejection, the
 * neutral fallback label, and the versioned `care_v1` codec including every
 * rejection path. */
#include "lanlan_caregiver.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers -- */

static uint32_t test_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* Recomputes the trailer so a targeted field edit is what the decoder reacts
 * to, not the CRC that would otherwise catch it first. */
static void reseal(uint8_t *blob) {
    size_t crc_pos = LANLAN_CAREGIVER_BLOB_BYTES - 4u;
    uint32_t crc = test_crc32(blob, crc_pos);
    blob[crc_pos] = (uint8_t)(crc & 0xFFu);
    blob[crc_pos + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    blob[crc_pos + 2] = (uint8_t)((crc >> 16) & 0xFFu);
    blob[crc_pos + 3] = (uint8_t)((crc >> 24) & 0xFFu);
}

static bool name_is_valid_utf8(const char *text) {
    size_t length = strlen(text);
    size_t i = 0;
    while (i < length) {
        unsigned char lead = (unsigned char)text[i];
        size_t need;
        if (lead < 0x80u) {
            need = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            need = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            need = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            need = 4;
        } else {
            return false;
        }
        if (i + need > length) return false;
        for (size_t k = 1; k < need; ++k) {
            if (((unsigned char)text[i + k] & 0xC0u) != 0x80u) return false;
        }
        i += need;
    }
    return true;
}

/* ------------------------------------------------------- assignment rules -- */

static void test_first_seen_slot_assignment_is_by_id(void) {
    lanlan_caregiver_table_t table;
    lanlan_caregiver_clear(&table);

    assert(lanlan_caregiver_learn(&table, "uid-b", "yangyang") == 0);
    assert(lanlan_caregiver_learn(&table, "uid-a", "hehe") == 1);
    assert(lanlan_caregiver_index_for_id(&table, "uid-b") == 0);
    assert(lanlan_caregiver_index_for_id(&table, "uid-a") == 1);
    assert(strcmp(lanlan_caregiver_name_at(&table, 0), "yangyang") == 0);
    assert(strcmp(lanlan_caregiver_name_at(&table, 1), "hehe") == 0);

    /* Re-learning an id never moves it, whatever order the next batch uses. */
    assert(lanlan_caregiver_learn(&table, "uid-a", "hehe") == 1);
    assert(lanlan_caregiver_learn(&table, "uid-b", "yangyang") == 0);
    assert(lanlan_caregiver_index_for_id(&table, "uid-a") == 1);
    assert(lanlan_caregiver_index_for_id(&table, "uid-b") == 0);

    /* A known id may refresh its display name without changing slots. */
    assert(lanlan_caregiver_learn(&table, "uid-a", "hehe-renamed") == 1);
    assert(strcmp(lanlan_caregiver_name_at(&table, 1), "hehe-renamed") == 0);
    assert(lanlan_caregiver_index_for_id(&table, "uid-b") == 0);

    /* Unknown ids and malformed ids are refused once both slots are taken. */
    assert(lanlan_caregiver_learn(&table, "uid-c", "third") == -1);
    assert(lanlan_caregiver_index_for_id(&table, "uid-c") == -1);
    assert(lanlan_caregiver_learn(&table, "", "empty") == -1);
    assert(lanlan_caregiver_learn(&table, NULL, "null") == -1);
    assert(lanlan_caregiver_learn(&table, "with space", "space") == -1);
    assert(lanlan_caregiver_learn(&table, "with\nnewline", "control") == -1);
    assert(strcmp(lanlan_caregiver_name_for_id(&table, "uid-c"),
                  lanlan_caregiver_fallback_label())
           == 0);
}

static void test_members_seed_the_service_order(void) {
    /* A fresh device takes the service's own order, which is what removes the
     * arrival-order swap. */
    lanlan_caregiver_table_t table;
    lanlan_caregiver_clear(&table);
    const lanlan_caregiver_member_t members[2] = {
        {.id = "hehe-id", .display_name = "hehe"},
        {.id = "yang-id", .display_name = "yang"},
    };
    lanlan_caregiver_apply_members(&table, members, 2);
    assert(lanlan_caregiver_index_for_id(&table, "hehe-id") == 0);
    assert(lanlan_caregiver_index_for_id(&table, "yang-id") == 1);
    assert(strcmp(lanlan_caregiver_name_at(&table, 0), "hehe") == 0);
    assert(strcmp(lanlan_caregiver_name_at(&table, 1), "yang") == 0);

    /* A later payload that lists them in the other order, or repeats them, must
     * not move an id that is already known. */
    const lanlan_caregiver_member_t reversed[2] = {
        {.id = "yang-id", .display_name = "yang"},
        {.id = "hehe-id", .display_name = "hehe"},
    };
    lanlan_caregiver_apply_members(&table, reversed, 2);
    assert(lanlan_caregiver_index_for_id(&table, "hehe-id") == 0);
    assert(lanlan_caregiver_index_for_id(&table, "yang-id") == 1);

    /* An absent list is "no update", and an entry without an id is skipped. */
    lanlan_caregiver_table_t before = table;
    lanlan_caregiver_apply_members(&table, NULL, 0);
    lanlan_caregiver_apply_members(&table, members, 0);
    assert(memcmp(&before, &table, sizeof(table)) == 0);
    const lanlan_caregiver_member_t nameless[3] = {
        {.id = NULL, .display_name = "nobody"},
        {.id = "", .display_name = "nobody"},
        {.id = "hehe-id", .display_name = NULL},
    };
    lanlan_caregiver_apply_members(&table, nameless, 3);
    assert(lanlan_caregiver_index_for_id(&table, "hehe-id") == 0);
    /* A NULL display name clears the stored name, so it renders as unknown
     * rather than keeping a stale label. */
    assert(strcmp(lanlan_caregiver_name_at(&table, 0), lanlan_caregiver_fallback_label()) == 0);
    assert(lanlan_caregiver_index_for_id(&table, "yang-id") == 1);
}

/* The exact shape the service pins in services/lanlan/tests/test_sync_protocol.py:
 * two members in creation order, three fields each, display names \u8D6B\u8D6B and
 * \u7F8A\u7F8A. The names are written as UTF-8 byte escapes so this file contains no
 * literal Chinese; the bytes are what the device receives. */
static void test_service_member_payload_shape(void) {
    static const char hehe_name[] = "\xE8\xB5\xAB\xE8\xB5\xAB";
    static const char yang_name[] = "\xE7\xBE\x8A\xE7\xBE\x8A";
    assert(strlen(hehe_name) == 6);
    assert(strlen(yang_name) == 6);
    assert(name_is_valid_utf8(hehe_name));
    assert(name_is_valid_utf8(yang_name));

    const lanlan_caregiver_member_t members[2] = {
        {.id = "hehe-user-id", .display_name = hehe_name},
        {.id = "yang-user-id", .display_name = yang_name},
    };
    lanlan_caregiver_table_t table;
    lanlan_caregiver_clear(&table);
    lanlan_caregiver_apply_members(&table, members, 2);
    /* Slots follow the service order, which is what removes the arrival-order
     * swap: a record attributed to "yang-user-id" is slot 1 and shows its own
     * name, whatever order the records themselves arrive in. */
    assert(lanlan_caregiver_index_for_id(&table, "hehe-user-id") == 0);
    assert(lanlan_caregiver_index_for_id(&table, "yang-user-id") == 1);
    assert(strcmp(lanlan_caregiver_name_at(&table, 0), hehe_name) == 0);
    assert(strcmp(lanlan_caregiver_name_at(&table, 1), yang_name) == 0);
    assert(strcmp(lanlan_caregiver_name_for_id(&table, "yang-user-id"), yang_name) == 0);

    /* A record lists its creator first here: the mapping must not depend on the
     * order the ids appear in the record either. */
    assert(lanlan_caregiver_index_for_id(&table, "yang-user-id") == 1);
    assert(lanlan_caregiver_index_for_id(&table, "hehe-user-id") == 0);

    /* The directory survives the NVS round trip byte for byte. */
    uint8_t blob[LANLAN_CAREGIVER_BLOB_BYTES];
    assert(lanlan_caregiver_encode(&table, blob, sizeof(blob)) == LANLAN_CAREGIVER_BLOB_BYTES);
    lanlan_caregiver_table_t decoded;
    assert(lanlan_caregiver_decode(blob, sizeof(blob), &decoded) == LANLAN_CAREGIVER_OK);
    assert(strcmp(lanlan_caregiver_name_at(&decoded, 0), hehe_name) == 0);
    assert(strcmp(lanlan_caregiver_name_at(&decoded, 1), yang_name) == 0);
}

/* ------------------------------------------------------------ text rules -- */

static void test_utf8_truncation(void) {
    char out[LANLAN_CAREGIVER_NAME_BYTES];
    /* 9 three-byte characters = 27 bytes: the cut lands on a boundary and keeps
     * exactly 8 characters (24 bytes). */
    const char *nine = "\xE6\xB1\xAA\xE6\xB1\xAA\xE6\xB1\xAA\xE6\xB1\xAA\xE6\xB1\xAA"
                       "\xE6\xB1\xAA\xE6\xB1\xAA\xE6\xB1\xAA\xE6\xB1\xAA";
    assert(strlen(nine) == 27);
    assert(lanlan_caregiver_sanitize_name(nine, out, sizeof(out)) == 24);
    assert(strlen(out) == 24);
    assert(name_is_valid_utf8(out));
    assert(out[24] == '\0');

    /* 13 two-byte characters = 26 bytes: 12 characters fit exactly. */
    const char *thirteen = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                           "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    assert(strlen(thirteen) == 26);
    assert(lanlan_caregiver_sanitize_name(thirteen, out, sizeof(out)) == 24);
    assert(name_is_valid_utf8(out));

    /* A character that would straddle the limit is dropped whole, never split. */
    const char *eight_and_a_half = "\xE6\xB1\xAA\xE6\xB1\xAA\xE6\xB1\xAA\xE6\xB1\xAA"
                                   "\xE6\xB1\xAA\xE6\xB1\xAA\xE6\xB1\xAA\xE6\xB1\xAA"
                                   "\xE6\xB1\xAA";
    assert(strlen(eight_and_a_half) == 27);
    assert(lanlan_caregiver_sanitize_name(eight_and_a_half, out, sizeof(out)) == 24);

    /* Shorter than the limit is copied unchanged. */
    assert(lanlan_caregiver_sanitize_name("hehe", out, sizeof(out)) == 4);
    assert(strcmp(out, "hehe") == 0);

    /* The encoded field holds the truncated name and re-decodes to it. */
    lanlan_caregiver_table_t table;
    lanlan_caregiver_clear(&table);
    assert(lanlan_caregiver_learn(&table, "uid-a", nine) == 0);
    assert(strlen(lanlan_caregiver_name_at(&table, 0)) == 24);
    uint8_t blob[LANLAN_CAREGIVER_BLOB_BYTES];
    assert(lanlan_caregiver_encode(&table, blob, sizeof(blob)) == LANLAN_CAREGIVER_BLOB_BYTES);
    lanlan_caregiver_table_t decoded;
    assert(lanlan_caregiver_decode(blob, sizeof(blob), &decoded) == LANLAN_CAREGIVER_OK);
    assert(strcmp(lanlan_caregiver_name_at(&decoded, 0), lanlan_caregiver_name_at(&table, 0))
           == 0);
}

static void test_control_characters_and_malformed_utf8_are_rejected(void) {
    char out[LANLAN_CAREGIVER_NAME_BYTES];
    assert(!lanlan_caregiver_name_is_valid(NULL));
    assert(!lanlan_caregiver_name_is_valid(""));
    assert(!lanlan_caregiver_name_is_valid("line\nbreak"));
    assert(!lanlan_caregiver_name_is_valid("tab\there"));
    assert(!lanlan_caregiver_name_is_valid("del\x7F"));
    assert(!lanlan_caregiver_name_is_valid("c1\xC2\x9B"));   /* U+009B */
    assert(!lanlan_caregiver_name_is_valid("bad\xFF"));      /* invalid lead */
    assert(!lanlan_caregiver_name_is_valid("cut\xE6\xB1"));  /* truncated char */
    assert(!lanlan_caregiver_name_is_valid("over\xC0\xAF")); /* overlong */
    assert(lanlan_caregiver_name_is_valid("hehe"));
    assert(lanlan_caregiver_name_is_valid("\xE6\xB1\xAA\xE6\xB1\xAA"));

    /* A rejected name yields an empty string, so callers cannot store it by
     * accident, and the module falls back to the neutral label. */
    assert(lanlan_caregiver_sanitize_name("line\nbreak", out, sizeof(out)) == 0);
    assert(out[0] == '\0');
    assert(lanlan_caregiver_sanitize_name(NULL, out, sizeof(out)) == 0);
    assert(out[0] == '\0');

    lanlan_caregiver_table_t table;
    lanlan_caregiver_clear(&table);
    /* The id is still learned: it is a valid service token even when the label
     * the service attached to it is not usable. */
    assert(lanlan_caregiver_learn(&table, "uid-a", "bad\x01name") == 0);
    assert(strcmp(lanlan_caregiver_name_at(&table, 0), lanlan_caregiver_fallback_label()) == 0);
    assert(lanlan_caregiver_index_for_id(&table, "uid-a") == 0);
}

/* --------------------------------------------------------------- display -- */

static void test_neutral_fallback_label(void) {
    const char *label = lanlan_caregiver_fallback_label();
    assert(label != NULL);
    assert(label[0] != '\0');

    lanlan_caregiver_table_t table;
    lanlan_caregiver_clear(&table);
    /* An empty directory, an out-of-range slot and an unknown id all answer with
     * the label, never NULL, never "", never an id or a slot number. */
    assert(strcmp(lanlan_caregiver_name_at(&table, 0), label) == 0);
    assert(strcmp(lanlan_caregiver_name_at(&table, 1), label) == 0);
    assert(strcmp(lanlan_caregiver_name_at(&table, 99), label) == 0);
    assert(strcmp(lanlan_caregiver_name_at(NULL, 0), label) == 0);
    assert(strcmp(lanlan_caregiver_name_for_id(&table, "nobody"), label) == 0);
    assert(strcmp(lanlan_caregiver_name_for_id(&table, ""), label) == 0);
    assert(strcmp(lanlan_caregiver_name_for_id(&table, NULL), label) == 0);
    assert(strcmp(lanlan_caregiver_name_for_id(NULL, "nobody"), label) == 0);

    /* Learning an id without a name keeps the id and reports the label. */
    assert(lanlan_caregiver_learn(&table, "uid-a", NULL) == 0);
    assert(strcmp(lanlan_caregiver_name_at(&table, 0), label) == 0);
    assert(strcmp(lanlan_caregiver_name_for_id(&table, "uid-a"), label) == 0);
    assert(strcmp(lanlan_caregiver_name_for_id(&table, "uid-a"), "uid-a") != 0);
}

/* ----------------------------------------------------------------- codec -- */

static void build_sample(lanlan_caregiver_table_t *table) {
    lanlan_caregiver_clear(table);
    assert(lanlan_caregiver_learn(table, "hehe-id", "hehe") == 0);
    assert(lanlan_caregiver_learn(table, "yang-id", "yang") == 1);
}

static void test_encode_decode_round_trip(void) {
    uint8_t blob[LANLAN_CAREGIVER_BLOB_BYTES];
    lanlan_caregiver_table_t table;
    build_sample(&table);
    assert(lanlan_caregiver_encode(&table, blob, sizeof(blob)) == LANLAN_CAREGIVER_BLOB_BYTES);
    lanlan_caregiver_table_t decoded;
    assert(lanlan_caregiver_decode(blob, sizeof(blob), &decoded) == LANLAN_CAREGIVER_OK);
    assert(memcmp(&table, &decoded, sizeof(table)) == 0);
    assert(lanlan_caregiver_index_for_id(&decoded, "hehe-id") == 0);
    assert(strcmp(lanlan_caregiver_name_at(&decoded, 1), "yang") == 0);

    /* An empty directory round-trips too, and an oversized output buffer is
     * accepted (the trailing bytes are padding, not state). */
    lanlan_caregiver_table_t empty;
    lanlan_caregiver_clear(&empty);
    assert(lanlan_caregiver_encode(&empty, blob, sizeof(blob)) == LANLAN_CAREGIVER_BLOB_BYTES);
    assert(lanlan_caregiver_decode(blob, sizeof(blob), &decoded) == LANLAN_CAREGIVER_OK);
    assert(decoded.used_mask == 0);
    assert(strcmp(lanlan_caregiver_name_at(&decoded, 0), lanlan_caregiver_fallback_label()) == 0);

    /* Refusals on the encode side. */
    assert(lanlan_caregiver_encode(NULL, blob, sizeof(blob)) == 0);
    assert(lanlan_caregiver_encode(&table, NULL, sizeof(blob)) == 0);
    assert(lanlan_caregiver_encode(&table, blob, LANLAN_CAREGIVER_BLOB_BYTES - 1u) == 0);
}

static void test_decode_rejections(void) {
    uint8_t blob[LANLAN_CAREGIVER_BLOB_BYTES];
    lanlan_caregiver_table_t table;
    lanlan_caregiver_table_t decoded;
    build_sample(&table);
    assert(lanlan_caregiver_encode(&table, blob, sizeof(blob)) == LANLAN_CAREGIVER_BLOB_BYTES);

    /* Short payload. */
    assert(lanlan_caregiver_decode(blob, LANLAN_CAREGIVER_BLOB_BYTES - 1u, &decoded)
           == LANLAN_CAREGIVER_TRUNCATED);
    assert(decoded.used_mask == 0);
    assert(lanlan_caregiver_decode(blob, 0, &decoded) == LANLAN_CAREGIVER_TRUNCATED);
    assert(lanlan_caregiver_decode(NULL, sizeof(blob), &decoded) == LANLAN_CAREGIVER_TRUNCATED);
    assert(lanlan_caregiver_decode(blob, sizeof(blob), NULL) == LANLAN_CAREGIVER_CORRUPT);

    /* A different format version is reported apart from damage. */
    uint8_t copy[LANLAN_CAREGIVER_BLOB_BYTES];
    memcpy(copy, blob, sizeof(copy));
    copy[2] = (uint8_t)(LANLAN_CAREGIVER_BLOB_VERSION + 1u);
    reseal(copy);
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded)
           == LANLAN_CAREGIVER_VERSION_MISMATCH);
    assert(decoded.used_mask == 0);

    /* Wrong magic. */
    memcpy(copy, blob, sizeof(copy));
    copy[0] = 'X';
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_CORRUPT);

    /* A damaged payload byte fails the CRC. */
    memcpy(copy, blob, sizeof(copy));
    copy[10] ^= 0xFF;
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_CORRUPT);

    /* A damaged trailer fails the CRC. */
    memcpy(copy, blob, sizeof(copy));
    copy[LANLAN_CAREGIVER_BLOB_BYTES - 1u] ^= 0x01;
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_CORRUPT);

    /* A reserved mask bit is refused even with a valid CRC. */
    memcpy(copy, blob, sizeof(copy));
    copy[3] = 0x80;
    reseal(copy);
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_CORRUPT);

    /* A used slot must carry a usable id: an all-zero id is refused. */
    memcpy(copy, blob, sizeof(copy));
    memset(copy + 4, 0, LANLAN_CAREGIVER_ID_BYTES);
    reseal(copy);
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_CORRUPT);

    /* An id with a control byte is refused. */
    memcpy(copy, blob, sizeof(copy));
    copy[4] = (uint8_t)'\n';
    reseal(copy);
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_CORRUPT);

    /* A name that trips the control-character rule is refused. */
    memcpy(copy, blob, sizeof(copy));
    size_t name_pos = 4u + LANLAN_CAREGIVER_SLOTS * LANLAN_CAREGIVER_ID_BYTES;
    copy[name_pos] = (uint8_t)'\x01';
    reseal(copy);
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_CORRUPT);

    /* A name that is not terminated inside its own field is refused. */
    memcpy(copy, blob, sizeof(copy));
    memset(copy + name_pos, (uint8_t)'a', LANLAN_CAREGIVER_NAME_BYTES);
    reseal(copy);
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_CORRUPT);

    /* An unused slot must be zero: filler in slot 1 with slot 1 unset is
     * refused, which is what proves the writer and the reader agree. */
    lanlan_caregiver_table_t single;
    lanlan_caregiver_clear(&single);
    assert(lanlan_caregiver_learn(&single, "hehe-id", "hehe") == 0);
    assert(lanlan_caregiver_encode(&single, copy, sizeof(copy)) == LANLAN_CAREGIVER_BLOB_BYTES);
    copy[4u + LANLAN_CAREGIVER_ID_BYTES] = (uint8_t)'x';
    reseal(copy);
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_CORRUPT);

    /* A learned id with an empty name is a legitimate state and survives. */
    lanlan_caregiver_table_t unnamed;
    lanlan_caregiver_clear(&unnamed);
    assert(lanlan_caregiver_learn(&unnamed, "hehe-id", NULL) == 0);
    assert(lanlan_caregiver_encode(&unnamed, copy, sizeof(copy)) == LANLAN_CAREGIVER_BLOB_BYTES);
    assert(lanlan_caregiver_decode(copy, sizeof(copy), &decoded) == LANLAN_CAREGIVER_OK);
    assert(lanlan_caregiver_index_for_id(&decoded, "hehe-id") == 0);
    assert(strcmp(lanlan_caregiver_name_at(&decoded, 0), lanlan_caregiver_fallback_label()) == 0);
}

static void test_status_names_and_bounds(void) {
    assert(strcmp(lanlan_caregiver_status_name(LANLAN_CAREGIVER_OK), "ok") == 0);
    assert(strcmp(lanlan_caregiver_status_name(LANLAN_CAREGIVER_CORRUPT), "corrupt") == 0);
    assert(strcmp(lanlan_caregiver_status_name(LANLAN_CAREGIVER_VERSION_MISMATCH),
                  "version_mismatch")
           == 0);
    assert(strcmp(lanlan_caregiver_status_name(LANLAN_CAREGIVER_TRUNCATED), "truncated") == 0);
    assert(strcmp(lanlan_caregiver_status_name((lanlan_caregiver_status_t)99), "unknown") == 0);

    /* The declared budgets hold on this compiler. */
    assert(LANLAN_CAREGIVER_ID_BYTES >= 41u);
    assert(sizeof(lanlan_caregiver_table_t) <= LANLAN_CAREGIVER_BLOB_BYTES);

    /* An id longer than the field is refused rather than truncated. */
    char long_id[LANLAN_CAREGIVER_ID_BYTES + 8];
    memset(long_id, 'a', sizeof(long_id) - 1u);
    long_id[sizeof(long_id) - 1u] = '\0';
    lanlan_caregiver_table_t table;
    lanlan_caregiver_clear(&table);
    assert(lanlan_caregiver_learn(&table, long_id, "hehe") == -1);
    assert(table.used_mask == 0);
}

int main(void) {
    test_first_seen_slot_assignment_is_by_id();
    test_members_seed_the_service_order();
    test_service_member_payload_shape();
    test_utf8_truncation();
    test_control_characters_and_malformed_utf8_are_rejected();
    test_neutral_fallback_label();
    test_encode_decode_round_trip();
    test_decode_rejections();
    test_status_names_and_bounds();
    puts("Lanlan caregiver: PASS (id-keyed slots, member order, UTF-8 rules, fallback, care_v1 codec)");
    return 0;
}

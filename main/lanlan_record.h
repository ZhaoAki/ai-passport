/* Cyber Lanlan record model: fixed-size record plus category, unit and
 * formatting rules. Pure logic: no ESP-IDF, no LVGL, no dynamic allocation.
 *
 * The server record model is frozen in docs/applications/cyber-lanlan-service.md
 * ("4.2 Field semantics" and "4.3 Category and unit validation"). This header
 * mirrors that table so a device-side validator bug cannot invent a row the
 * service would reject, and so the cache decoder can prove a decoded entry is
 * self-consistent before the UI renders it.
 *
 * One deliberate difference from the server: the device keeps only a bounded
 * NOTE PREVIEW, not the full 200-character note. The full note stays on the
 * service and on the phone; the device truncation is explicit and visible
 * (see lanlan_record_note_preview()). The bound exists because the whole cache
 * must fit the frozen 24 KB NVS partition. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* LANLAN_STR_COUNT and the generated string ids. */
#include "lanlan_strings.h"

/* ---------------------------------------------------------------- bounds -- */

/* Stable server id (records.records.id) is a UUIDv4 string; the cache stores
 * the 16 raw bytes of the UUID, not its 36-character text form. */
#define LANLAN_ID_BYTES 16
/* Service rule: note is at most 200 characters. A longer note is cut to the
 * device preview and flagged, never rejected: the full text is still on the
 * service and the phone. */
#define LANLAN_RECORD_NOTE_CHARS 200
/* Device-side note preview bound. 48 bytes holds 16 Chinese characters or 12
 * emoji, plus the explicit cut marker appended by lanlan_record_note_preview()
 * (which needs room for its own bytes too). Keeping the preview this small is
 * what lets sizeof(lanlan_record_t) stay within 176 bytes and the serialized
 * cache stay within 8192 bytes. */
#define LANLAN_RECORD_NOTE_PREVIEW_BYTES 48
/* Service rule: custom_name is 1 to 12 characters; 24 bytes holds 12 Chinese
 * characters exactly. */
#define LANLAN_RECORD_CUSTOM_CHARS 12
#define LANLAN_RECORD_CUSTOM_BYTES 24
/* Service rule: amount_value must be greater than 0 and at most 9999. */
#define LANLAN_RECORD_AMOUNT_MAX 9999
/* Service rule: duration_minutes is 1 to 1440 minutes. */
#define LANLAN_RECORD_DURATION_MAX 1440
/* Half-open amount used to detect the "set" state of a zero-valued amount.
 * Values at or below this magnitude are treated as unset; see
 * lanlan_record_set_amount_unknown() for the normative representation. */
#define LANLAN_RECORD_AMOUNT_EPSILON 0.001

/* ------------------------------------------------------------- data model -- */

typedef enum {
    LANLAN_CAT_MEAL = 0,
    LANLAN_CAT_WATER,
    LANLAN_CAT_CARE,
    LANLAN_CAT_CLEANING,
    LANLAN_CAT_WALK,
    LANLAN_CAT_OTHER,
    LANLAN_CAT_COUNT
} lanlan_category_t;

/* Sub-item keys. The wire value "other" is shared by care and cleaning, so the
 * enum keeps distinct values and the category disambiguates the wire string;
 * see lanlan_record_subitem_wire(). */
typedef enum {
    LANLAN_SUB_NONE = 0,
    LANLAN_SUB_BATH,
    LANLAN_SUB_GROOMING,
    LANLAN_SUB_TEETH,
    LANLAN_SUB_COMB,
    LANLAN_SUB_CARE_OTHER,
    LANLAN_SUB_EAR,
    LANLAN_SUB_PAW,
    LANLAN_SUB_PAD,
    LANLAN_SUB_LITTER,
    LANLAN_SUB_CLEANING_OTHER,
    LANLAN_SUB_COUNT
} lanlan_subitem_t;

typedef enum {
    LANLAN_UNIT_NONE = 0,
    LANLAN_UNIT_G,
    LANLAN_UNIT_ML,
    LANLAN_UNIT_SCOOP,
    LANLAN_UNIT_CUP,
    LANLAN_UNIT_PIECE,
    LANLAN_UNIT_BAG,
    LANLAN_UNIT_BOWL,
    LANLAN_UNIT_COUNT
} lanlan_unit_t;

typedef enum {
    LANLAN_TIME_CONFIDENCE_TRUSTED = 0,
    LANLAN_TIME_CONFIDENCE_ESTIMATED
} lanlan_time_confidence_t;

typedef enum {
    LANLAN_STATUS_ACTIVE = 0,
    LANLAN_STATUS_REVOKED
} lanlan_record_status_t;

/* Fixed-size record, kept within 176 bytes (asserted in lanlan_record.c). The
 * 64-bit members come first to minimise padding. Copyable by value and safe to
 * memcpy into a cache slot: no pointers, no variable-length members. */
typedef struct {
    int64_t occurred_epoch;        /* event time, UTC seconds */
    int64_t created_epoch;         /* server time of the accepted revision */
    uint8_t id[LANLAN_ID_BYTES];   /* binary UUID, 16 bytes */
    char note[LANLAN_RECORD_NOTE_PREVIEW_BYTES];  /* bounded preview, UTF-8 */
    char custom_name[LANLAN_RECORD_CUSTOM_BYTES]; /* short owner-defined name */
    double amount_value;           /* meaningful only while amount_known */
    uint32_t version;              /* 1 on creation, +1 per accepted revision */
    uint32_t seq;                  /* service change order; 0 when unsynced */
    int16_t occurred_tz_offset_min;/* offset the caregiver entered the time in */
    uint16_t duration_minutes;     /* 0 = unset; walk only */
    lanlan_category_t category;
    lanlan_subitem_t subitem;      /* LANLAN_SUB_NONE when the category has none */
    lanlan_unit_t unit;            /* LANLAN_UNIT_NONE when no amount is known */
    lanlan_time_confidence_t time_confidence;
    lanlan_record_status_t status;
    uint8_t created_by;            /* caregiver index 0/1 (Hehe/Yangyang) */
    uint8_t performed_by;          /* caregiver index 0/1; defaults to creator */
    bool amount_known;             /* false: no amount; must never render as 0 */
    /* True when the stored note is only a prefix of a longer server note. The
     * UI must say so instead of pretending the note is complete. */
    bool note_truncated;
} lanlan_record_t;

/* Machine-readable validation reasons. The first entry is the success value;
 * callers may persist or log the numeric code without text. */
typedef enum {
    LANLAN_RECORD_OK = 0,
    LANLAN_RECORD_ERR_CATEGORY,
    LANLAN_RECORD_ERR_SUBITEM,
    LANLAN_RECORD_ERR_UNIT,
    LANLAN_RECORD_ERR_AMOUNT,
    LANLAN_RECORD_ERR_DURATION,
    LANLAN_RECORD_ERR_CUSTOM_NAME,
    LANLAN_RECORD_ERR_NOTE,
    LANLAN_RECORD_ERR_TIME_CONFIDENCE,
    LANLAN_RECORD_ERR_STATUS,
    LANLAN_RECORD_ERR_REVISION,
    LANLAN_RECORD_ERR_CAREGIVER,
    LANLAN_RECORD_ERR_TIME_RANGE,
    LANLAN_RECORD_ERR_SEQ
} lanlan_record_error_t;

/* Short ASCII reason token for logs and tests. */
const char *lanlan_record_error_name(lanlan_record_error_t error);

/* ---------------------------------------------------------------- tables -- */

/* The fixed device-side labels, in the order the UI needs them. Kept in one
 * place so the font generator can derive the required glyph inventory from
 * this translation unit plus main/lanlan/strings.json. */
extern const char *const
    LANLAN_RECORD_FIXED_LABELS[/* marker for tools/generate_lanlan_assets.py */ 10];

const char *lanlan_category_label(lanlan_category_t category);

/* Sub-item or custom name, per the server table: care/bath, cleaning/ear and
 * friends use their fixed label, while care_other/cleaning_other/other use the
 * owner-defined name. Returns "" when the category has no sub-item and no
 * custom name. src is sanitized and truncated, and the result is always a
 * valid, terminated UTF-8 string. */
void lanlan_record_subitem_label(const lanlan_record_t *record, char *out, size_t out_size);

/* Server wire tokens ("meal", "care_other" -> "other", "cleaning_other" ->
 * "other"). Returns NULL when the combination is not a valid sub-item. */
const char *lanlan_category_wire(lanlan_category_t category);
const char *lanlan_record_subitem_wire(lanlan_category_t category, lanlan_subitem_t subitem);
const char *lanlan_unit_wire(lanlan_unit_t unit);

/* ------------------------------------------------------------ validation -- */

/* Mirrors the server's category/unit table. Any invalid combination returns a
 * non-OK reason and the caller must not accept the value. */
lanlan_record_error_t lanlan_record_validate_category(lanlan_category_t category,
                                                      lanlan_subitem_t subitem,
                                                      lanlan_unit_t unit,
                                                      bool amount_known,
                                                      bool has_custom_name);
/* Same as above, but also checks amount > 0, amount <= 9999, duration 1..1440,
 * caregiver index, time confidence and status. */
lanlan_record_error_t lanlan_record_validate(const lanlan_record_t *record);
/* Full entry point used by the cache decoder before a record enters the cache.
 * Adds the bounded preview/name byte checks and requires a non-zero id. */
lanlan_record_error_t lanlan_record_is_valid(const lanlan_record_t *record);

/* True when a real quantity is present. An unknown amount is represented by
 * amount_known == false; lanlan_record_set_amount_unknown() enforces that the
 * value is cleared too, so an unknown amount can never be read as 0. */
bool lanlan_record_amount_is_known(const lanlan_record_t *record);
/* Marks the amount unknown and zeroes the value, unit and flag together. */
void lanlan_record_set_amount_unknown(lanlan_record_t *record);
/* Sets a known amount. A value that is not finite or is out of range marks the
 * amount unknown instead of storing an invalid number. */
void lanlan_record_set_amount(lanlan_record_t *record, double value, lanlan_unit_t unit);

/* Copies a server note into the bounded preview, cutting only at a UTF-8
 * boundary. Returns true when the note was actually truncated, and stores that
 * in record->note_truncated. */
bool lanlan_record_set_note(lanlan_record_t *record, const char *note);
/* Copies a server custom name into the bounded field, cutting only at a UTF-8
 * boundary. Returns true when it was truncated. */
bool lanlan_record_set_custom_name(lanlan_record_t *record, const char *custom_name);
/* Renders the note for the UI: the stored preview plus the explicit cut marker
 * when note_truncated is set, or the "no note" text when it is empty. */
size_t lanlan_record_note_preview(const lanlan_record_t *record, char *out, size_t out_size);

/* True when every one of the 16 id bytes is zero, i.e. the id is unset. */
bool lanlan_record_id_is_zero(const uint8_t id[LANLAN_ID_BYTES]);
bool lanlan_record_id_equal(const uint8_t a[LANLAN_ID_BYTES], const uint8_t b[LANLAN_ID_BYTES]);
/* 0 when equal, -1 when a < b, 1 when a > b (memcmp order). */
int lanlan_record_id_compare(const uint8_t a[LANLAN_ID_BYTES], const uint8_t b[LANLAN_ID_BYTES]);

/* ----------------------------------------------------------- text helpers -- */

/* Copies at most max_chars UTF-8 characters (never bytes) from src to dst and
 * always terminates. A partial character at the cut point is dropped, and a
 * malformed byte sequence is replaced by U+FFFD. Returns the number of bytes
 * written, excluding the terminator. */
size_t lanlan_utf8_copy_chars(const char *src, char *dst, size_t dst_size, size_t max_chars);
/* Number of UTF-8 characters, counting malformed bytes once each. */
size_t lanlan_utf8_length(const char *text);
/* Byte length of the character starting with lead, or 1 when lead is malformed. */
size_t lanlan_utf8_char_len(unsigned char lead);
/* Drops ASCII control characters (notes are single-line) and replaces malformed
 * sequences with U+FFFD; terminates dst. Returns bytes written. */
size_t lanlan_text_sanitize(const char *src, char *dst, size_t dst_size);
/* Offset of the last complete UTF-8 character that fits in max_bytes, leaving
 * room for the terminator. */
size_t lanlan_utf8_clamp_bytes(const char *text, size_t max_bytes);

/* ------------------------------------------------------------- formatting -- */

/* "120 克" style amount; the unknown-quantity text when the amount is unknown.
 * Writes a valid UTF-8 string and returns its byte length. */
size_t lanlan_record_format_amount(const lanlan_record_t *record, char *out, size_t out_size);
/* "25 分钟", or "" when the duration is unset. */
size_t lanlan_record_format_duration(const lanlan_record_t *record, char *out, size_t out_size);
/* Local wall clock "HH:MM". */
size_t lanlan_record_format_local_time(const lanlan_record_t *record, char *out, size_t out_size);
/* Local date "M月D日" (no zero padding, matching the design's examples). */
size_t lanlan_record_format_local_date(const lanlan_record_t *record, char *out, size_t out_size);
/* Caregiver display name; index >= 2 falls back to "-". */
const char *lanlan_caregiver_name(unsigned index);

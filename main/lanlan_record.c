/* Cyber Lanlan record model implementation. See lanlan_record.h. */
#include "lanlan_record.h"
#include "lanlan_strings.h"
#include "lanlan_time.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Fixed device-side labels. The array carries one dummy element because the
 * font generator scans this translation unit for the quoted literals between
 * the LANLAN_RECORD_FIXED_LABELS markers; adding a label here without
 * regenerating the fonts makes tests/test_lanlan_assets.py fail. */
const char *const
    LANLAN_RECORD_FIXED_LABELS[/* marker for tools/generate_lanlan_assets.py */ 10] = {
        LANLAN_STR_UNITS_G,
        LANLAN_STR_UNITS_ML,
        LANLAN_STR_UNITS_SCOOP,
        LANLAN_STR_UNITS_CUP,
        LANLAN_STR_UNITS_PIECE,
        LANLAN_STR_UNITS_BAG,
        LANLAN_STR_UNITS_BOWL,
        LANLAN_STR_QUANTITY_DURATION_MINUTES,
        LANLAN_STR_QUANTITY_UNKNOWN,
        "（预览已截断）",
};

_Static_assert(sizeof(lanlan_record_t) <= 176,
               "the record budget is 176 bytes so 40 of them plus staging fit 16 KB");
_Static_assert(LANLAN_RECORD_NOTE_PREVIEW_BYTES + LANLAN_RECORD_CUSTOM_BYTES + 32
                   <= sizeof(lanlan_record_t),
               "the fixed fields must be smaller than the whole record");

/* ---------------------------------------------------------------- tables -- */

static const char *const s_category_labels[LANLAN_CAT_COUNT] = {
    LANLAN_STR_CATEGORIES_MEAL,
    LANLAN_STR_CATEGORIES_WATER,
    LANLAN_STR_CATEGORIES_CARE,
    LANLAN_STR_CATEGORIES_CLEANING,
    LANLAN_STR_CATEGORIES_WALK,
    LANLAN_STR_CATEGORIES_OTHER,
};

static const char *const s_category_wire[LANLAN_CAT_COUNT] = {
    "meal", "water", "care", "cleaning", "walk", "other",
};

static const char *const s_unit_labels[LANLAN_UNIT_COUNT] = {
    LANLAN_STR_UNITS_NONE,
    LANLAN_STR_UNITS_G,
    LANLAN_STR_UNITS_ML,
    LANLAN_STR_UNITS_SCOOP,
    LANLAN_STR_UNITS_CUP,
    LANLAN_STR_UNITS_PIECE,
    LANLAN_STR_UNITS_BAG,
    LANLAN_STR_UNITS_BOWL,
};

static const char *const s_unit_wire[LANLAN_UNIT_COUNT] = {
    NULL, "g", "ml", "scoop", "cup", "piece", "bag", "bowl",
};

/* Sub-item labels indexed by lanlan_subitem_t; LANLAN_STR_SUBITEMS_CARE_OTHER
 * is the shared fallback shown when no custom name was supplied. */
static const char *const s_subitem_labels[LANLAN_SUB_COUNT] = {
    "",
    LANLAN_STR_SUBITEMS_BATH,
    LANLAN_STR_SUBITEMS_GROOMING,
    LANLAN_STR_SUBITEMS_TEETH,
    LANLAN_STR_SUBITEMS_COMB,
    LANLAN_STR_SUBITEMS_CARE_OTHER,
    LANLAN_STR_SUBITEMS_EAR,
    LANLAN_STR_SUBITEMS_PAW,
    LANLAN_STR_SUBITEMS_PAD,
    LANLAN_STR_SUBITEMS_LITTER,
    LANLAN_STR_SUBITEMS_CLEANING_OTHER,
};

static const char *const s_caregiver_names[2] = {
    LANLAN_STR_CAREGIVERS_HEHE,
    LANLAN_STR_CAREGIVERS_YANGYANG,
};

const char *lanlan_record_error_name(lanlan_record_error_t error) {
    switch (error) {
    case LANLAN_RECORD_OK: return "ok";
    case LANLAN_RECORD_ERR_CATEGORY: return "category";
    case LANLAN_RECORD_ERR_SUBITEM: return "subitem";
    case LANLAN_RECORD_ERR_UNIT: return "unit";
    case LANLAN_RECORD_ERR_AMOUNT: return "amount";
    case LANLAN_RECORD_ERR_DURATION: return "duration";
    case LANLAN_RECORD_ERR_CUSTOM_NAME: return "custom_name";
    case LANLAN_RECORD_ERR_NOTE: return "note";
    case LANLAN_RECORD_ERR_TIME_CONFIDENCE: return "time_confidence";
    case LANLAN_RECORD_ERR_STATUS: return "status";
    case LANLAN_RECORD_ERR_REVISION: return "revision";
    case LANLAN_RECORD_ERR_CAREGIVER: return "caregiver";
    case LANLAN_RECORD_ERR_TIME_RANGE: return "time";
    case LANLAN_RECORD_ERR_SEQ: return "seq";
    }
    return "unknown";
}

const char *lanlan_category_label(lanlan_category_t category) {
    if (category < 0 || category >= LANLAN_CAT_COUNT) return "";
    return s_category_labels[category];
}

const char *lanlan_category_wire(lanlan_category_t category) {
    if (category < 0 || category >= LANLAN_CAT_COUNT) return NULL;
    return s_category_wire[category];
}

const char *lanlan_unit_wire(lanlan_unit_t unit) {
    if (unit < 0 || unit >= LANLAN_UNIT_COUNT) return NULL;
    return s_unit_wire[unit];
}

/* The server writes the shared key "other" for both care/other and
 * cleaning/other, so the wire token depends on the category. */
static const char *const s_care_sub_wire[LANLAN_SUB_CARE_OTHER + 1] = {
    NULL, "bath", "grooming", "teeth", "comb", "other",
};

const char *lanlan_record_subitem_wire(lanlan_category_t category, lanlan_subitem_t subitem) {
    if (category < 0 || category >= LANLAN_CAT_COUNT) return NULL;
    if (subitem <= LANLAN_SUB_NONE || subitem >= LANLAN_SUB_COUNT) return NULL;
    if (category == LANLAN_CAT_CARE && subitem <= LANLAN_SUB_CARE_OTHER) {
        return s_care_sub_wire[subitem];
    }
    if (category == LANLAN_CAT_CLEANING) {
        switch (subitem) {
        case LANLAN_SUB_EAR: return "ear";
        case LANLAN_SUB_PAW: return "paw";
        case LANLAN_SUB_PAD: return "pad";
        case LANLAN_SUB_LITTER: return "litter";
        case LANLAN_SUB_CLEANING_OTHER: return "other";
        default: return NULL;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ validation -- */

/* Table from docs/applications/cyber-lanlan-service.md section 4.3. */
lanlan_record_error_t lanlan_record_validate_category(lanlan_category_t category,
                                                      lanlan_subitem_t subitem,
                                                      lanlan_unit_t unit,
                                                      bool amount_known,
                                                      bool has_custom_name) {
    if (category < 0 || category >= LANLAN_CAT_COUNT) return LANLAN_RECORD_ERR_CATEGORY;
    if (subitem < 0 || subitem >= LANLAN_SUB_COUNT) return LANLAN_RECORD_ERR_SUBITEM;
    if (unit < 0 || unit >= LANLAN_UNIT_COUNT) return LANLAN_RECORD_ERR_UNIT;
    /* An unknown amount must not carry a unit; a known amount must have one. */
    if (unit == LANLAN_UNIT_NONE && amount_known) return LANLAN_RECORD_ERR_UNIT;
    if (unit != LANLAN_UNIT_NONE && !amount_known) return LANLAN_RECORD_ERR_UNIT;
    if (has_custom_name && category != LANLAN_CAT_CARE && category != LANLAN_CAT_CLEANING
        && category != LANLAN_CAT_OTHER) {
        return LANLAN_RECORD_ERR_CUSTOM_NAME;
    }

    switch (category) {
    case LANLAN_CAT_MEAL:
        if (subitem != LANLAN_SUB_NONE) return LANLAN_RECORD_ERR_SUBITEM;
        if (amount_known && !(unit == LANLAN_UNIT_G || unit == LANLAN_UNIT_ML
                              || unit == LANLAN_UNIT_SCOOP || unit == LANLAN_UNIT_CUP
                              || unit == LANLAN_UNIT_PIECE || unit == LANLAN_UNIT_BAG)) {
            return LANLAN_RECORD_ERR_UNIT;
        }
        return LANLAN_RECORD_OK;
    case LANLAN_CAT_WATER:
        if (subitem != LANLAN_SUB_NONE) return LANLAN_RECORD_ERR_SUBITEM;
        if (amount_known && !(unit == LANLAN_UNIT_ML || unit == LANLAN_UNIT_BOWL)) {
            return LANLAN_RECORD_ERR_UNIT;
        }
        return LANLAN_RECORD_OK;
    case LANLAN_CAT_CARE:
        if (unit != LANLAN_UNIT_NONE || amount_known) return LANLAN_RECORD_ERR_UNIT;
        if (subitem != LANLAN_SUB_BATH && subitem != LANLAN_SUB_GROOMING
            && subitem != LANLAN_SUB_TEETH && subitem != LANLAN_SUB_COMB
            && subitem != LANLAN_SUB_CARE_OTHER) {
            return LANLAN_RECORD_ERR_SUBITEM;
        }
        if (subitem == LANLAN_SUB_CARE_OTHER && !has_custom_name) {
            return LANLAN_RECORD_ERR_CUSTOM_NAME;
        }
        return LANLAN_RECORD_OK;
    case LANLAN_CAT_CLEANING:
        if (unit != LANLAN_UNIT_NONE || amount_known) return LANLAN_RECORD_ERR_UNIT;
        if (subitem != LANLAN_SUB_EAR && subitem != LANLAN_SUB_PAW
            && subitem != LANLAN_SUB_PAD && subitem != LANLAN_SUB_LITTER
            && subitem != LANLAN_SUB_CLEANING_OTHER) {
            return LANLAN_RECORD_ERR_SUBITEM;
        }
        if (subitem == LANLAN_SUB_CLEANING_OTHER && !has_custom_name) {
            return LANLAN_RECORD_ERR_CUSTOM_NAME;
        }
        return LANLAN_RECORD_OK;
    case LANLAN_CAT_WALK:
        if (subitem != LANLAN_SUB_NONE) return LANLAN_RECORD_ERR_SUBITEM;
        if (unit != LANLAN_UNIT_NONE || amount_known) return LANLAN_RECORD_ERR_UNIT;
        return LANLAN_RECORD_OK;
    case LANLAN_CAT_OTHER:
        if (subitem != LANLAN_SUB_NONE) return LANLAN_RECORD_ERR_SUBITEM;
        if (unit != LANLAN_UNIT_NONE || amount_known) return LANLAN_RECORD_ERR_UNIT;
        if (!has_custom_name) return LANLAN_RECORD_ERR_CUSTOM_NAME;
        return LANLAN_RECORD_OK;
    case LANLAN_CAT_COUNT:
        break;
    }
    return LANLAN_RECORD_ERR_CATEGORY;
}

bool lanlan_record_amount_is_known(const lanlan_record_t *record) {
    return record && record->amount_known && record->amount_value > LANLAN_RECORD_AMOUNT_EPSILON
           && record->amount_value <= LANLAN_RECORD_AMOUNT_MAX;
}

void lanlan_record_set_amount_unknown(lanlan_record_t *record) {
    if (!record) return;
    record->amount_known = false;
    record->amount_value = 0.0;
    record->unit = LANLAN_UNIT_NONE;
}

void lanlan_record_set_amount(lanlan_record_t *record, double value, lanlan_unit_t unit) {
    if (!record) return;
    if (unit < 0 || unit >= LANLAN_UNIT_COUNT || unit == LANLAN_UNIT_NONE
        || !(value > LANLAN_RECORD_AMOUNT_EPSILON) || value > LANLAN_RECORD_AMOUNT_MAX
        || value != value /* NaN */) {
        lanlan_record_set_amount_unknown(record);
        return;
    }
    record->amount_known = true;
    record->amount_value = value;
    record->unit = unit;
}

/* The stored note is always a byte-bounded UTF-8 prefix. Truncation is
 * reported, never hidden: a caller that drops the flag would make an
 * incomplete note look complete in the UI. */
bool lanlan_record_set_note(lanlan_record_t *record, const char *note) {
    if (!record) return false;
    const char *source = note ? note : "";
    char sanitized[LANLAN_RECORD_NOTE_CHARS * 4 + 1];
    lanlan_text_sanitize(source, sanitized, sizeof(sanitized));
    size_t limit = sizeof(record->note);
    size_t kept = lanlan_utf8_clamp_bytes(sanitized, limit);
    memcpy(record->note, sanitized, kept);
    record->note[kept] = '\0';
    record->note_truncated = sanitized[kept] != '\0';
    return record->note_truncated;
}

bool lanlan_record_set_custom_name(lanlan_record_t *record, const char *custom_name) {
    if (!record) return false;
    const char *source = custom_name ? custom_name : "";
    char sanitized[LANLAN_RECORD_CUSTOM_BYTES * 4 + 1];
    lanlan_text_sanitize(source, sanitized, sizeof(sanitized));
    size_t limit = sizeof(record->custom_name);
    size_t kept = lanlan_utf8_clamp_bytes(sanitized, limit);
    memcpy(record->custom_name, sanitized, kept);
    record->custom_name[kept] = '\0';
    return sanitized[kept] != '\0';
}

size_t lanlan_record_note_preview(const lanlan_record_t *record, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!record) return 0;
    if (record->note[0] == '\0') {
        return lanlan_utf8_copy_chars(LANLAN_STR_DETAIL_NO_NOTE, out, out_size,
                                      LANLAN_RECORD_CUSTOM_CHARS);
    }
    if (!record->note_truncated) {
        return lanlan_utf8_copy_chars(record->note, out, out_size, LANLAN_RECORD_NOTE_CHARS);
    }
    /* The marker is at the end of the fixed-label table and is picked up by the
     * font inventory, so a generated font always covers it. */
    const char *marker = LANLAN_RECORD_FIXED_LABELS[9];
    int written = snprintf(out, out_size, "%s%s", record->note, marker);
    if (written < 0) {
        out[0] = '\0';
        return 0;
    }
    if ((size_t)written >= out_size) {
        /* The caller gave a buffer too small for preview plus marker: keep the
         * marker visible so the truncation is still communicated. */
        size_t marker_bytes = strlen(marker);
        if (marker_bytes < out_size) {
            memcpy(out, marker, marker_bytes + 1);
        }
        out[out_size - 1] = '\0';
        return strlen(out);
    }
    return (size_t)written;
}

bool lanlan_record_id_is_zero(const uint8_t id[LANLAN_ID_BYTES]) {
    for (size_t i = 0; i < LANLAN_ID_BYTES; ++i) {
        if (id[i]) return false;
    }
    return true;
}

bool lanlan_record_id_equal(const uint8_t a[LANLAN_ID_BYTES], const uint8_t b[LANLAN_ID_BYTES]) {
    return memcmp(a, b, LANLAN_ID_BYTES) == 0;
}

int lanlan_record_id_compare(const uint8_t a[LANLAN_ID_BYTES], const uint8_t b[LANLAN_ID_BYTES]) {
    int order = memcmp(a, b, LANLAN_ID_BYTES);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

lanlan_record_error_t lanlan_record_validate(const lanlan_record_t *record) {
    if (!record) return LANLAN_RECORD_ERR_CATEGORY;
    if (record->time_confidence != LANLAN_TIME_CONFIDENCE_TRUSTED
        && record->time_confidence != LANLAN_TIME_CONFIDENCE_ESTIMATED) {
        return LANLAN_RECORD_ERR_TIME_CONFIDENCE;
    }
    if (record->status != LANLAN_STATUS_ACTIVE && record->status != LANLAN_STATUS_REVOKED) {
        return LANLAN_RECORD_ERR_STATUS;
    }
    if (record->version == 0) return LANLAN_RECORD_ERR_REVISION;
    if (record->created_by > 1 || record->performed_by > 1) return LANLAN_RECORD_ERR_CAREGIVER;
    if (record->occurred_tz_offset_min < LANLAN_TIME_OFFSET_MIN
        || record->occurred_tz_offset_min > LANLAN_TIME_OFFSET_MAX) {
        return LANLAN_RECORD_ERR_TIME_RANGE;
    }
    if (record->occurred_epoch <= 0 || record->created_epoch <= 0) {
        return LANLAN_RECORD_ERR_TIME_RANGE;
    }
    if (record->amount_known) {
        /* Same epsilon the setter uses: an amount that would format as "0"
         * must be stored as unknown instead, so "known" and "renderable" cannot
         * disagree. */
        if (!(record->amount_value > LANLAN_RECORD_AMOUNT_EPSILON)
            || record->amount_value > LANLAN_RECORD_AMOUNT_MAX) {
            return LANLAN_RECORD_ERR_AMOUNT;
        }
    } else if (record->amount_value != 0.0) {
        /* The unknown flag and the value must agree; otherwise a stale value
         * could leak back into the UI as if it were known. */
        return LANLAN_RECORD_ERR_AMOUNT;
    }
    if (record->duration_minutes > LANLAN_RECORD_DURATION_MAX) {
        return LANLAN_RECORD_ERR_DURATION;
    }
    if (record->category != LANLAN_CAT_WALK && record->duration_minutes != 0) {
        return LANLAN_RECORD_ERR_DURATION;
    }
    if (record->seq == 0) return LANLAN_RECORD_ERR_SEQ;
    return lanlan_record_validate_category(record->category, record->subitem, record->unit,
                                           record->amount_known,
                                           record->custom_name[0] != '\0');
}

lanlan_record_error_t lanlan_record_is_valid(const lanlan_record_t *record) {
    if (!record) return LANLAN_RECORD_ERR_CATEGORY;
    if (lanlan_record_id_is_zero(record->id)) return LANLAN_RECORD_ERR_REVISION;
    lanlan_record_error_t error = lanlan_record_validate(record);
    if (error != LANLAN_RECORD_OK) return error;
    /* The fields are byte-bounded buffers, so the checks are byte checks: a
     * string without its terminator inside its own buffer is already a bug. */
    if (strnlen(record->note, sizeof(record->note)) >= sizeof(record->note)) {
        return LANLAN_RECORD_ERR_NOTE;
    }
    if (strnlen(record->custom_name, sizeof(record->custom_name))
        >= sizeof(record->custom_name)) {
        return LANLAN_RECORD_ERR_CUSTOM_NAME;
    }
    if (lanlan_utf8_length(record->custom_name) > LANLAN_RECORD_CUSTOM_CHARS) {
        return LANLAN_RECORD_ERR_CUSTOM_NAME;
    }
    return LANLAN_RECORD_OK;
}

/* ----------------------------------------------------------- text helpers -- */

size_t lanlan_utf8_char_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1; /* stray continuation byte or invalid lead */
}

static bool is_continuation(unsigned char byte) { return (byte & 0xC0) == 0x80; }

/* Decodes one character, validating continuation bytes. Returns the number of
 * bytes consumed (1 for a malformed sequence, so the caller always advances)
 * and stores the code point; malformed input yields U+FFFD. */
static size_t utf8_next(const char *text, uint32_t *codepoint) {
    const unsigned char *p = (const unsigned char *)text;
    size_t len = lanlan_utf8_char_len(p[0]);
    static const uint32_t minima[5] = {0, 0, 0x80, 0x800, 0x10000};
    uint32_t value = 0;
    if (len == 1) {
        if (p[0] < 0x80) {
            *codepoint = p[0];
            return 1;
        }
        *codepoint = 0xFFFD;
        return 1;
    }
    value = (uint32_t)(p[0] & (0xFFu >> (len + 1)));
    for (size_t i = 1; i < len; ++i) {
        if (!is_continuation(p[i])) {
            *codepoint = 0xFFFD;
            return 1;
        }
        value = (value << 6) | (uint32_t)(p[i] & 0x3Fu);
    }
    if (value < minima[len] || value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
        *codepoint = 0xFFFD;
        return len;
    }
    *codepoint = value;
    return len;
}

static size_t utf8_encode(uint32_t codepoint, char *out) {
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

size_t lanlan_utf8_length(const char *text) {
    if (!text) return 0;
    size_t count = 0;
    for (const char *p = text; *p;) {
        uint32_t codepoint = 0;
        p += utf8_next(p, &codepoint);
        ++count;
    }
    return count;
}

size_t lanlan_text_sanitize(const char *src, char *dst, size_t dst_size) {
    if (!dst || dst_size == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;
    size_t written = 0;
    for (const char *p = src; *p;) {
        uint32_t codepoint = 0;
        size_t consumed = utf8_next(p, &codepoint);
        p += consumed;
        /* Drop C0/C1 control characters: notes are rendered on one line and a
         * newline or escape byte would corrupt the label layout. */
        if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F)) continue;
        char encoded[4];
        size_t encoded_len = utf8_encode(codepoint, encoded);
        if (written + encoded_len + 1 > dst_size) break;
        memcpy(dst + written, encoded, encoded_len);
        written += encoded_len;
    }
    dst[written] = '\0';
    return written;
}

size_t lanlan_utf8_copy_chars(const char *src, char *dst, size_t dst_size, size_t max_chars) {
    if (!dst || dst_size == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;
    size_t written = 0;
    size_t chars = 0;
    for (const char *p = src; *p && chars < max_chars;) {
        uint32_t codepoint = 0;
        size_t consumed = utf8_next(p, &codepoint);
        p += consumed;
        if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F)) continue;
        char encoded[4];
        size_t encoded_len = utf8_encode(codepoint, encoded);
        if (written + encoded_len + 1 > dst_size) break;
        memcpy(dst + written, encoded, encoded_len);
        written += encoded_len;
        ++chars;
    }
    dst[written] = '\0';
    return written;
}

size_t lanlan_utf8_clamp_bytes(const char *text, size_t max_bytes) {
    if (!text) return 0;
    /* Leave room for the terminator: a caller writing text into a buffer of
     * max_bytes uses this as the truncation point. */
    size_t limit = max_bytes > 0 ? max_bytes - 1 : 0;
    size_t offset = 0;
    size_t last_complete = 0;
    while (text[offset] && offset < limit) {
        size_t len = lanlan_utf8_char_len((unsigned char)text[offset]);
        if (offset + len > limit) break;
        bool complete = true;
        for (size_t i = 1; i < len; ++i) {
            if (!is_continuation((unsigned char)text[offset + i])) {
                complete = false;
                break;
            }
        }
        offset += complete ? len : 1;
        last_complete = offset;
    }
    return last_complete;
}

/* ------------------------------------------------------------- formatting -- */

void lanlan_record_subitem_label(const lanlan_record_t *record, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!record) return;
    /* A custom name wins whenever the server supplied one: for care/other,
     * cleaning/other and other it is mandatory, for cleaning presets it is an
     * optional label and care/bath keeps the fixed label. */
    if (record->custom_name[0] != '\0') {
        lanlan_utf8_copy_chars(record->custom_name, out, out_size, LANLAN_RECORD_CUSTOM_CHARS);
        return;
    }
    if (record->subitem > LANLAN_SUB_NONE && record->subitem < LANLAN_SUB_COUNT) {
        lanlan_utf8_copy_chars(s_subitem_labels[record->subitem], out, out_size,
                               LANLAN_RECORD_CUSTOM_CHARS);
        return;
    }
    if (record->category >= 0 && record->category < LANLAN_CAT_COUNT) {
        lanlan_utf8_copy_chars(s_category_labels[record->category], out, out_size,
                               LANLAN_RECORD_CUSTOM_CHARS);
    }
}

/* Amounts are shown with at most one decimal so the label stays short; the
 * unit table is fixed, so a client cannot invent a unit string. */
static size_t format_amount_value(double value, char *out, size_t out_size) {
    double scaled = value * 10.0;
    long long rounded = (long long)(scaled + 0.5);
    if (rounded <= 0) return (size_t)snprintf(out, out_size, "0");
    if (rounded % 10 == 0) return (size_t)snprintf(out, out_size, "%lld", rounded / 10);
    return (size_t)snprintf(out, out_size, "%lld.%lld", rounded / 10, rounded % 10);
}

size_t lanlan_record_format_amount(const lanlan_record_t *record, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!record) return 0;
    if (!lanlan_record_amount_is_known(record)) {
        return lanlan_utf8_copy_chars(LANLAN_STR_QUANTITY_UNKNOWN, out, out_size, 8);
    }
    char number[24];
    size_t number_len = format_amount_value(record->amount_value, number, sizeof(number));
    if (number_len == 0 || number_len >= sizeof(number)) {
        out[0] = '\0';
        return 0;
    }
    /* "120 克": one ASCII space between value and unit, matching the design. */
    int written = snprintf(out, out_size, "%s %s", number, s_unit_labels[record->unit]);
    if (written < 0 || (size_t)written >= out_size) {
        out[out_size - 1] = '\0';
        return strlen(out);
    }
    return (size_t)written;
}

size_t lanlan_record_format_duration(const lanlan_record_t *record, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!record || record->duration_minutes == 0) return 0;
    /* "%u" + the localized minute unit keeps the number formatting in C. */
    int written = snprintf(out, out_size, "%u %s", (unsigned)record->duration_minutes,
                           LANLAN_STR_QUANTITY_DURATION_MINUTES);
    if (written < 0 || (size_t)written >= out_size) {
        out[out_size - 1] = '\0';
        return strlen(out);
    }
    return (size_t)written;
}

size_t lanlan_record_format_local_time(const lanlan_record_t *record, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!record) return 0;
    int hour = 0;
    int minute = 0;
    if (lanlan_time_local_hhmm(record->occurred_epoch, record->occurred_tz_offset_min, &hour,
                               &minute)
        != LANLAN_TIME_OK) {
        return 0;
    }
    int written = snprintf(out, out_size, "%02d:%02d", hour, minute);
    return written < 0 ? 0 : (size_t)written;
}

size_t lanlan_record_format_local_date(const lanlan_record_t *record, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!record) return 0;
    lanlan_date_t date = {0};
    if (lanlan_time_local_date(record->occurred_epoch, record->occurred_tz_offset_min, &date)
        != LANLAN_TIME_OK) {
        return 0;
    }
    /* Month/day numbers are locale-independent digits; the unit characters
     * come from the single string source. date.day is the absolute day index,
     * so the day of the month must come from day_in_month. */
    int written = snprintf(out, out_size, "%d%s%d%s", (int)date.month,
                           LANLAN_STR_QUANTITY_DATE_MONTH_UNIT, (int)date.day_in_month,
                           LANLAN_STR_QUANTITY_DATE_DAY_UNIT);
    return written < 0 ? 0 : (size_t)written;
}

const char *lanlan_caregiver_name(unsigned index) {
    return index < 2 ? s_caregiver_names[index] : "-";
}

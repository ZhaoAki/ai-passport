/* Cyber Lanlan sync response decoding. See lanlan_sync_parse.h for the contract.
 *
 * The field handling mirrors the previous cJSON implementation exactly, including
 * its tolerance rules: an absent or null string member reads as the empty string,
 * a member of another type rejects the record, and non-number numerics are
 * ignored. Anything that rejects one record rejects the page, so a malformed
 * batch can never be half applied. */

#include "lanlan_sync_parse.h"

#include <string.h>

#include "lanlan_json.h"
#include "lanlan_record.h"
#include "lanlan_time.h"

/* Stack budget: the largest string this module copies for itself is a note or a
 * display name. Longer tokens are still validated in full by the JSON reader and
 * truncated here, which is what the bounded cache previews need anyway. */
#define PARSE_TEXT_MAX 256
#define PARSE_FIELD_MAX 64
#define PARSE_TEXT_SCRATCH 192

const char *lanlan_sync_parse_status_name(lanlan_sync_parse_status_t status) {
    switch (status) {
    case LANLAN_SYNC_PARSE_OK: return "ok";
    case LANLAN_SYNC_PARSE_MALFORMED: return "malformed";
    case LANLAN_SYNC_PARSE_MISSING_FIELD: return "missing_field";
    case LANLAN_SYNC_PARSE_WRONG_TYPE: return "wrong_type";
    case LANLAN_SYNC_PARSE_TOO_LARGE: return "too_large";
    case LANLAN_SYNC_PARSE_INVALID_VALUE: return "invalid_value";
    default: return "unknown";
    }
}

static lanlan_sync_parse_status_t map_json(lanlan_json_status_t status) {
    switch (status) {
    case LANLAN_JSON_OK: return LANLAN_SYNC_PARSE_OK;
    case LANLAN_JSON_ERR_TOO_LARGE: return LANLAN_SYNC_PARSE_TOO_LARGE;
    case LANLAN_JSON_ERR_TYPE: return LANLAN_SYNC_PARSE_WRONG_TYPE;
    case LANLAN_JSON_ERR_NOT_FOUND: return LANLAN_SYNC_PARSE_MISSING_FIELD;
    default: return LANLAN_SYNC_PARSE_MALFORMED;
    }
}

/* ------------------------------------------------------------------ members -- */

typedef enum {
    MEMBER_ABSENT = 0,
    MEMBER_NULL,
    MEMBER_STRING,
    MEMBER_NUMBER,
    MEMBER_BOOL,
    MEMBER_CONTAINER
} member_kind_t;

static member_kind_t kind_of(const lanlan_json_value_t *value) {
    switch (value->type) {
    case LANLAN_JSON_TYPE_NULL: return MEMBER_NULL;
    case LANLAN_JSON_TYPE_STRING: return MEMBER_STRING;
    case LANLAN_JSON_TYPE_NUMBER: return MEMBER_NUMBER;
    case LANLAN_JSON_TYPE_BOOL: return MEMBER_BOOL;
    default: return MEMBER_CONTAINER;
    }
}

/* Looks up one object member. An absent member is not an error; every other
 * outcome is reported through `kind`. */
static lanlan_sync_parse_status_t member_get(lanlan_json_t *json,
                                             const lanlan_json_value_t *object, const char *name,
                                             lanlan_json_value_t *value, member_kind_t *kind) {
    memset(value, 0, sizeof(*value));
    lanlan_json_status_t status = lanlan_json_object_get(json, object, name, value);
    if (status == LANLAN_JSON_ERR_NOT_FOUND) {
        *kind = MEMBER_ABSENT;
        return LANLAN_SYNC_PARSE_OK;
    }
    if (status != LANLAN_JSON_OK) return map_json(status);
    *kind = kind_of(value);
    return LANLAN_SYNC_PARSE_OK;
}

/* A string member. Absent and null both read as an empty string with
 * *present = false; a value of any other type is WRONG_TYPE, which is what the
 * previous implementation rejected. */
static lanlan_sync_parse_status_t member_text(lanlan_json_t *json,
                                              const lanlan_json_value_t *object, const char *name,
                                              char *out, size_t out_size, bool *present) {
    lanlan_json_value_t value;
    member_kind_t kind = MEMBER_ABSENT;
    lanlan_sync_parse_status_t status = member_get(json, object, name, &value, &kind);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    *present = false;
    if (out && out_size > 0) out[0] = '\0';
    if (kind == MEMBER_STRING) {
        status = map_json(lanlan_json_get_string(json, &value, out, out_size, NULL));
        if (status != LANLAN_SYNC_PARSE_OK) return status;
        *present = true;
        return LANLAN_SYNC_PARSE_OK;
    }
    if (kind == MEMBER_BOOL || kind == MEMBER_CONTAINER) return LANLAN_SYNC_PARSE_WRONG_TYPE;
    return LANLAN_SYNC_PARSE_OK;
}

/* A numeric member. Absent, null and any other type read as *present = false,
 * matching the old json_number() helper. */
static lanlan_sync_parse_status_t member_number(lanlan_json_t *json,
                                                const lanlan_json_value_t *object, const char *name,
                                                double *out, bool *present) {
    lanlan_json_value_t value;
    member_kind_t kind = MEMBER_ABSENT;
    lanlan_sync_parse_status_t status = member_get(json, object, name, &value, &kind);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    *present = false;
    *out = 0;
    if (kind != MEMBER_NUMBER) return LANLAN_SYNC_PARSE_OK;
    status = map_json(lanlan_json_get_double(json, &value, out));
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    *present = true;
    return LANLAN_SYNC_PARSE_OK;
}

/* ------------------------------------------------------------------ helpers -- */

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The service sends a UUIDv4 in text form; the cache stores its 16 raw bytes. */
static bool parse_uuid(const char *text, uint8_t out[LANLAN_ID_BYTES]) {
    if (!text || strlen(text) != 36) return false;
    size_t index = 0;
    size_t produced = 0;
    while (index < 36) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') return false;
            ++index;
            continue;
        }
        if (index + 1 >= 36 || produced >= LANLAN_ID_BYTES) return false;
        int high = hex_digit(text[index]);
        int low = hex_digit(text[index + 1]);
        if (high < 0 || low < 0) return false;
        out[produced++] = (uint8_t)((high << 4) | low);
        index += 2;
    }
    return produced == LANLAN_ID_BYTES && !lanlan_record_id_is_zero(out);
}

static lanlan_category_t parse_category(const char *text) {
    if (!text) return LANLAN_CAT_COUNT;
    if (strcmp(text, "meal") == 0) return LANLAN_CAT_MEAL;
    if (strcmp(text, "water") == 0) return LANLAN_CAT_WATER;
    if (strcmp(text, "care") == 0) return LANLAN_CAT_CARE;
    if (strcmp(text, "cleaning") == 0) return LANLAN_CAT_CLEANING;
    if (strcmp(text, "walk") == 0) return LANLAN_CAT_WALK;
    if (strcmp(text, "other") == 0) return LANLAN_CAT_OTHER;
    return LANLAN_CAT_COUNT;
}

static lanlan_subitem_t parse_subitem(lanlan_category_t category, const char *text) {
    if (!text || text[0] == '\0') return LANLAN_SUB_NONE;
    if (category == LANLAN_CAT_CARE) {
        if (strcmp(text, "bath") == 0) return LANLAN_SUB_BATH;
        if (strcmp(text, "grooming") == 0) return LANLAN_SUB_GROOMING;
        if (strcmp(text, "teeth") == 0) return LANLAN_SUB_TEETH;
        if (strcmp(text, "comb") == 0) return LANLAN_SUB_COMB;
        if (strcmp(text, "other") == 0) return LANLAN_SUB_CARE_OTHER;
        return LANLAN_SUB_COUNT;
    }
    if (category == LANLAN_CAT_CLEANING) {
        if (strcmp(text, "ear") == 0) return LANLAN_SUB_EAR;
        if (strcmp(text, "paw") == 0) return LANLAN_SUB_PAW;
        if (strcmp(text, "pad") == 0) return LANLAN_SUB_PAD;
        if (strcmp(text, "litter") == 0) return LANLAN_SUB_LITTER;
        if (strcmp(text, "other") == 0) return LANLAN_SUB_CLEANING_OTHER;
        return LANLAN_SUB_COUNT;
    }
    return LANLAN_SUB_COUNT;
}

static lanlan_unit_t parse_unit(const char *text) {
    if (!text || text[0] == '\0') return LANLAN_UNIT_NONE;
    if (strcmp(text, "g") == 0) return LANLAN_UNIT_G;
    if (strcmp(text, "ml") == 0) return LANLAN_UNIT_ML;
    if (strcmp(text, "scoop") == 0) return LANLAN_UNIT_SCOOP;
    if (strcmp(text, "cup") == 0) return LANLAN_UNIT_CUP;
    if (strcmp(text, "piece") == 0) return LANLAN_UNIT_PIECE;
    if (strcmp(text, "bag") == 0) return LANLAN_UNIT_BAG;
    if (strcmp(text, "bowl") == 0) return LANLAN_UNIT_BOWL;
    return LANLAN_UNIT_COUNT;
}

/* Copies a service string into a fixed field: sanitized, UTF-8 safe, bounded by
 * characters, always terminated. Used for the reminder custom name, which has no
 * dedicated setter in the record module. */
static void copy_text_field(const char *source, char *destination, size_t destination_size,
                            size_t max_chars) {
    if (!destination || destination_size == 0) return;
    destination[0] = '\0';
    if (!source) return;
    char sanitized[PARSE_TEXT_SCRATCH];
    (void)lanlan_text_sanitize(source, sanitized, sizeof(sanitized));
    (void)lanlan_utf8_copy_chars(sanitized, destination, destination_size, max_chars);
}

static uint8_t caregiver_index(lanlan_json_t *json, const lanlan_json_value_t *object,
                               const char *name, const lanlan_sync_parse_hooks_t *hooks,
                               lanlan_sync_parse_status_t *status) {
    char text[PARSE_FIELD_MAX];
    bool present = false;
    *status = member_text(json, object, name, text, sizeof(text), &present);
    if (*status != LANLAN_SYNC_PARSE_OK) return 0;
    if (!present || text[0] == '\0' || !hooks || !hooks->caregiver) return 0;
    uint8_t index = hooks->caregiver(text, hooks->user);
    return index < 2 ? index : 0;
}

/* ------------------------------------------------------------------ records -- */

static lanlan_sync_parse_status_t parse_record(lanlan_json_t *json,
                                               const lanlan_json_value_t *item,
                                               int16_t offset_minutes,
                                               const lanlan_sync_parse_hooks_t *hooks,
                                               lanlan_record_t *record) {
    memset(record, 0, sizeof(*record));
    lanlan_sync_parse_status_t status = LANLAN_SYNC_PARSE_OK;
    char text[PARSE_TEXT_MAX];
    bool present = false;
    double number = 0;

    status = member_text(json, item, "id", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    if (!present || !parse_uuid(text, record->id)) return LANLAN_SYNC_PARSE_INVALID_VALUE;

    status = member_number(json, item, "v", &number, &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    record->version = (present && number >= 1) ? (uint32_t)number : 1;

    status = member_text(json, item, "cat", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    record->category = parse_category(present ? text : NULL);
    if (record->category >= LANLAN_CAT_COUNT) return LANLAN_SYNC_PARSE_INVALID_VALUE;

    status = member_text(json, item, "sub", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    record->subitem = parse_subitem(record->category, present ? text : NULL);
    if (record->subitem >= LANLAN_SUB_COUNT) return LANLAN_SYNC_PARSE_INVALID_VALUE;

    status = member_text(json, item, "at", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    if (!present) return LANLAN_SYNC_PARSE_INVALID_VALUE;
    int16_t parsed_offset = 0;
    if (lanlan_time_parse_rfc3339(text, &record->occurred_epoch, &parsed_offset)
        != LANLAN_TIME_OK) {
        return LANLAN_SYNC_PARSE_INVALID_VALUE;
    }
    /* The compact record carries an IANA zone name in "tz"; the device has no
     * timezone database, so display uses the offset the response reported. */
    record->occurred_tz_offset_min = offset_minutes;
    record->created_epoch = record->occurred_epoch;

    status = member_text(json, item, "tc", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    record->time_confidence = (present && strcmp(text, "estimated") == 0)
                                  ? LANLAN_TIME_CONFIDENCE_ESTIMATED
                                  : LANLAN_TIME_CONFIDENCE_TRUSTED;

    record->created_by = caregiver_index(json, item, "by", hooks, &status);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    record->performed_by = caregiver_index(json, item, "perf", hooks, &status);
    if (status != LANLAN_SYNC_PARSE_OK) return status;

    /* The cache stores bounded previews, cut on a UTF-8 boundary by the record
     * module, which also records that the note was truncated. */
    status = member_text(json, item, "name", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    (void)lanlan_record_set_custom_name(record, present ? text : "");
    status = member_text(json, item, "note", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    (void)lanlan_record_set_note(record, present ? text : "");

    lanlan_record_set_amount_unknown(record);
    status = member_text(json, item, "unit", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    lanlan_unit_t unit = parse_unit(present ? text : NULL);
    if (unit >= LANLAN_UNIT_COUNT) return LANLAN_SYNC_PARSE_INVALID_VALUE;
    bool amount_present = false;
    status = member_number(json, item, "amt", &number, &amount_present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    if (amount_present) {
        lanlan_record_set_amount(record, number, unit);
    } else if (unit != LANLAN_UNIT_NONE) {
        /* An unknown amount is only representable without a unit. */
        return LANLAN_SYNC_PARSE_INVALID_VALUE;
    }

    status = member_number(json, item, "dur", &number, &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    if (present && number > 0) {
        if (number > LANLAN_RECORD_DURATION_MAX) return LANLAN_SYNC_PARSE_INVALID_VALUE;
        record->duration_minutes = (uint16_t)number;
    }

    status = member_text(json, item, "st", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    record->status = (present && strcmp(text, "revoked") == 0) ? LANLAN_STATUS_REVOKED
                                                              : LANLAN_STATUS_ACTIVE;

    status = member_number(json, item, "seq", &number, &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    if (present && number >= 0) record->seq = (uint32_t)number;

    if (lanlan_record_is_valid(record) != LANLAN_RECORD_OK) {
        return LANLAN_SYNC_PARSE_INVALID_VALUE;
    }
    return LANLAN_SYNC_PARSE_OK;
}

static lanlan_sync_parse_status_t parse_reminder(lanlan_json_t *json,
                                                 const lanlan_json_value_t *item,
                                                 lanlan_reminder_t *reminder) {
    memset(reminder, 0, sizeof(*reminder));
    lanlan_sync_parse_status_t status = LANLAN_SYNC_PARSE_OK;
    char text[PARSE_TEXT_MAX];
    bool present = false;
    double number = 0;

    status = member_text(json, item, "id", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    if (!present || !parse_uuid(text, reminder->id)) return LANLAN_SYNC_PARSE_INVALID_VALUE;

    status = member_number(json, item, "v", &number, &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    reminder->version = (present && number >= 1) ? (uint32_t)number : 1;

    status = member_text(json, item, "cat", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    reminder->category = parse_category(present ? text : NULL);
    if (reminder->category >= LANLAN_CAT_COUNT) return LANLAN_SYNC_PARSE_INVALID_VALUE;

    status = member_text(json, item, "sub", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    reminder->subitem = parse_subitem(reminder->category, present ? text : NULL);
    if (reminder->subitem >= LANLAN_SUB_COUNT) return LANLAN_SYNC_PARSE_INVALID_VALUE;

    status = member_text(json, item, "name", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    copy_text_field(present ? text : "", reminder->custom_name, sizeof(reminder->custom_name),
                    LANLAN_RECORD_CUSTOM_CHARS);

    status = member_number(json, item, "en", &number, &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    reminder->enabled = present && number != 0;

    status = member_text(json, item, "t", text, sizeof(text), &present);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    reminder->time_local[0] = '\0';
    if (present && text[0] != '\0') {
        int hour = 0;
        int minute = 0;
        if (!lanlan_time_parse_hhmm(text, &hour, &minute)) return LANLAN_SYNC_PARSE_INVALID_VALUE;
        if (lanlan_time_format_hhmm(hour, minute, reminder->time_local,
                                    sizeof(reminder->time_local))
            == 0) {
            return LANLAN_SYNC_PARSE_INVALID_VALUE;
        }
    }
    reminder->last_rung_day = LANLAN_REMINDER_NO_RUNG;
    return LANLAN_SYNC_PARSE_OK;
}

/* ------------------------------------------------------------------ arrays -- */

static lanlan_sync_parse_status_t parse_record_array(lanlan_json_t *json,
                                                     const lanlan_json_value_t *array,
                                                     int16_t offset_minutes,
                                                     const lanlan_sync_parse_hooks_t *hooks,
                                                     lanlan_sync_batch_t *batch) {
    if (array->type != LANLAN_JSON_TYPE_ARRAY) return LANLAN_SYNC_PARSE_WRONG_TYPE;
    lanlan_json_array_iter_t iter;
    lanlan_sync_parse_status_t status = map_json(lanlan_json_array_begin(json, array, &iter));
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    lanlan_json_value_t element;
    int32_t count = 0;
    for (;;) {
        lanlan_json_status_t jstatus = lanlan_json_array_next(json, &iter, &element);
        if (jstatus == LANLAN_JSON_ERR_NOT_FOUND) break;
        if (jstatus != LANLAN_JSON_OK) return map_json(jstatus);
        if (count >= (int32_t)LANLAN_CACHE_MAX_RECORDS) continue; /* bounded, extras dropped */
        status = parse_record(json, &element, offset_minutes, hooks, &batch->records[count]);
        if (status != LANLAN_SYNC_PARSE_OK) return status;
        ++count;
    }
    batch->record_count = count;
    return LANLAN_SYNC_PARSE_OK;
}

static lanlan_sync_parse_status_t parse_reminder_array(lanlan_json_t *json,
                                                       const lanlan_json_value_t *array,
                                                       lanlan_sync_batch_t *batch) {
    if (array->type != LANLAN_JSON_TYPE_ARRAY) return LANLAN_SYNC_PARSE_WRONG_TYPE;
    lanlan_json_array_iter_t iter;
    lanlan_sync_parse_status_t status = map_json(lanlan_json_array_begin(json, array, &iter));
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    lanlan_json_value_t element;
    int32_t count = 0;
    for (;;) {
        lanlan_json_status_t jstatus = lanlan_json_array_next(json, &iter, &element);
        if (jstatus == LANLAN_JSON_ERR_NOT_FOUND) break;
        if (jstatus != LANLAN_JSON_OK) return map_json(jstatus);
        if (count >= (int32_t)LANLAN_CACHE_MAX_REMINDERS) continue;
        status = parse_reminder(json, &element, &batch->reminders[count]);
        if (status != LANLAN_SYNC_PARSE_OK) return status;
        ++count;
    }
    batch->reminder_count = count;
    return LANLAN_SYNC_PARSE_OK;
}

static lanlan_sync_parse_status_t parse_revoked_array(lanlan_json_t *json,
                                                      const lanlan_json_value_t *array,
                                                      lanlan_sync_batch_t *batch) {
    if (array->type != LANLAN_JSON_TYPE_ARRAY) return LANLAN_SYNC_PARSE_WRONG_TYPE;
    lanlan_json_array_iter_t iter;
    lanlan_sync_parse_status_t status = map_json(lanlan_json_array_begin(json, array, &iter));
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    lanlan_json_value_t element;
    int32_t count = 0;
    for (;;) {
        lanlan_json_status_t jstatus = lanlan_json_array_next(json, &iter, &element);
        if (jstatus == LANLAN_JSON_ERR_NOT_FOUND) break;
        if (jstatus != LANLAN_JSON_OK) return map_json(jstatus);
        if (element.type != LANLAN_JSON_TYPE_STRING) return LANLAN_SYNC_PARSE_WRONG_TYPE;
        if (count >= (int32_t)LANLAN_CACHE_MAX_RECORDS) continue;
        char text[PARSE_FIELD_MAX];
        status = map_json(lanlan_json_get_string(json, &element, text, sizeof(text), NULL));
        if (status != LANLAN_SYNC_PARSE_OK) return status;
        if (!parse_uuid(text, batch->revoked[count])) return LANLAN_SYNC_PARSE_INVALID_VALUE;
        ++count;
    }
    batch->revoked_count = count;
    return LANLAN_SYNC_PARSE_OK;
}

/* Applies the optional caregiver directory before any record is mapped. A
 * malformed list is "no update", never a failure. */
static lanlan_sync_parse_status_t apply_members(lanlan_json_t *json,
                                                const lanlan_json_value_t *root,
                                                const lanlan_sync_parse_hooks_t *hooks) {
#define PARSE_MEMBER_LIMIT (LANLAN_CAREGIVER_SLOTS * 2)
    if (!hooks || !hooks->members) return LANLAN_SYNC_PARSE_OK;
    lanlan_json_value_t members;
    member_kind_t kind = MEMBER_ABSENT;
    lanlan_sync_parse_status_t status = member_get(json, root, "members", &members, &kind);
    if (status != LANLAN_SYNC_PARSE_OK) return status;
    if (kind != MEMBER_CONTAINER || members.type != LANLAN_JSON_TYPE_ARRAY) {
        /* Absent, null, scalar or an object: "no update", never a failure. */
        return LANLAN_SYNC_PARSE_OK;
    }
    lanlan_json_array_iter_t iter;
    if (lanlan_json_array_begin(json, &members, &iter) != LANLAN_JSON_OK) {
        return LANLAN_SYNC_PARSE_OK;
    }
    /* The hook borrows these strings for the duration of the call, so every
     * entry needs its own storage rather than a reused loop-local buffer. */
    char ids[PARSE_MEMBER_LIMIT][PARSE_TEXT_MAX];
    char names[PARSE_MEMBER_LIMIT][PARSE_TEXT_MAX];
    lanlan_caregiver_member_t list[PARSE_MEMBER_LIMIT];
    int count = 0;
    lanlan_json_value_t element;
    while (count < (int)PARSE_MEMBER_LIMIT) {
        lanlan_json_status_t jstatus = lanlan_json_array_next(json, &iter, &element);
        if (jstatus == LANLAN_JSON_ERR_NOT_FOUND) break;
        if (jstatus != LANLAN_JSON_OK) return LANLAN_SYNC_PARSE_OK;
        if (element.type != LANLAN_JSON_TYPE_OBJECT) continue;
        bool id_present = false;
        bool name_present = false;
        if (member_text(json, &element, "id", ids[count], sizeof(ids[count]), &id_present)
            != LANLAN_SYNC_PARSE_OK) {
            return LANLAN_SYNC_PARSE_OK; /* malformed list: no update */
        }
        if (member_text(json, &element, "display_name", names[count], sizeof(names[count]),
                        &name_present)
            != LANLAN_SYNC_PARSE_OK) {
            return LANLAN_SYNC_PARSE_OK;
        }
        if (!id_present || ids[count][0] == '\0') continue;
        list[count].id = ids[count];
        list[count].display_name = name_present ? names[count] : NULL;
        ++count;
    }
    if (count > 0) hooks->members(list, count, hooks->user);
    return LANLAN_SYNC_PARSE_OK;
#undef PARSE_MEMBER_LIMIT
}

/* --------------------------------------------------------------- page entry -- */

lanlan_sync_parse_status_t lanlan_sync_parse_page(lanlan_sync_page_t page, const char *body,
                                                  size_t length, int16_t utc_offset_fallback,
                                                  const lanlan_sync_parse_hooks_t *hooks,
                                                  lanlan_sync_batch_t *batch,
                                                  lanlan_sync_page_info_t *info) {
    if (!batch) return LANLAN_SYNC_PARSE_MALFORMED;
    /* The contract is that a failed page leaves the batch cleared, so every
     * failure path funnels through one exit that zeroes it again. */
    memset(batch, 0, sizeof(*batch));
    if (info) memset(info, 0, sizeof(*info));
    batch->utc_offset_minutes = utc_offset_fallback;

    lanlan_sync_parse_status_t result = LANLAN_SYNC_PARSE_OK;
    lanlan_json_t json;
    lanlan_json_value_t root;
    lanlan_json_value_t value;
    member_kind_t kind = MEMBER_ABSENT;
    char text[PARSE_TEXT_MAX];
    bool present = false;
    double number = 0;
    int64_t cursor = 0;

    do {
        if (!body || length == 0) {
            result = LANLAN_SYNC_PARSE_MALFORMED;
            break;
        }
        lanlan_json_init(&json, body, length);
        lanlan_json_status_t jstatus = lanlan_json_root(&json, &root);
        if (jstatus != LANLAN_JSON_OK) {
            result = map_json(jstatus);
            break;
        }
        if (root.type != LANLAN_JSON_TYPE_OBJECT) {
            result = LANLAN_SYNC_PARSE_WRONG_TYPE;
            break;
        }

        /* cursor is mandatory on both endpoints. */
        result = member_get(&json, &root, "cursor", &value, &kind);
        if (result != LANLAN_SYNC_PARSE_OK) break;
        if (kind == MEMBER_ABSENT || kind == MEMBER_NULL) {
            result = LANLAN_SYNC_PARSE_MISSING_FIELD;
            break;
        }
        if (kind != MEMBER_NUMBER) {
            result = LANLAN_SYNC_PARSE_WRONG_TYPE;
            break;
        }
        result = map_json(lanlan_json_get_i64(&json, &value, &cursor));
        if (result != LANLAN_SYNC_PARSE_OK) break;
        if (cursor < 0 || cursor > (int64_t)UINT32_MAX) {
            result = LANLAN_SYNC_PARSE_INVALID_VALUE;
            break;
        }
        batch->cursor = (uint32_t)cursor;

        /* utc_offset_minutes must be known before the records are converted: it
         * is the display offset every record on this page uses. */
        result = member_number(&json, &root, "utc_offset_minutes", &number, &present);
        if (result != LANLAN_SYNC_PARSE_OK) break;
        if (present && number >= LANLAN_TIME_OFFSET_MIN && number <= LANLAN_TIME_OFFSET_MAX) {
            batch->utc_offset_minutes = (int16_t)number;
        }

        result = member_text(&json, &root, "server_time", text, sizeof(text), &present);
        if (result != LANLAN_SYNC_PARSE_OK) break;
        if (present) {
            int64_t epoch = 0;
            int16_t parsed_offset = 0;
            if (lanlan_time_parse_rfc3339(text, &epoch, &parsed_offset) == LANLAN_TIME_OK) {
                batch->server_epoch = epoch;
                batch->has_server_time = true;
            }
        }

        /* The caregiver directory must be current before a record is mapped to a
         * slot, so it is applied here, ahead of the records array. */
        result = apply_members(&json, &root, hooks);
        if (result != LANLAN_SYNC_PARSE_OK) break;

        result = member_get(&json, &root, "records", &value, &kind);
        if (result != LANLAN_SYNC_PARSE_OK) break;
        if (kind == MEMBER_ABSENT || kind == MEMBER_NULL) {
            result = LANLAN_SYNC_PARSE_MISSING_FIELD;
            break;
        }
        if (kind != MEMBER_CONTAINER || value.type != LANLAN_JSON_TYPE_ARRAY) {
            result = LANLAN_SYNC_PARSE_WRONG_TYPE;
            break;
        }
        result = parse_record_array(&json, &value, batch->utc_offset_minutes, hooks, batch);
        if (result != LANLAN_SYNC_PARSE_OK) break;

        result = member_get(&json, &root, "reminders", &value, &kind);
        if (result != LANLAN_SYNC_PARSE_OK) break;
        if (kind == MEMBER_CONTAINER && value.type == LANLAN_JSON_TYPE_ARRAY) {
            result = parse_reminder_array(&json, &value, batch);
            if (result != LANLAN_SYNC_PARSE_OK) break;
        }

        result = member_get(&json, &root, "revoked", &value, &kind);
        if (result != LANLAN_SYNC_PARSE_OK) break;
        if (kind == MEMBER_CONTAINER && value.type == LANLAN_JSON_TYPE_ARRAY) {
            result = parse_revoked_array(&json, &value, batch);
            if (result != LANLAN_SYNC_PARSE_OK) break;
        }

        if (info) {
            /* has_more is sent by both endpoints and drives paging. It is a real
             * boolean in the payload; a numeric 1/0 is accepted as well. */
            result = member_get(&json, &root, "has_more", &value, &kind);
            if (result != LANLAN_SYNC_PARSE_OK) break;
            if (kind == MEMBER_BOOL) {
                bool truth = false;
                (void)lanlan_json_get_bool(&json, &value, &truth);
                info->has_has_more = true;
                info->has_more = truth;
            } else if (kind == MEMBER_NUMBER) {
                double flag = 0;
                if (lanlan_json_get_double(&json, &value, &flag) == LANLAN_JSON_OK) {
                    info->has_has_more = true;
                    info->has_more = flag != 0;
                }
            }
            if (page == LANLAN_SYNC_PAGE_SNAPSHOT) {
                result = member_number(&json, &root, "offset", &number, &present);
                if (result != LANLAN_SYNC_PARSE_OK) break;
                info->has_offset = present;
                info->offset = present ? (int64_t)number : 0;
                result = member_number(&json, &root, "limit", &number, &present);
                if (result != LANLAN_SYNC_PARSE_OK) break;
                info->has_limit = present;
                info->limit = present ? (int64_t)number : 0;
                result = member_number(&json, &root, "total", &number, &present);
                if (result != LANLAN_SYNC_PARSE_OK) break;
                info->has_total = present;
                info->total = present ? (int64_t)number : 0;
            }
        }
    } while (false);

    if (result != LANLAN_SYNC_PARSE_OK) {
        memset(batch, 0, sizeof(*batch));
        if (info) memset(info, 0, sizeof(*info));
    }
    return result;
}

/* -------------------------------------------------------------- HTTP status -- */

lanlan_sync_http_t lanlan_sync_http_classify(int status, const char *error_code) {
    if (status >= 200 && status <= 299) return LANLAN_SYNC_HTTP_OK;
    if (status == 401) return LANLAN_SYNC_HTTP_REJECTED;
    if (status == 409) {
        if (error_code && strcmp(error_code, "cursor_invalid") == 0) {
            return LANLAN_SYNC_HTTP_RESYNC;
        }
        return LANLAN_SYNC_HTTP_CLIENT_ERROR;
    }
    if (status >= 400 && status <= 499) return LANLAN_SYNC_HTTP_CLIENT_ERROR;
    if (status >= 500 && status <= 599) return LANLAN_SYNC_HTTP_RETRY;
    return LANLAN_SYNC_HTTP_FATAL;
}

const char *lanlan_sync_http_name(lanlan_sync_http_t result) {
    switch (result) {
    case LANLAN_SYNC_HTTP_OK: return "ok";
    case LANLAN_SYNC_HTTP_REJECTED: return "credential_rejected";
    case LANLAN_SYNC_HTTP_RESYNC: return "resync";
    case LANLAN_SYNC_HTTP_CLIENT_ERROR: return "client_error";
    case LANLAN_SYNC_HTTP_RETRY: return "retry";
    case LANLAN_SYNC_HTTP_FATAL: return "fatal";
    default: return "unknown";
    }
}

bool lanlan_sync_parse_error_code(const char *body, size_t length, char *out, size_t out_size) {
    if (out && out_size > 0) out[0] = '\0';
    if (!body || length == 0 || !out || out_size == 0) return false;
    lanlan_json_t json;
    lanlan_json_init(&json, body, length);
    lanlan_json_value_t root;
    if (lanlan_json_root(&json, &root) != LANLAN_JSON_OK) return false;
    if (root.type != LANLAN_JSON_TYPE_OBJECT) return false;
    lanlan_json_value_t error;
    if (lanlan_json_object_get(&json, &root, "error", &error) != LANLAN_JSON_OK) return false;
    if (error.type != LANLAN_JSON_TYPE_OBJECT) return false;
    lanlan_json_value_t code;
    if (lanlan_json_object_get(&json, &error, "code", &code) != LANLAN_JSON_OK) return false;
    if (code.type != LANLAN_JSON_TYPE_STRING) return false;
    if (lanlan_json_get_string(&json, &code, out, out_size, NULL) != LANLAN_JSON_OK) return false;
    return out[0] != '\0';
}

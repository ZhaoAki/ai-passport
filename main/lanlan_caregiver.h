/* Cyber Lanlan caregiver directory: the family members the service reports in
 * the `members` array of every sync response, keyed by service user id.
 *
 * Why this exists: a cached record stores the caregiver as a 0/1 index
 * (lanlan_record_t.created_by / performed_by), while the service identifies a
 * caregiver by an opaque user id. The compact record carries the id, so the
 * device needs a stable id -> slot directory. Slots are learned **by id**, so a
 * batch that happens to list the other caregiver first cannot move an id that is
 * already known; a fresh device seeds the slots from the service's own member
 * order.
 *
 * Pure logic: no ESP-IDF, no LVGL, no dynamic allocation, no libc time. Host
 * tests live in tests/test_lanlan_caregiver.c. The only application dependency
 * is the generated string table, and only for its compile-time literal: the
 * neutral label is never invented here and never empty. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The first generation ships exactly two caregivers. */
#define LANLAN_CAREGIVER_SLOTS 2
/* Service user ids are UUID-ish tokens; the field is deliberately generous
 * (>= 40 bytes) and keeps room for the terminator. */
#define LANLAN_CAREGIVER_ID_BYTES 48
/* Display names are UTF-8 and at most 24 bytes; the field adds the terminator. */
#define LANLAN_CAREGIVER_NAME_MAX_BYTES 24
#define LANLAN_CAREGIVER_NAME_BYTES (LANLAN_CAREGIVER_NAME_MAX_BYTES + 1)

/* Versioned, fixed-size, CRC-checked NVS payload for the `care_v1` key. A
 * constant length means the value never changes size while the version stays. */
#define LANLAN_CAREGIVER_BLOB_VERSION 1u
#define LANLAN_CAREGIVER_BLOB_BYTES 154u

typedef enum {
    LANLAN_CAREGIVER_OK = 0,
    LANLAN_CAREGIVER_CORRUPT,          /* bad magic, bad CRC, or an invalid field */
    LANLAN_CAREGIVER_VERSION_MISMATCH, /* readable header, unsupported version */
    LANLAN_CAREGIVER_TRUNCATED         /* shorter than the canonical payload */
} lanlan_caregiver_status_t;

const char *lanlan_caregiver_status_name(lanlan_caregiver_status_t status);

/* One entry of the service's `members` array. Both strings are borrowed for the
 * duration of the call; `display_name` may be NULL. */
typedef struct {
    const char *id;
    const char *display_name;
} lanlan_caregiver_member_t;

typedef struct {
    uint8_t used_mask; /* bit i set: slot i holds the id in id[i] */
    char id[LANLAN_CAREGIVER_SLOTS][LANLAN_CAREGIVER_ID_BYTES];
    char display_name[LANLAN_CAREGIVER_SLOTS][LANLAN_CAREGIVER_NAME_BYTES];
} lanlan_caregiver_table_t;

void lanlan_caregiver_clear(lanlan_caregiver_table_t *table);

/* ------------------------------------------------------------- text rules -- */

/* True when `input` is a usable display name: non-empty, well-formed UTF-8, and
 * free of C0/C1 control characters. Length alone never makes a name invalid;
 * over-long names are truncated instead. */
bool lanlan_caregiver_name_is_valid(const char *input);
/* Validates and copies a display name, cutting only at a UTF-8 boundary and
 * never past LANLAN_CAREGIVER_NAME_MAX_BYTES. A rejected name yields "" and a
 * return of 0, so the caller keeps the id and renders the neutral label. */
size_t lanlan_caregiver_sanitize_name(const char *input, char *out, size_t out_size);

/* --------------------------------------------------------------- lookup -- */

/* Learns one caregiver by id and returns its slot index.
 *   - known id: the slot is unchanged and a usable display name updates it;
 *   - new id:   the lowest free slot is taken;
 *   - both slots taken by other ids: returns -1 and changes nothing.
 * A NULL/empty/invalid id returns -1. A rejected display name leaves the slot's
 * name cleared, which renders as the neutral label. */
int lanlan_caregiver_learn(lanlan_caregiver_table_t *table, const char *id,
                           const char *display_name);
/* Applies one `members` array in service order. A missing list (NULL or
 * count <= 0) is "no update" and changes nothing; entries without a usable id
 * are skipped. */
void lanlan_caregiver_apply_members(lanlan_caregiver_table_t *table,
                                    const lanlan_caregiver_member_t *members, int count);
/* Slot index for an id, or -1 when the id is unknown. */
int lanlan_caregiver_index_for_id(const lanlan_caregiver_table_t *table, const char *id);

/* ---------------------------------------------------------------- display -- */

/* The neutral label, from the generated string table. Never empty. */
const char *lanlan_caregiver_fallback_label(void);
/* Display name for a slot: the learned name, or the neutral label when the slot
 * is unused or its name was rejected. Never returns NULL, "" or a raw id. */
const char *lanlan_caregiver_name_at(const lanlan_caregiver_table_t *table, unsigned index);
/* Display name for a service id, with the same fallback. Never returns NULL,
 * "" or the id itself. */
const char *lanlan_caregiver_name_for_id(const lanlan_caregiver_table_t *table, const char *id);

/* ----------------------------------------------------------------- codec -- */

/* Serializes the directory. Returns the byte length written, or 0 when the
 * output buffer is smaller than LANLAN_CAREGIVER_BLOB_BYTES. */
size_t lanlan_caregiver_encode(const lanlan_caregiver_table_t *table, uint8_t *out,
                               size_t out_size);
/* Decodes the `care_v1` payload. A short value reports TRUNCATED, a bad magic or
 * CRC reports CORRUPT, and another version reports VERSION_MISMATCH; in every
 * failure case `table` is left cleared and the caller re-learns from the service
 * and must treat the cached records as unindexed. */
lanlan_caregiver_status_t lanlan_caregiver_decode(const uint8_t *data, size_t size,
                                                  lanlan_caregiver_table_t *table);

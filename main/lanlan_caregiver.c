/* Cyber Lanlan caregiver directory. See lanlan_caregiver.h for the contract and
 * tests/test_lanlan_caregiver.c for the covered behaviour. */

#include "lanlan_caregiver.h"

#include <string.h>

#include "lanlan_strings.h"

/* ------------------------------------------------------------- wire layout --
 * care_v1 (154 bytes, fixed):
 *   0    2   magic 'L' 'G'
 *   2    1   version
 *   3    1   used mask
 *   4   48   slot 0 id, NUL padded
 *   52  48   slot 1 id, NUL padded
 *   100 25   slot 0 display name, NUL padded
 *   125 25   slot 1 display name, NUL padded
 *   150  4   CRC-32 (IEEE 802.3, reflected) over bytes 0..149
 */
#define CG_MAGIC_0 'L'
#define CG_MAGIC_1 'G'
#define CG_VERSION_OFFSET 2u
#define CG_MASK_OFFSET 3u
#define CG_IDS_OFFSET 4u
#define CG_NAMES_OFFSET (CG_IDS_OFFSET + LANLAN_CAREGIVER_SLOTS * LANLAN_CAREGIVER_ID_BYTES)
#define CG_CRC_OFFSET (CG_NAMES_OFFSET + LANLAN_CAREGIVER_SLOTS * LANLAN_CAREGIVER_NAME_BYTES)

_Static_assert(CG_CRC_OFFSET + 4u == LANLAN_CAREGIVER_BLOB_BYTES,
               "the CRC trailer must end the caregiver payload");
_Static_assert(sizeof(lanlan_caregiver_table_t) <= LANLAN_CAREGIVER_BLOB_BYTES,
               "the in-memory table must fit its NVS payload");

/* --------------------------------------------------------------- helpers -- */

static const char *const CG_FALLBACK_LABEL = LANLAN_STR_CAREGIVERS_UNKNOWN;

const char *lanlan_caregiver_status_name(lanlan_caregiver_status_t status) {
    switch (status) {
    case LANLAN_CAREGIVER_OK: return "ok";
    case LANLAN_CAREGIVER_CORRUPT: return "corrupt";
    case LANLAN_CAREGIVER_VERSION_MISMATCH: return "version_mismatch";
    case LANLAN_CAREGIVER_TRUNCATED: return "truncated";
    default: return "unknown";
    }
}

void lanlan_caregiver_clear(lanlan_caregiver_table_t *table) {
    if (table) memset(table, 0, sizeof(*table));
}

/* ---------------------------------------------------------------- UTF-8 -- */

/* Decodes one sequence. Returns its byte length, or 0 when the input is
 * malformed (bad lead byte, truncated sequence, stray continuation byte,
 * overlong encoding, surrogate, or above U+10FFFF). */
static size_t cg_utf8_decode(const char *text, uint32_t *codepoint_out) {
    if (!text || !text[0]) return 0;
    const unsigned char *bytes = (const unsigned char *)text;
    unsigned char lead = bytes[0];
    size_t length;
    uint32_t value;
    uint32_t minimum;
    if (lead < 0x80u) {
        length = 1;
        value = lead;
        minimum = 0;
    } else if ((lead & 0xE0u) == 0xC0u) {
        length = 2;
        value = (uint32_t)(lead & 0x1Fu);
        minimum = 0x80u;
    } else if ((lead & 0xF0u) == 0xE0u) {
        length = 3;
        value = (uint32_t)(lead & 0x0Fu);
        minimum = 0x800u;
    } else if ((lead & 0xF8u) == 0xF0u) {
        length = 4;
        value = (uint32_t)(lead & 0x07u);
        minimum = 0x10000u;
    } else {
        return 0;
    }
    for (size_t i = 1; i < length; ++i) {
        if ((bytes[i] & 0xC0u) != 0x80u) return 0;
        value = (value << 6) | (uint32_t)(bytes[i] & 0x3Fu);
    }
    if (length > 1 && value < minimum) return 0;
    if (value > 0x10FFFFu) return 0;
    if (value >= 0xD800u && value <= 0xDFFFu) return 0;
    if (codepoint_out) *codepoint_out = value;
    return length;
}

static bool cg_is_control(uint32_t codepoint) {
    return codepoint < 0x20u || codepoint == 0x7Fu
           || (codepoint >= 0x80u && codepoint <= 0x9Fu);
}

bool lanlan_caregiver_name_is_valid(const char *input) {
    if (!input || input[0] == '\0') return false;
    for (const char *cursor = input; *cursor != '\0';) {
        uint32_t codepoint = 0;
        size_t length = cg_utf8_decode(cursor, &codepoint);
        if (length == 0) return false;
        if (cg_is_control(codepoint)) return false;
        cursor += length;
    }
    return true;
}

size_t lanlan_caregiver_sanitize_name(const char *input, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!lanlan_caregiver_name_is_valid(input)) return 0;
    size_t limit = out_size - 1;
    if (limit > LANLAN_CAREGIVER_NAME_MAX_BYTES) limit = LANLAN_CAREGIVER_NAME_MAX_BYTES;
    size_t written = 0;
    for (const char *cursor = input; *cursor != '\0';) {
        size_t length = cg_utf8_decode(cursor, NULL);
        if (length == 0 || written + length > limit) break;
        memcpy(out + written, cursor, length);
        written += length;
        cursor += length;
    }
    out[written] = '\0';
    return written;
}

/* An id is an opaque service token: non-empty, no control bytes, and short
 * enough for the field. It is never displayed. */
static bool cg_id_is_usable(const char *id) {
    if (!id || id[0] == '\0') return false;
    size_t length = strlen(id);
    if (length >= LANLAN_CAREGIVER_ID_BYTES) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)id[i];
        if (byte < 0x21u || byte == 0x7Fu) return false;
    }
    return true;
}

static bool cg_field_is_zero(const char *field, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (field[i] != '\0') return false;
    }
    return true;
}

/* A stored field must be terminated inside its own buffer. */
static bool cg_field_terminated(const char *field, size_t size) {
    return memchr(field, '\0', size) != NULL;
}

/* -------------------------------------------------------------- learn/lookup -- */

int lanlan_caregiver_index_for_id(const lanlan_caregiver_table_t *table, const char *id) {
    if (!table || !id || id[0] == '\0') return -1;
    for (unsigned slot = 0; slot < LANLAN_CAREGIVER_SLOTS; ++slot) {
        if ((table->used_mask & (1u << slot)) == 0) continue;
        if (strcmp(table->id[slot], id) == 0) return (int)slot;
    }
    return -1;
}

int lanlan_caregiver_learn(lanlan_caregiver_table_t *table, const char *id,
                           const char *display_name) {
    if (!table || !cg_id_is_usable(id)) return -1;
    int slot = lanlan_caregiver_index_for_id(table, id);
    if (slot < 0) {
        for (unsigned candidate = 0; candidate < LANLAN_CAREGIVER_SLOTS; ++candidate) {
            if ((table->used_mask & (1u << candidate)) == 0) {
                slot = (int)candidate;
                memcpy(table->id[candidate], id, strlen(id) + 1);
                table->used_mask |= (uint8_t)(1u << candidate);
                break;
            }
        }
    }
    if (slot < 0) return -1;
    /* An empty or rejected name is stored as "unknown": the slot keeps its id
     * and the UI renders the neutral label rather than the id. */
    (void)lanlan_caregiver_sanitize_name(display_name, table->display_name[slot],
                                        sizeof(table->display_name[slot]));
    return slot;
}

void lanlan_caregiver_apply_members(lanlan_caregiver_table_t *table,
                                    const lanlan_caregiver_member_t *members, int count) {
    if (!table || !members || count <= 0) return;
    for (int i = 0; i < count; ++i) {
        if (!members[i].id) continue;
        (void)lanlan_caregiver_learn(table, members[i].id, members[i].display_name);
    }
}

/* ---------------------------------------------------------------- display -- */

const char *lanlan_caregiver_fallback_label(void) { return CG_FALLBACK_LABEL; }

const char *lanlan_caregiver_name_at(const lanlan_caregiver_table_t *table, unsigned index) {
    if (!table || index >= LANLAN_CAREGIVER_SLOTS) return CG_FALLBACK_LABEL;
    if ((table->used_mask & (1u << index)) == 0) return CG_FALLBACK_LABEL;
    if (table->display_name[index][0] == '\0') return CG_FALLBACK_LABEL;
    return table->display_name[index];
}

const char *lanlan_caregiver_name_for_id(const lanlan_caregiver_table_t *table, const char *id) {
    int slot = lanlan_caregiver_index_for_id(table, id);
    if (slot < 0) return CG_FALLBACK_LABEL;
    return lanlan_caregiver_name_at(table, (unsigned)slot);
}

/* ------------------------------------------------------------------ codec -- */

static uint32_t cg_crc32(const uint8_t *data, size_t length) {
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

static void cg_put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t cg_get_u32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16)
           | ((uint32_t)in[3] << 24);
}

size_t lanlan_caregiver_encode(const lanlan_caregiver_table_t *table, uint8_t *out,
                               size_t out_size) {
    if (!table || !out || out_size < LANLAN_CAREGIVER_BLOB_BYTES) return 0;
    memset(out, 0, LANLAN_CAREGIVER_BLOB_BYTES);
    out[0] = CG_MAGIC_0;
    out[1] = CG_MAGIC_1;
    out[CG_VERSION_OFFSET] = (uint8_t)LANLAN_CAREGIVER_BLOB_VERSION;
    out[CG_MASK_OFFSET] = (uint8_t)(table->used_mask & ((1u << LANLAN_CAREGIVER_SLOTS) - 1u));
    for (unsigned slot = 0; slot < LANLAN_CAREGIVER_SLOTS; ++slot) {
        if ((table->used_mask & (1u << slot)) == 0) continue;
        /* A field that is not terminated in memory would be truncated here; the
         * learn path always terminates, and the decode path rejects anything
         * that is not. */
        size_t id_length = strnlen(table->id[slot], LANLAN_CAREGIVER_ID_BYTES);
        if (id_length >= LANLAN_CAREGIVER_ID_BYTES) id_length = LANLAN_CAREGIVER_ID_BYTES - 1;
        memcpy(out + CG_IDS_OFFSET + slot * LANLAN_CAREGIVER_ID_BYTES, table->id[slot], id_length);
        size_t name_length = strnlen(table->display_name[slot], LANLAN_CAREGIVER_NAME_BYTES);
        if (name_length >= LANLAN_CAREGIVER_NAME_BYTES) {
            name_length = LANLAN_CAREGIVER_NAME_BYTES - 1;
        }
        memcpy(out + CG_NAMES_OFFSET + slot * LANLAN_CAREGIVER_NAME_BYTES,
               table->display_name[slot], name_length);
    }
    cg_put_u32(out + CG_CRC_OFFSET, cg_crc32(out, CG_CRC_OFFSET));
    return LANLAN_CAREGIVER_BLOB_BYTES;
}

lanlan_caregiver_status_t lanlan_caregiver_decode(const uint8_t *data, size_t size,
                                                  lanlan_caregiver_table_t *table) {
    if (!table) return LANLAN_CAREGIVER_CORRUPT;
    lanlan_caregiver_clear(table);
    if (!data || size < LANLAN_CAREGIVER_BLOB_BYTES) return LANLAN_CAREGIVER_TRUNCATED;
    if (data[0] != CG_MAGIC_0 || data[1] != CG_MAGIC_1) return LANLAN_CAREGIVER_CORRUPT;
    if (data[CG_VERSION_OFFSET] != (uint8_t)LANLAN_CAREGIVER_BLOB_VERSION) {
        return LANLAN_CAREGIVER_VERSION_MISMATCH;
    }
    if (cg_crc32(data, CG_CRC_OFFSET) != cg_get_u32(data + CG_CRC_OFFSET)) {
        return LANLAN_CAREGIVER_CORRUPT;
    }
    uint8_t mask = data[CG_MASK_OFFSET];
    if ((mask & (uint8_t)~((1u << LANLAN_CAREGIVER_SLOTS) - 1u)) != 0) {
        return LANLAN_CAREGIVER_CORRUPT;
    }
    lanlan_caregiver_table_t value;
    memset(&value, 0, sizeof(value));
    value.used_mask = mask;
    for (unsigned slot = 0; slot < LANLAN_CAREGIVER_SLOTS; ++slot) {
        const char *stored_id = (const char *)data + CG_IDS_OFFSET
                                + slot * LANLAN_CAREGIVER_ID_BYTES;
        const char *stored_name = (const char *)data + CG_NAMES_OFFSET
                                  + slot * LANLAN_CAREGIVER_NAME_BYTES;
        bool used = (mask & (1u << slot)) != 0;
        if (!used) {
            /* Unused slots must be zero: a non-zero value means the writer and
             * the reader disagree about the layout. */
            if (!cg_field_is_zero(stored_id, LANLAN_CAREGIVER_ID_BYTES)
                || !cg_field_is_zero(stored_name, LANLAN_CAREGIVER_NAME_BYTES)) {
                return LANLAN_CAREGIVER_CORRUPT;
            }
            continue;
        }
        if (!cg_field_terminated(stored_id, LANLAN_CAREGIVER_ID_BYTES)) {
            return LANLAN_CAREGIVER_CORRUPT;
        }
        if (!cg_id_is_usable(stored_id)) return LANLAN_CAREGIVER_CORRUPT;
        memcpy(value.id[slot], stored_id, strlen(stored_id) + 1);
        if (!cg_field_terminated(stored_name, LANLAN_CAREGIVER_NAME_BYTES)) {
            return LANLAN_CAREGIVER_CORRUPT;
        }
        /* An empty name is a valid "learned id, unknown name" state; a non-empty
         * one must be a well-formed name, and must survive re-encoding. */
        if (stored_name[0] != '\0') {
            if (strlen(stored_name) > LANLAN_CAREGIVER_NAME_MAX_BYTES) {
                return LANLAN_CAREGIVER_CORRUPT;
            }
            if (lanlan_caregiver_sanitize_name(stored_name, value.display_name[slot],
                                               sizeof(value.display_name[slot]))
                == 0) {
                return LANLAN_CAREGIVER_CORRUPT;
            }
        }
    }
    *table = value;
    return LANLAN_CAREGIVER_OK;
}

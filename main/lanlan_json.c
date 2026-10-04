/* Cyber Lanlan bounded JSON reader. See lanlan_json.h for the contract.
 *
 * Every byte access goes through a bounds-checked helper, and every scan carries
 * the slice end with it, so a truncated or hostile document can only produce an
 * error status. No allocation, no libc parsing (strtod is deliberately avoided so
 * the result is identical on the host and on the ESP32-C3). */

#include "lanlan_json.h"

#include <string.h>

/* --------------------------------------------------------------- helpers -- */

const char *lanlan_json_status_name(lanlan_json_status_t status) {
    switch (status) {
    case LANLAN_JSON_OK: return "ok";
    case LANLAN_JSON_ERR_TRUNCATED: return "truncated";
    case LANLAN_JSON_ERR_SYNTAX: return "syntax";
    case LANLAN_JSON_ERR_DEPTH: return "depth";
    case LANLAN_JSON_ERR_STRING: return "string";
    case LANLAN_JSON_ERR_NUMBER: return "number";
    case LANLAN_JSON_ERR_TOO_LARGE: return "too_large";
    case LANLAN_JSON_ERR_TRAILING: return "trailing";
    case LANLAN_JSON_ERR_TYPE: return "type";
    case LANLAN_JSON_ERR_NOT_FOUND: return "not_found";
    case LANLAN_JSON_ERR_INDEX: return "index";
    default: return "unknown";
    }
}

const char *lanlan_json_type_name(lanlan_json_type_t type) {
    switch (type) {
    case LANLAN_JSON_TYPE_NULL: return "null";
    case LANLAN_JSON_TYPE_BOOL: return "bool";
    case LANLAN_JSON_TYPE_NUMBER: return "number";
    case LANLAN_JSON_TYPE_STRING: return "string";
    case LANLAN_JSON_TYPE_ARRAY: return "array";
    case LANLAN_JSON_TYPE_OBJECT: return "object";
    default: return "unknown";
    }
}

static lanlan_json_status_t fail(lanlan_json_t *json, lanlan_json_status_t status, size_t offset) {
    if (json) {
        json->status = status;
        json->error_offset = offset;
    }
    return status;
}

static bool in_bounds(const lanlan_json_t *json, size_t offset) {
    return json && json->data && offset < json->length;
}

static unsigned char byte_at(const lanlan_json_t *json, size_t offset) {
    return (unsigned char)json->data[offset];
}

static bool is_ws(unsigned char byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

static void skip_ws(const lanlan_json_t *json, size_t *offset) {
    while (in_bounds(json, *offset) && is_ws(byte_at(json, *offset))) ++*offset;
}

/* ---------------------------------------------------------------- strings -- */

static int hex_value(unsigned char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

/* Validates one \uXXXX escape at *offset (which points at the 'u'). A high
 * surrogate must be followed by a low surrogate escape; a lone surrogate, bad
 * hex or a missing pair is rejected. On success *offset is one past the escape
 * and *codepoint_out holds the decoded code point. */
static lanlan_json_status_t scan_unicode_escape(lanlan_json_t *json, size_t *offset,
                                                uint32_t *codepoint_out) {
    const size_t start = *offset;
    if (json->length - start < 5) return fail(json, LANLAN_JSON_ERR_TRUNCATED, start);
    unsigned code_unit = 0;
    for (size_t i = 1; i <= 4; ++i) {
        int value = hex_value(byte_at(json, start + i));
        if (value < 0) return fail(json, LANLAN_JSON_ERR_STRING, start + i);
        code_unit = (code_unit << 4) | (unsigned)value;
    }
    size_t consumed = 5;
    uint32_t codepoint = code_unit;
    if (code_unit >= 0xD800u && code_unit <= 0xDBFFu) {
        if (json->length - start < 11 || byte_at(json, start + 5) != '\\'
            || byte_at(json, start + 6) != 'u') {
            return fail(json, LANLAN_JSON_ERR_STRING, start);
        }
        unsigned low = 0;
        for (size_t i = 7; i <= 10; ++i) {
            int value = hex_value(byte_at(json, start + i));
            if (value < 0) return fail(json, LANLAN_JSON_ERR_STRING, start + i);
            low = (low << 4) | (unsigned)value;
        }
        if (low < 0xDC00u || low > 0xDFFFu) return fail(json, LANLAN_JSON_ERR_STRING, start + 5);
        codepoint = 0x10000u + ((code_unit - 0xD800u) << 10) + (low - 0xDC00u);
        consumed = 11;
    } else if (code_unit >= 0xDC00u && code_unit <= 0xDFFFu) {
        /* A low surrogate with no high surrogate before it. */
        return fail(json, LANLAN_JSON_ERR_STRING, start);
    }
    *offset = start + consumed;
    if (codepoint_out) *codepoint_out = codepoint;
    return LANLAN_JSON_OK;
}

/* Scans the string token starting at the opening quote. On success *end is one
 * past the closing quote. Validates escapes and rejects raw control bytes. */
static lanlan_json_status_t scan_string(lanlan_json_t *json, size_t start, size_t *end) {
    if (!in_bounds(json, start) || byte_at(json, start) != '"') {
        return fail(json, LANLAN_JSON_ERR_SYNTAX, start);
    }
    size_t offset = start + 1;
    for (;;) {
        if (!in_bounds(json, offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, offset);
        unsigned char byte = byte_at(json, offset);
        if (byte == '"') {
            ++offset;
            break;
        }
        if (byte < 0x20u) return fail(json, LANLAN_JSON_ERR_STRING, offset);
        if (byte == '\\') {
            ++offset;
            if (!in_bounds(json, offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, offset);
            unsigned char escape = byte_at(json, offset);
            switch (escape) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                ++offset;
                break;
            case 'u': {
                lanlan_json_status_t status = scan_unicode_escape(json, &offset, NULL);
                if (status != LANLAN_JSON_OK) return status;
                break;
            }
            default: return fail(json, LANLAN_JSON_ERR_STRING, offset);
            }
        } else {
            ++offset;
        }
        if (offset - start > json->string_limit) {
            return fail(json, LANLAN_JSON_ERR_TOO_LARGE, start);
        }
    }
    if (offset - start > json->string_limit) {
        return fail(json, LANLAN_JSON_ERR_TOO_LARGE, start);
    }
    *end = offset;
    return LANLAN_JSON_OK;
}

/* ---------------------------------------------------------------- numbers -- */

/* Validates the RFC 8259 number grammar and returns one past its last byte. */
static lanlan_json_status_t scan_number(lanlan_json_t *json, size_t start, size_t *end) {
    size_t offset = start;
    if (!in_bounds(json, offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, offset);
    if (byte_at(json, offset) == '-') ++offset;
    if (!in_bounds(json, offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, offset);
    unsigned char byte = byte_at(json, offset);
    if (byte == '0') {
        ++offset;
    } else if (byte >= '1' && byte <= '9') {
        while (in_bounds(json, offset) && byte_at(json, offset) >= '0'
               && byte_at(json, offset) <= '9') {
            ++offset;
        }
    } else {
        return fail(json, LANLAN_JSON_ERR_NUMBER, offset);
    }
    if (in_bounds(json, offset) && byte_at(json, offset) == '.') {
        ++offset;
        if (!in_bounds(json, offset) || byte_at(json, offset) < '0' || byte_at(json, offset) > '9') {
            return fail(json, LANLAN_JSON_ERR_NUMBER, offset);
        }
        while (in_bounds(json, offset) && byte_at(json, offset) >= '0'
               && byte_at(json, offset) <= '9') {
            ++offset;
        }
    }
    if (in_bounds(json, offset) && (byte_at(json, offset) == 'e' || byte_at(json, offset) == 'E')) {
        ++offset;
        if (in_bounds(json, offset) && (byte_at(json, offset) == '+' || byte_at(json, offset) == '-')) {
            ++offset;
        }
        if (!in_bounds(json, offset) || byte_at(json, offset) < '0' || byte_at(json, offset) > '9') {
            return fail(json, LANLAN_JSON_ERR_NUMBER, offset);
        }
        while (in_bounds(json, offset) && byte_at(json, offset) >= '0'
               && byte_at(json, offset) <= '9') {
            ++offset;
        }
    }
    if (offset - start > json->number_limit) {
        return fail(json, LANLAN_JSON_ERR_TOO_LARGE, start);
    }
    *end = offset;
    return LANLAN_JSON_OK;
}

/* -------------------------------------------------------------- the walker -- */

/* Validates the value starting at *offset and leaves *offset one past it.
 * `depth` is the nesting level of this value; the root is 1. */
static lanlan_json_status_t skip_value(lanlan_json_t *json, size_t *offset, size_t depth) {
    if (depth > json->depth_limit) return fail(json, LANLAN_JSON_ERR_DEPTH, *offset);
    skip_ws(json, offset);
    if (!in_bounds(json, *offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, *offset);
    unsigned char byte = byte_at(json, *offset);
    if (byte == '{') {
        ++*offset;
        skip_ws(json, offset);
        if (!in_bounds(json, *offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, *offset);
        if (byte_at(json, *offset) == '}') {
            ++*offset;
            return LANLAN_JSON_OK;
        }
        for (;;) {
            skip_ws(json, offset);
            if (!in_bounds(json, *offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, *offset);
            size_t key_end = 0;
            lanlan_json_status_t status = scan_string(json, *offset, &key_end);
            if (status != LANLAN_JSON_OK) return status;
            *offset = key_end;
            skip_ws(json, offset);
            if (!in_bounds(json, *offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, *offset);
            if (byte_at(json, *offset) != ':') {
                return fail(json, LANLAN_JSON_ERR_SYNTAX, *offset);
            }
            ++*offset;
            status = skip_value(json, offset, depth + 1);
            if (status != LANLAN_JSON_OK) return status;
            skip_ws(json, offset);
            if (!in_bounds(json, *offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, *offset);
            if (byte_at(json, *offset) == ',') {
                ++*offset;
                continue;
            }
            if (byte_at(json, *offset) == '}') {
                ++*offset;
                return LANLAN_JSON_OK;
            }
            return fail(json, LANLAN_JSON_ERR_SYNTAX, *offset);
        }
    }
    if (byte == '[') {
        ++*offset;
        skip_ws(json, offset);
        if (in_bounds(json, *offset) && byte_at(json, *offset) == ']') {
            ++*offset;
            return LANLAN_JSON_OK;
        }
        for (;;) {
            lanlan_json_status_t status = skip_value(json, offset, depth + 1);
            if (status != LANLAN_JSON_OK) return status;
            skip_ws(json, offset);
            if (!in_bounds(json, *offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, *offset);
            if (byte_at(json, *offset) == ',') {
                ++*offset;
                continue;
            }
            if (byte_at(json, *offset) == ']') {
                ++*offset;
                return LANLAN_JSON_OK;
            }
            return fail(json, LANLAN_JSON_ERR_SYNTAX, *offset);
        }
    }
    if (byte == '"') {
        size_t end = 0;
        lanlan_json_status_t status = scan_string(json, *offset, &end);
        if (status != LANLAN_JSON_OK) return status;
        *offset = end;
        return LANLAN_JSON_OK;
    }
    if (byte == 't') {
        if (json->length - *offset < 4 || memcmp(json->data + *offset, "true", 4) != 0) {
            return fail(json, LANLAN_JSON_ERR_SYNTAX, *offset);
        }
        *offset += 4;
        return LANLAN_JSON_OK;
    }
    if (byte == 'f') {
        if (json->length - *offset < 5 || memcmp(json->data + *offset, "false", 5) != 0) {
            return fail(json, LANLAN_JSON_ERR_SYNTAX, *offset);
        }
        *offset += 5;
        return LANLAN_JSON_OK;
    }
    if (byte == 'n') {
        if (json->length - *offset < 4 || memcmp(json->data + *offset, "null", 4) != 0) {
            return fail(json, LANLAN_JSON_ERR_SYNTAX, *offset);
        }
        *offset += 4;
        return LANLAN_JSON_OK;
    }
    if (byte == '-' || (byte >= '0' && byte <= '9')) {
        size_t end = 0;
        lanlan_json_status_t status = scan_number(json, *offset, &end);
        if (status != LANLAN_JSON_OK) return status;
        *offset = end;
        return LANLAN_JSON_OK;
    }
    return fail(json, LANLAN_JSON_ERR_SYNTAX, *offset);
}

/* --------------------------------------------------------------- lifecycle -- */

void lanlan_json_init(lanlan_json_t *json, const char *data, size_t length) {
    if (!json) return;
    json->data = data;
    json->length = (data && length > 0) ? length : 0;
    json->depth_limit = LANLAN_JSON_MAX_DEPTH;
    json->string_limit = LANLAN_JSON_MAX_STRING_BYTES;
    json->number_limit = LANLAN_JSON_MAX_NUMBER_BYTES;
    json->status = LANLAN_JSON_OK;
    json->error_offset = 0;
}

size_t lanlan_json_error_offset(const lanlan_json_t *json) {
    return json ? json->error_offset : 0;
}

lanlan_json_status_t lanlan_json_last_status(const lanlan_json_t *json) {
    return json ? json->status : LANLAN_JSON_ERR_SYNTAX;
}

lanlan_json_status_t lanlan_json_root(lanlan_json_t *json, lanlan_json_value_t *value) {
    if (!json || !value) return LANLAN_JSON_ERR_SYNTAX;
    memset(value, 0, sizeof(*value));
    json->status = LANLAN_JSON_OK;
    json->error_offset = 0;
    if (!json->data || json->length == 0) return fail(json, LANLAN_JSON_ERR_TRUNCATED, 0);
    size_t start = 0;
    skip_ws(json, &start);
    size_t offset = start;
    lanlan_json_status_t status = skip_value(json, &offset, 1);
    if (status != LANLAN_JSON_OK) return status;
    size_t cursor = offset;
    skip_ws(json, &cursor);
    if (cursor != json->length) return fail(json, LANLAN_JSON_ERR_TRAILING, cursor);
    value->start = start;
    value->end = offset;
    /* The type of the root is the byte it starts with; skip_value already proved
     * the document is well formed. */
    unsigned char byte = byte_at(json, start);
    switch (byte) {
    case '{': value->type = LANLAN_JSON_TYPE_OBJECT; break;
    case '[': value->type = LANLAN_JSON_TYPE_ARRAY; break;
    case '"': value->type = LANLAN_JSON_TYPE_STRING; break;
    case 't':
    case 'f': value->type = LANLAN_JSON_TYPE_BOOL; break;
    case 'n': value->type = LANLAN_JSON_TYPE_NULL; break;
    default: value->type = LANLAN_JSON_TYPE_NUMBER; break;
    }
    return LANLAN_JSON_OK;
}

static bool slice_is_usable(const lanlan_json_t *json, const lanlan_json_value_t *value) {
    return json && value && json->data && value->start < value->end && value->end <= json->length;
}

/* ----------------------------------------------------------------- objects -- */

/* Decodes the key token at *offset (an opening quote) into `out` and advances
 * *offset past the closing quote. */
static lanlan_json_status_t read_key(lanlan_json_t *json, size_t *offset, char *out,
                                     size_t out_size) {
    size_t token_start = *offset;
    size_t token_end = 0;
    lanlan_json_status_t status = scan_string(json, token_start, &token_end);
    if (status != LANLAN_JSON_OK) return status;
    lanlan_json_value_t key = {.type = LANLAN_JSON_TYPE_STRING,
                               .start = token_start,
                               .end = token_end};
    status = lanlan_json_get_string(json, &key, out, out_size, NULL);
    if (status != LANLAN_JSON_OK) return status;
    *offset = token_end;
    return LANLAN_JSON_OK;
}

lanlan_json_status_t lanlan_json_object_get(lanlan_json_t *json,
                                            const lanlan_json_value_t *object, const char *name,
                                            lanlan_json_value_t *out) {
    if (!json || !out || !name) return LANLAN_JSON_ERR_SYNTAX;
    memset(out, 0, sizeof(*out));
    if (!slice_is_usable(json, object) || object->type != LANLAN_JSON_TYPE_OBJECT) {
        return fail(json, LANLAN_JSON_ERR_TYPE, object ? object->start : 0);
    }
    size_t offset = object->start;
    if (byte_at(json, offset) != '{') return fail(json, LANLAN_JSON_ERR_TYPE, offset);
    ++offset;
    bool found = false;
    lanlan_json_value_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    skip_ws(json, &offset);
    if (in_bounds(json, offset) && byte_at(json, offset) == '}') {
        return fail(json, LANLAN_JSON_ERR_NOT_FOUND, offset);
    }
    for (;;) {
        skip_ws(json, &offset);
        char key[64];
        lanlan_json_status_t status = read_key(json, &offset, key, sizeof(key));
        if (status != LANLAN_JSON_OK) return status;
        skip_ws(json, &offset);
        if (!in_bounds(json, offset) || byte_at(json, offset) != ':') {
            return fail(json, LANLAN_JSON_ERR_SYNTAX, offset);
        }
        ++offset;
        skip_ws(json, &offset);
        size_t value_end = offset;
        status = skip_value(json, &value_end, 2);
        if (status != LANLAN_JSON_OK) return status;
        if (strcmp(key, name) == 0) {
            /* Keep scanning so the LAST duplicate wins, as documented. */
            candidate.start = offset;
            candidate.end = value_end;
            found = true;
        }
        offset = value_end;
        skip_ws(json, &offset);
        if (!in_bounds(json, offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, offset);
        if (byte_at(json, offset) == ',') {
            ++offset;
            continue;
        }
        if (byte_at(json, offset) == '}') break;
        return fail(json, LANLAN_JSON_ERR_SYNTAX, offset);
    }
    if (!found) return fail(json, LANLAN_JSON_ERR_NOT_FOUND, object->start);
    /* Re-derive the type of the member we kept. */
    size_t probe = candidate.start;
    skip_ws(json, &probe);
    unsigned char byte = byte_at(json, probe);
    switch (byte) {
    case '{': candidate.type = LANLAN_JSON_TYPE_OBJECT; break;
    case '[': candidate.type = LANLAN_JSON_TYPE_ARRAY; break;
    case '"': candidate.type = LANLAN_JSON_TYPE_STRING; break;
    case 't':
    case 'f': candidate.type = LANLAN_JSON_TYPE_BOOL; break;
    case 'n': candidate.type = LANLAN_JSON_TYPE_NULL; break;
    default: candidate.type = LANLAN_JSON_TYPE_NUMBER; break;
    }
    *out = candidate;
    return LANLAN_JSON_OK;
}

/* ------------------------------------------------------------------ arrays -- */

lanlan_json_status_t lanlan_json_array_begin(lanlan_json_t *json,
                                             const lanlan_json_value_t *array,
                                             lanlan_json_array_iter_t *iter) {
    if (!json || !iter) return LANLAN_JSON_ERR_SYNTAX;
    memset(iter, 0, sizeof(*iter));
    if (!slice_is_usable(json, array) || array->type != LANLAN_JSON_TYPE_ARRAY) {
        return fail(json, LANLAN_JSON_ERR_TYPE, array ? array->start : 0);
    }
    if (byte_at(json, array->start) != '[') {
        return fail(json, LANLAN_JSON_ERR_TYPE, array->start);
    }
    iter->next = array->start + 1;
    iter->end = array->end - 1; /* the closing bracket */
    iter->index = 0;
    iter->done = false;
    return LANLAN_JSON_OK;
}

lanlan_json_status_t lanlan_json_array_next(lanlan_json_t *json, lanlan_json_array_iter_t *iter,
                                            lanlan_json_value_t *out) {
    if (!json || !iter || !out) return LANLAN_JSON_ERR_SYNTAX;
    memset(out, 0, sizeof(*out));
    if (iter->done) return LANLAN_JSON_ERR_NOT_FOUND;
    size_t offset = iter->next;
    skip_ws(json, &offset);
    if (!in_bounds(json, offset)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, offset);
    if (offset >= iter->end && byte_at(json, offset) == ']') {
        iter->done = true;
        return LANLAN_JSON_ERR_NOT_FOUND;
    }
    size_t value_end = offset;
    lanlan_json_status_t status = skip_value(json, &value_end, 2);
    if (status != LANLAN_JSON_OK) return status;
    unsigned char byte = byte_at(json, offset);
    switch (byte) {
    case '{': out->type = LANLAN_JSON_TYPE_OBJECT; break;
    case '[': out->type = LANLAN_JSON_TYPE_ARRAY; break;
    case '"': out->type = LANLAN_JSON_TYPE_STRING; break;
    case 't':
    case 'f': out->type = LANLAN_JSON_TYPE_BOOL; break;
    case 'n': out->type = LANLAN_JSON_TYPE_NULL; break;
    default: out->type = LANLAN_JSON_TYPE_NUMBER; break;
    }
    out->start = offset;
    out->end = value_end;
    skip_ws(json, &value_end);
    if (!in_bounds(json, value_end)) return fail(json, LANLAN_JSON_ERR_TRUNCATED, value_end);
    if (byte_at(json, value_end) == ',') {
        iter->next = value_end + 1;
    } else if (byte_at(json, value_end) == ']') {
        iter->next = value_end;
        iter->done = false; /* one more call reports the end */
    } else {
        return fail(json, LANLAN_JSON_ERR_SYNTAX, value_end);
    }
    ++iter->index;
    return LANLAN_JSON_OK;
}

size_t lanlan_json_array_size(lanlan_json_t *json, const lanlan_json_value_t *array) {
    lanlan_json_array_iter_t iter;
    if (lanlan_json_array_begin(json, array, &iter) != LANLAN_JSON_OK) return 0;
    lanlan_json_value_t element;
    size_t count = 0;
    while (lanlan_json_array_next(json, &iter, &element) == LANLAN_JSON_OK) ++count;
    return count;
}

lanlan_json_status_t lanlan_json_array_at(lanlan_json_t *json, const lanlan_json_value_t *array,
                                          size_t index, lanlan_json_value_t *out) {
    lanlan_json_array_iter_t iter;
    lanlan_json_status_t status = lanlan_json_array_begin(json, array, &iter);
    if (status != LANLAN_JSON_OK) return status;
    for (size_t i = 0; i <= index; ++i) {
        status = lanlan_json_array_next(json, &iter, out);
        if (status == LANLAN_JSON_ERR_NOT_FOUND) return fail(json, LANLAN_JSON_ERR_INDEX, 0);
        if (status != LANLAN_JSON_OK) return status;
    }
    return LANLAN_JSON_OK;
}

/* ----------------------------------------------------------------- scalars -- */

lanlan_json_status_t lanlan_json_get_bool(lanlan_json_t *json, const lanlan_json_value_t *value,
                                          bool *out) {
    if (!json || !out) return LANLAN_JSON_ERR_SYNTAX;
    if (!slice_is_usable(json, value) || value->type != LANLAN_JSON_TYPE_BOOL) {
        return fail(json, LANLAN_JSON_ERR_TYPE, value ? value->start : 0);
    }
    size_t offset = value->start;
    skip_ws(json, &offset);
    if (byte_at(json, offset) == 't') {
        *out = true;
        return LANLAN_JSON_OK;
    }
    *out = false;
    return LANLAN_JSON_OK;
}

/* Parses a validated number token. The exponent is clamped so a hostile token
 * cannot drive a long loop; the resulting magnitude then fails the caller's own
 * range checks. */
static double number_to_double(const char *text, size_t length) {
    size_t i = 0;
    bool negative = false;
    if (i < length && text[i] == '-') {
        negative = true;
        ++i;
    }
    double value = 0.0;
    while (i < length && text[i] >= '0' && text[i] <= '9') {
        value = value * 10.0 + (double)(text[i] - '0');
        ++i;
    }
    if (i < length && text[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < length && text[i] >= '0' && text[i] <= '9') {
            value += (double)(text[i] - '0') * scale;
            scale *= 0.1;
            ++i;
        }
    }
    int exponent = 0;
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool exponent_negative = false;
        if (i < length && (text[i] == '+' || text[i] == '-')) {
            exponent_negative = text[i] == '-';
            ++i;
        }
        while (i < length && text[i] >= '0' && text[i] <= '9') {
            if (exponent < 1000000) exponent = exponent * 10 + (text[i] - '0');
            ++i;
        }
        if (exponent_negative) exponent = -exponent;
    }
    if (exponent > 308) exponent = 308;
    if (exponent < -308) exponent = -308;
    while (exponent > 0) {
        value *= 10.0;
        --exponent;
    }
    while (exponent < 0) {
        value /= 10.0;
        ++exponent;
    }
    return negative ? -value : value;
}

lanlan_json_status_t lanlan_json_get_double(lanlan_json_t *json, const lanlan_json_value_t *value,
                                            double *out) {
    if (!json || !out) return LANLAN_JSON_ERR_SYNTAX;
    if (!slice_is_usable(json, value) || value->type != LANLAN_JSON_TYPE_NUMBER) {
        return fail(json, LANLAN_JSON_ERR_TYPE, value ? value->start : 0);
    }
    *out = number_to_double(json->data + value->start, value->end - value->start);
    return LANLAN_JSON_OK;
}

lanlan_json_status_t lanlan_json_get_i64(lanlan_json_t *json, const lanlan_json_value_t *value,
                                         int64_t *out) {
    double number = 0;
    lanlan_json_status_t status = lanlan_json_get_double(json, value, &number);
    if (status != LANLAN_JSON_OK) return status;
    /* 2^63, the first double that cannot be represented as an int64. */
    if (!(number >= -9223372036854775808.0 && number < 9223372036854775808.0)) {
        return fail(json, LANLAN_JSON_ERR_NUMBER, value->start);
    }
    int64_t truncated = (int64_t)number;
    if ((double)truncated != number) return fail(json, LANLAN_JSON_ERR_NUMBER, value->start);
    *out = truncated;
    return LANLAN_JSON_OK;
}

/* Decodes one \uXXXX escape at *offset (pointing at 'u') into `encoded`, which
 * receives up to four UTF-8 bytes. The surrogate rules live in
 * scan_unicode_escape() so validation and decoding cannot disagree. */
static lanlan_json_status_t decode_unicode_escape(lanlan_json_t *json, size_t *offset,
                                                  char *encoded, size_t *encoded_length) {
    uint32_t codepoint = 0;
    lanlan_json_status_t status = scan_unicode_escape(json, offset, &codepoint);
    if (status != LANLAN_JSON_OK) return status;
    size_t length = 0;
    if (codepoint < 0x80u) {
        encoded[length++] = (char)codepoint;
    } else if (codepoint < 0x800u) {
        encoded[length++] = (char)(0xC0u | (codepoint >> 6));
        encoded[length++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint < 0x10000u) {
        encoded[length++] = (char)(0xE0u | (codepoint >> 12));
        encoded[length++] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[length++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else {
        encoded[length++] = (char)(0xF0u | (codepoint >> 18));
        encoded[length++] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        encoded[length++] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[length++] = (char)(0x80u | (codepoint & 0x3Fu));
    }
    *encoded_length = length;
    return LANLAN_JSON_OK;
}

lanlan_json_status_t lanlan_json_get_string(lanlan_json_t *json, const lanlan_json_value_t *value,
                                            char *out, size_t out_size, size_t *decoded_length) {
    if (json && decoded_length) *decoded_length = 0;
    if (!json) return LANLAN_JSON_ERR_SYNTAX;
    if (!slice_is_usable(json, value) || value->type != LANLAN_JSON_TYPE_STRING) {
        return fail(json, LANLAN_JSON_ERR_TYPE, value ? value->start : 0);
    }
    if (byte_at(json, value->start) != '"') {
        return fail(json, LANLAN_JSON_ERR_TYPE, value->start);
    }
    if (out && out_size > 0) out[0] = '\0';
    size_t offset = value->start + 1;
    const size_t limit = value->end - 1; /* the closing quote */
    size_t written = 0;
    size_t total = 0;
    bool truncated = false;
    while (offset < limit) {
        unsigned char byte = byte_at(json, offset);
        char decoded[4];
        size_t decoded_size = 0;
        if (byte == '\\') {
            ++offset;
            if (offset >= limit) return fail(json, LANLAN_JSON_ERR_TRUNCATED, offset);
            unsigned char escape = byte_at(json, offset);
            switch (escape) {
            case '"': decoded[0] = '"'; decoded_size = 1; ++offset; break;
            case '\\': decoded[0] = '\\'; decoded_size = 1; ++offset; break;
            case '/': decoded[0] = '/'; decoded_size = 1; ++offset; break;
            case 'b': decoded[0] = '\b'; decoded_size = 1; ++offset; break;
            case 'f': decoded[0] = '\f'; decoded_size = 1; ++offset; break;
            case 'n': decoded[0] = '\n'; decoded_size = 1; ++offset; break;
            case 'r': decoded[0] = '\r'; decoded_size = 1; ++offset; break;
            case 't': decoded[0] = '\t'; decoded_size = 1; ++offset; break;
            case 'u': {
                lanlan_json_status_t status =
                    decode_unicode_escape(json, &offset, decoded, &decoded_size);
                if (status != LANLAN_JSON_OK) return status;
                break;
            }
            default: return fail(json, LANLAN_JSON_ERR_STRING, offset);
            }
        } else {
            if (byte < 0x20u) return fail(json, LANLAN_JSON_ERR_STRING, offset);
            decoded[0] = (char)byte;
            decoded_size = 1;
            ++offset;
        }
        total += decoded_size;
        if (out && out_size > 0 && !truncated) {
            if (written + decoded_size + 1 <= out_size) {
                memcpy(out + written, decoded, decoded_size);
                written += decoded_size;
            } else {
                /* Stop at the first character that does not fit: the buffer then
                 * always ends on a UTF-8 boundary. Counting continues so the
                 * caller still learns the full decoded length. */
                truncated = true;
            }
        }
    }
    if (out && out_size > 0) out[written] = '\0';
    if (decoded_length) *decoded_length = total;
    return LANLAN_JSON_OK;
}

bool lanlan_json_string_equals(lanlan_json_t *json, const lanlan_json_value_t *value,
                               const char *expected) {
    if (!json || !value || !expected) return false;
    if (value->type != LANLAN_JSON_TYPE_STRING) return false;
    /* Compare without copying: walk the token and the expected string together. */
    const size_t limit = value->end - 1;
    size_t offset = value->start + 1;
    size_t index = 0;
    while (offset < limit) {
        unsigned char byte = byte_at(json, offset);
        char decoded[4];
        size_t decoded_size = 0;
        if (byte == '\\') {
            ++offset;
            if (offset >= limit) return false;
            unsigned char escape = byte_at(json, offset);
            switch (escape) {
            case '"': decoded[0] = '"'; decoded_size = 1; ++offset; break;
            case '\\': decoded[0] = '\\'; decoded_size = 1; ++offset; break;
            case '/': decoded[0] = '/'; decoded_size = 1; ++offset; break;
            case 'b': decoded[0] = '\b'; decoded_size = 1; ++offset; break;
            case 'f': decoded[0] = '\f'; decoded_size = 1; ++offset; break;
            case 'n': decoded[0] = '\n'; decoded_size = 1; ++offset; break;
            case 'r': decoded[0] = '\r'; decoded_size = 1; ++offset; break;
            case 't': decoded[0] = '\t'; decoded_size = 1; ++offset; break;
            case 'u': {
                if (decode_unicode_escape(json, &offset, decoded, &decoded_size)
                    != LANLAN_JSON_OK) {
                    return false;
                }
                break;
            }
            default: return false;
            }
        } else {
            if (byte < 0x20u) return false;
            decoded[0] = (char)byte;
            decoded_size = 1;
            ++offset;
        }
        for (size_t i = 0; i < decoded_size; ++i) {
            if (expected[index] == '\0' || expected[index] != decoded[i]) return false;
            ++index;
        }
    }
    return expected[index] == '\0';
}

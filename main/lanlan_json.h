/* Cyber Lanlan bounded JSON reader.
 *
 * A strict, allocation-free reader for the RFC 8259 subset the sync service
 * emits. It exists so the sync response parsing can be host-tested: this module
 * has no ESP-IDF, no cJSON, no LVGL and no dynamic allocation, and it never
 * reads outside the caller's buffer.
 *
 * Model
 * -----
 * The caller owns one byte buffer. lanlan_json_root() validates the whole
 * document once (syntax, escapes, numbers, depth, token sizes, no trailing
 * garbage) and returns a value slice for the top-level value. Every accessor
 * then walks that slice on demand, re-checking each byte it touches, so a
 * hand-made or stale lanlan_json_value_t cannot cause an out-of-bounds read.
 *
 * Strings are read directly from the source buffer: no copy is made unless the
 * caller passes an output buffer to lanlan_json_get_string(), which decodes
 * escapes (including \uXXXX surrogate pairs) into it. A buffer that is too small
 * receives the longest prefix that ends on a UTF-8 character boundary and the
 * call still succeeds; the full decoded length is reported separately.
 *
 * Limits (documented, compile-time, overridable by the caller before use):
 *   - nesting depth LANLAN_JSON_MAX_DEPTH (8);
 *   - one string token at most LANLAN_JSON_MAX_STRING_BYTES raw bytes;
 *   - one number token at most LANLAN_JSON_MAX_NUMBER_BYTES raw bytes.
 * The caller is expected to cap the whole buffer as well (the sync worker caps
 * the HTTP body before this reader ever sees it). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The documents this reader serves nest four or five levels (object -> array ->
 * record object -> scalar); eight leaves room without letting a hostile body
 * drive the stack. */
#define LANLAN_JSON_MAX_DEPTH 8u
#define LANLAN_JSON_MAX_STRING_BYTES 1024u
#define LANLAN_JSON_MAX_NUMBER_BYTES 64u

typedef enum {
    LANLAN_JSON_OK = 0,
    LANLAN_JSON_ERR_TRUNCATED, /* input ended inside a value */
    LANLAN_JSON_ERR_SYNTAX,    /* unexpected byte where a token was expected */
    LANLAN_JSON_ERR_DEPTH,     /* nesting deeper than the configured limit */
    LANLAN_JSON_ERR_STRING,    /* bad escape, raw control byte, unterminated */
    LANLAN_JSON_ERR_NUMBER,    /* malformed number, or a non-integral i64 read */
    LANLAN_JSON_ERR_TOO_LARGE, /* string or number token past its byte limit */
    LANLAN_JSON_ERR_TRAILING,  /* bytes after the top-level value */
    LANLAN_JSON_ERR_TYPE,      /* the value has a different type */
    LANLAN_JSON_ERR_NOT_FOUND, /* object member absent */
    LANLAN_JSON_ERR_INDEX      /* array index past the end */
} lanlan_json_status_t;

const char *lanlan_json_status_name(lanlan_json_status_t status);

typedef enum {
    LANLAN_JSON_TYPE_NULL = 0,
    LANLAN_JSON_TYPE_BOOL,
    LANLAN_JSON_TYPE_NUMBER,
    LANLAN_JSON_TYPE_STRING,
    LANLAN_JSON_TYPE_ARRAY,
    LANLAN_JSON_TYPE_OBJECT
} lanlan_json_type_t;

const char *lanlan_json_type_name(lanlan_json_type_t type);

/* A slice of the caller's buffer. start/end are byte offsets and are always
 * inside the document; end is one past the value's last byte. */
typedef struct {
    lanlan_json_type_t type;
    size_t start;
    size_t end;
} lanlan_json_value_t;

typedef struct {
    const char *data;
    size_t length;
    size_t depth_limit;
    size_t string_limit;
    size_t number_limit;
    lanlan_json_status_t status; /* the most recent failure, for diagnostics */
    size_t error_offset;         /* byte offset of that failure */
} lanlan_json_t;

/* Binds the reader to [data, data + length) and installs the default limits.
 * A NULL buffer is treated as an empty document rather than a crash. */
void lanlan_json_init(lanlan_json_t *json, const char *data, size_t length);

/* Parses the single top-level value. Succeeds only when nothing but JSON
 * whitespace follows it. On failure the status is the reason and
 * lanlan_json_error_offset() is the offending byte. */
lanlan_json_status_t lanlan_json_root(lanlan_json_t *json, lanlan_json_value_t *value);
size_t lanlan_json_error_offset(const lanlan_json_t *json);
lanlan_json_status_t lanlan_json_last_status(const lanlan_json_t *json);

/* --------------------------------------------------------------- objects -- */

/* Looks up an object member. The key is compared after decoding, so an escaped
 * key matches its unescaped name. Duplicate keys are allowed and the LAST one
 * wins, matching the service and every mainstream JSON parser. Returns
 * LANLAN_JSON_ERR_NOT_FOUND when the member is absent and
 * LANLAN_JSON_ERR_TYPE when `object` is not an object. */
lanlan_json_status_t lanlan_json_object_get(lanlan_json_t *json,
                                            const lanlan_json_value_t *object, const char *name,
                                            lanlan_json_value_t *out);

/* ---------------------------------------------------------------- arrays -- */

/* Array walker. Begin, then call next() until it reports NOT_FOUND. */
typedef struct {
    size_t next;  /* offset of the next element, or of the closing bracket */
    size_t end;   /* one past the array */
    size_t index; /* elements already returned */
    bool done;
} lanlan_json_array_iter_t;

lanlan_json_status_t lanlan_json_array_begin(lanlan_json_t *json,
                                             const lanlan_json_value_t *array,
                                             lanlan_json_array_iter_t *iter);
/* Fills `out` with the next element, or returns LANLAN_JSON_ERR_NOT_FOUND at the
 * end of the array. */
lanlan_json_status_t lanlan_json_array_next(lanlan_json_t *json,
                                            lanlan_json_array_iter_t *iter,
                                            lanlan_json_value_t *out);
/* Number of elements, or 0 when `array` is not an array. */
size_t lanlan_json_array_size(lanlan_json_t *json, const lanlan_json_value_t *array);
/* Random access; LANLAN_JSON_ERR_INDEX past the end. */
lanlan_json_status_t lanlan_json_array_at(lanlan_json_t *json, const lanlan_json_value_t *array,
                                          size_t index, lanlan_json_value_t *out);

/* ---------------------------------------------------------------- scalars -- */

lanlan_json_status_t lanlan_json_get_bool(lanlan_json_t *json, const lanlan_json_value_t *value,
                                          bool *out);
lanlan_json_status_t lanlan_json_get_double(lanlan_json_t *json,
                                            const lanlan_json_value_t *value, double *out);
/* Accepts integral numbers with an optional zero fraction or exponent, so
 * `412` and `412.0` both read as 412; a non-integral value is
 * LANLAN_JSON_ERR_NUMBER. */
lanlan_json_status_t lanlan_json_get_i64(lanlan_json_t *json, const lanlan_json_value_t *value,
                                         int64_t *out);

/* Decodes a string value into `out`, which is always NUL-terminated when
 * out_size > 0. `decoded_length` (optional) receives the full decoded byte
 * length even when the buffer was too small. Pass out == NULL to validate the
 * string without copying it. */
lanlan_json_status_t lanlan_json_get_string(lanlan_json_t *json,
                                            const lanlan_json_value_t *value, char *out,
                                            size_t out_size, size_t *decoded_length);
/* Convenience: true when the value is a string equal to `expected`. */
bool lanlan_json_string_equals(lanlan_json_t *json, const lanlan_json_value_t *value,
                               const char *expected);

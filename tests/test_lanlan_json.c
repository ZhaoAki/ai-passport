/* Host tests for main/lanlan_json.c.
 * Build:  cc -std=c11 -Wall -Wextra -Werror -Imain \
 *             tests/test_lanlan_json.c main/lanlan_json.c -o /tmp/t && /tmp/t
 * Covers every documented limit and rejection path: empty input, truncation at
 * every shape, trailing garbage, control characters, bad escapes, surrogate
 * pairs and lone surrogates, depth overflow, oversized tokens, number grammar
 * (including exponents), duplicate keys, UTF-8 passthrough and a canary that
 * proves a failure never reads past the end of the caller's buffer. */
#include "lanlan_json.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- harness -- */

/* Parses `text` with a canary immediately after the caller's buffer, so any read
 * past the end trips an assertion instead of silently passing. */
#define CANARY_BYTES 8u
static unsigned char s_buffer[4096];

static lanlan_json_status_t parse(const char *text, lanlan_json_value_t *out) {
    size_t length = text ? strlen(text) : 0;
    assert(length + CANARY_BYTES < sizeof(s_buffer));
    if (length > 0) memcpy(s_buffer, text, length);
    memset(s_buffer + length, 0xA5, CANARY_BYTES);
    lanlan_json_t json;
    lanlan_json_init(&json, (const char *)s_buffer, length);
    lanlan_json_status_t status = lanlan_json_root(&json, out);
    for (size_t i = 0; i < CANARY_BYTES; ++i) assert(s_buffer[length + i] == 0xA5);
    return status;
}

/* Parses a valid document and returns its root. */
static lanlan_json_value_t root_of(const char *text) {
    lanlan_json_value_t value;
    assert(parse(text, &value) == LANLAN_JSON_OK);
    return value;
}

static lanlan_json_t json_of(const char *text) {
    lanlan_json_t json;
    lanlan_json_init(&json, text, strlen(text));
    lanlan_json_value_t value;
    assert(lanlan_json_root(&json, &value) == LANLAN_JSON_OK);
    return json;
}

static void expect_status(const char *text, lanlan_json_status_t expected) {
    lanlan_json_value_t value;
    lanlan_json_status_t actual = parse(text, &value);
    if (actual != expected) {
        printf("expect_status('%s'): expected %s, got %s\n", text,
               lanlan_json_status_name(expected), lanlan_json_status_name(actual));
    }
    assert(actual == expected);
}

/* ------------------------------------------------------------------ shapes -- */

static void test_scalars_and_root_types(void) {
    lanlan_json_value_t value = root_of("true");
    assert(value.type == LANLAN_JSON_TYPE_BOOL);
    lanlan_json_t json = json_of("true");
    bool flag = false;
    assert(lanlan_json_get_bool(&json, &value, &flag) == LANLAN_JSON_OK && flag);

    value = root_of("false");
    json = json_of("false");
    flag = true;
    assert(lanlan_json_get_bool(&json, &value, &flag) == LANLAN_JSON_OK && !flag);

    assert(root_of("null").type == LANLAN_JSON_TYPE_NULL);
    assert(root_of("  42  ").type == LANLAN_JSON_TYPE_NUMBER);
    assert(root_of("\"x\"").type == LANLAN_JSON_TYPE_STRING);
    assert(root_of("[]").type == LANLAN_JSON_TYPE_ARRAY);
    assert(root_of("{}").type == LANLAN_JSON_TYPE_OBJECT);

    /* Leading and trailing JSON whitespace is fine; the value slice excludes it. */
    lanlan_json_value_t spaced = root_of(" \t\r\n { } \n");
    assert(spaced.type == LANLAN_JSON_TYPE_OBJECT);
    lanlan_json_t spaced_json = json_of(" \t\r\n { } \n");
    assert(lanlan_json_object_get(&spaced_json, &spaced, "anything", &value)
           == LANLAN_JSON_ERR_NOT_FOUND);

    /* Type mismatches are reported, not coerced. */
    json = json_of("123");
    value = root_of("123");
    assert(lanlan_json_get_bool(&json, &value, &flag) == LANLAN_JSON_ERR_TYPE);
    char text[16];
    assert(lanlan_json_get_string(&json, &value, text, sizeof(text), NULL)
           == LANLAN_JSON_ERR_TYPE);
}

static void test_empty_and_truncated(void) {
    lanlan_json_value_t value;
    lanlan_json_t json;
    lanlan_json_init(&json, NULL, 0);
    assert(lanlan_json_root(&json, &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("   ", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("{", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("{\"a\"", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("{\"a\":", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("[", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("[1,", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("[1", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("\"abc", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("{\"a\":\"b\\", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("tru", &value) == LANLAN_JSON_ERR_SYNTAX);
    assert(parse("nul", &value) == LANLAN_JSON_ERR_SYNTAX);
    assert(parse("fals", &value) == LANLAN_JSON_ERR_SYNTAX);
}

static void test_syntax_and_trailing(void) {
    lanlan_json_value_t value;
    assert(parse("{} {}", &value) == LANLAN_JSON_ERR_TRAILING);
    assert(parse("1 2", &value) == LANLAN_JSON_ERR_TRAILING);
    assert(parse("{},", &value) == LANLAN_JSON_ERR_TRAILING);
    assert(parse("\"a\"x", &value) == LANLAN_JSON_ERR_TRAILING);
    assert(parse("{1:2}", &value) == LANLAN_JSON_ERR_SYNTAX);
    assert(parse("{\"a\" 1}", &value) == LANLAN_JSON_ERR_SYNTAX);
    assert(parse("{\"a\":1,}", &value) == LANLAN_JSON_ERR_SYNTAX);
    assert(parse("[1,]", &value) == LANLAN_JSON_ERR_SYNTAX);
    assert(parse("[1 2]", &value) == LANLAN_JSON_ERR_SYNTAX);
    assert(parse("[,]", &value) == LANLAN_JSON_ERR_SYNTAX);
    assert(parse("'a'", &value) == LANLAN_JSON_ERR_SYNTAX);
    assert(parse("@{", &value) == LANLAN_JSON_ERR_SYNTAX);

    /* The reported error offset points at the offending byte. */
    lanlan_json_t json;
    lanlan_json_init(&json, "{} {}", 5);
    assert(lanlan_json_root(&json, &value) == LANLAN_JSON_ERR_TRAILING);
    assert(lanlan_json_error_offset(&json) == 3);
}

static void test_numbers(void) {
    lanlan_json_t json;
    lanlan_json_value_t value;
    double number = 0;
    int64_t integer = 0;

    json = json_of("-12.5");
    value = root_of("-12.5");
    assert(lanlan_json_get_double(&json, &value, &number) == LANLAN_JSON_OK);
    assert(number > -12.5001 && number < -12.4999);
    /* A non-integral value is refused by the integer reader. */
    assert(lanlan_json_get_i64(&json, &value, &integer) == LANLAN_JSON_ERR_NUMBER);

    /* An exponent keeps its value, and an integral exponent form reads as an
     * integer: -12.5e2 is exactly -1250. */
    json = json_of("-12.5e2");
    value = root_of("-12.5e2");
    assert(lanlan_json_get_double(&json, &value, &number) == LANLAN_JSON_OK);
    assert(number > -1250.0001 && number < -1249.9999);
    assert(lanlan_json_get_i64(&json, &value, &integer) == LANLAN_JSON_OK && integer == -1250);

    json = json_of("412.0");
    value = root_of("412.0");
    assert(lanlan_json_get_i64(&json, &value, &integer) == LANLAN_JSON_OK && integer == 412);

    json = json_of("7e2");
    value = root_of("7e2");
    assert(lanlan_json_get_i64(&json, &value, &integer) == LANLAN_JSON_OK && integer == 700);

    json = json_of("0");
    value = root_of("0");
    assert(lanlan_json_get_i64(&json, &value, &integer) == LANLAN_JSON_OK && integer == 0);

    json = json_of("4294967295");
    value = root_of("4294967295");
    assert(lanlan_json_get_i64(&json, &value, &integer) == LANLAN_JSON_OK);
    assert(integer == 4294967295LL);

    /* A magnitude past int64 is a number error, not a wrap. */
    json = json_of("1e30");
    value = root_of("1e30");
    assert(lanlan_json_get_i64(&json, &value, &integer) == LANLAN_JSON_ERR_NUMBER);

    /* The grammar is strict. A leading zero stops the number token, so the
     * leftover digit is reported as trailing garbage rather than as a number. */
    expect_status("01", LANLAN_JSON_ERR_TRAILING);
    /* A lone minus sign has no digits left to read. */
    expect_status("-", LANLAN_JSON_ERR_TRUNCATED);
    expect_status("1.", LANLAN_JSON_ERR_NUMBER);
    expect_status(".5", LANLAN_JSON_ERR_SYNTAX);
    expect_status("+1", LANLAN_JSON_ERR_SYNTAX);
    expect_status("1e", LANLAN_JSON_ERR_NUMBER);
    expect_status("1e+", LANLAN_JSON_ERR_NUMBER);
    expect_status("--1", LANLAN_JSON_ERR_NUMBER);
    expect_status("0x10", LANLAN_JSON_ERR_TRAILING);
}

/* ----------------------------------------------------------------- strings -- */

static void test_string_escapes_and_utf8(void) {
    lanlan_json_t json;
    lanlan_json_value_t value;
    char out[128];

    /* Passthrough of raw multi-byte UTF-8 (the Chinese note path). */
    json = json_of("\"\\u5582\\u98df\"");
    value = root_of("\"\\u5582\\u98df\"");
    assert(lanlan_json_get_string(&json, &value, out, sizeof(out), NULL) == LANLAN_JSON_OK);
    assert(strcmp(out, "\xE5\x96\x82\xE9\xA3\x9F") == 0);

    /* Every short escape. */
    json = json_of("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"");
    value = root_of("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"");
    assert(lanlan_json_get_string(&json, &value, out, sizeof(out), NULL) == LANLAN_JSON_OK);
    assert(strcmp(out, "\"\\/\b\f\n\r\t") == 0);

    /* \u00e9 -> two bytes; \u0041 -> one ASCII byte. */
    json = json_of("\"\\u0041\\u00e9\"");
    value = root_of("\"\\u0041\\u00e9\"");
    assert(lanlan_json_get_string(&json, &value, out, sizeof(out), NULL) == LANLAN_JSON_OK);
    assert(strcmp(out, "A\xC3\xA9") == 0);

    /* A surrogate pair becomes one four-byte UTF-8 character. */
    json = json_of("\"\\uD83D\\uDE00\"");
    value = root_of("\"\\uD83D\\uDE00\"");
    assert(lanlan_json_get_string(&json, &value, out, sizeof(out), NULL) == LANLAN_JSON_OK);
    assert(strcmp(out, "\xF0\x9F\x98\x80") == 0);

    /* Mixed content keeps its order. */
    json = json_of("\"a\\u4e2d\\uD83D\\uDE00z\"");
    value = root_of("\"a\\u4e2d\\uD83D\\uDE00z\"");
    size_t decoded = 0;
    assert(lanlan_json_get_string(&json, &value, out, sizeof(out), &decoded) == LANLAN_JSON_OK);
    assert(strcmp(out, "a\xE4\xB8\xAD\xF0\x9F\x98\x80z") == 0);
    assert(decoded == strlen(out));

    /* A caller buffer that is too small is filled to a character boundary and
     * still reports the full decoded length. */
    char small[8];
    json = json_of("\"a\\u4e2d\\uD83D\\uDE00z\"");
    value = root_of("\"a\\u4e2d\\uD83D\\uDE00z\"");
    decoded = 0;
    assert(lanlan_json_get_string(&json, &value, small, sizeof(small), &decoded)
           == LANLAN_JSON_OK);
    assert(strcmp(small, "a\xE4\xB8\xAD") == 0);
    assert(decoded == strlen("a\xE4\xB8\xAD\xF0\x9F\x98\x80z"));

    /* Validation without a destination buffer. */
    json = json_of("\"\\uD83D\\uDE00\"");
    value = root_of("\"\\uD83D\\uDE00\"");
    assert(lanlan_json_get_string(&json, &value, NULL, 0, &decoded) == LANLAN_JSON_OK);
    assert(decoded == 4);

    /* Comparison decodes escapes too. */
    json = json_of("\"cursor_invalid\"");
    value = root_of("\"cursor_invalid\"");
    assert(lanlan_json_string_equals(&json, &value, "cursor_invalid"));
    assert(!lanlan_json_string_equals(&json, &value, "cursor"));
    json = json_of("\"\\u0063ursor_invalid\"");
    value = root_of("\"\\u0063ursor_invalid\"");
    assert(lanlan_json_string_equals(&json, &value, "cursor_invalid"));
}

static void test_string_rejections(void) {
    lanlan_json_value_t value;
    char out[32];

    /* Raw control characters are never allowed inside a string. */
    assert(parse("\"a\x01\xb\"", &value) == LANLAN_JSON_ERR_STRING);
    assert(parse("\"a\nb\"", &value) == LANLAN_JSON_ERR_STRING);
    /* DEL is not a JSON control character (RFC 8259 forbids only U+0000..U+001F
     * raw), so it passes through here; the record and caregiver layers sanitize
     * it later. */
    assert(parse("\"\x7f\"", &value) == LANLAN_JSON_OK);

    /* Bad escapes. */
    assert(parse("\"\\x\"", &value) == LANLAN_JSON_ERR_STRING);
    /* Only three of the four hex digits exist before the end of the document. */
    assert(parse("\"\\u12\"", &value) == LANLAN_JSON_ERR_TRUNCATED);
    assert(parse("\"\\uZZZZ\"", &value) == LANLAN_JSON_ERR_STRING);
    assert(parse("\"\\u12g4\"", &value) == LANLAN_JSON_ERR_STRING);

    /* Lone surrogates: a high one without a low, and a low one on its own. */
    assert(parse("\"\\uD83D\"", &value) == LANLAN_JSON_ERR_STRING);
    assert(parse("\"\\uD83Dx\"", &value) == LANLAN_JSON_ERR_STRING);
    assert(parse("\"\\uD83D\\u0041\"", &value) == LANLAN_JSON_ERR_STRING);
    assert(parse("\"\\uDE00\"", &value) == LANLAN_JSON_ERR_STRING);
    /* A high surrogate with a bad second escape. */
    assert(parse("\"\\uD83D\\n\"", &value) == LANLAN_JSON_ERR_STRING);

    /* The getter refuses non-strings. */
    lanlan_json_t json = json_of("12");
    value = root_of("12");
    assert(lanlan_json_get_string(&json, &value, out, sizeof(out), NULL) == LANLAN_JSON_ERR_TYPE);
}

static void test_limits(void) {
    lanlan_json_value_t value;
    /* A string token past the raw byte limit. */
    static char big[LANLAN_JSON_MAX_STRING_BYTES + 32];
    big[0] = '"';
    memset(big + 1, 'a', sizeof(big) - 3);
    big[sizeof(big) - 2] = '"';
    big[sizeof(big) - 1] = '\0';
    assert(parse(big, &value) == LANLAN_JSON_ERR_TOO_LARGE);

    /* The same string just inside the limit is accepted. */
    static char ok[LANLAN_JSON_MAX_STRING_BYTES + 1];
    ok[0] = '"';
    memset(ok + 1, 'a', LANLAN_JSON_MAX_STRING_BYTES - 2);
    ok[LANLAN_JSON_MAX_STRING_BYTES - 1] = '"';
    ok[LANLAN_JSON_MAX_STRING_BYTES] = '\0';
    assert(parse(ok, &value) == LANLAN_JSON_OK);

    /* A number token past the limit. */
    static char number[LANLAN_JSON_MAX_NUMBER_BYTES + 8];
    memset(number, '1', sizeof(number) - 1);
    number[sizeof(number) - 1] = '\0';
    assert(parse(number, &value) == LANLAN_JSON_ERR_TOO_LARGE);
}

static void test_depth(void) {
    lanlan_json_value_t value;
    static char deep[64];
    /* The root counts as depth 1, so LANLAN_JSON_MAX_DEPTH open brackets are
     * accepted and one more is refused. */
    size_t depth = LANLAN_JSON_MAX_DEPTH;
    for (size_t i = 0; i < depth; ++i) deep[i] = '[';
    for (size_t i = 0; i < depth; ++i) deep[depth + i] = ']';
    deep[depth * 2] = '\0';
    assert(parse(deep, &value) == LANLAN_JSON_OK);

    depth = LANLAN_JSON_MAX_DEPTH + 1;
    for (size_t i = 0; i < depth; ++i) deep[i] = '[';
    for (size_t i = 0; i < depth; ++i) deep[depth + i] = ']';
    deep[depth * 2] = '\0';
    assert(parse(deep, &value) == LANLAN_JSON_ERR_DEPTH);

    /* Objects nest the same way. */
    static char objects[64];
    size_t levels = LANLAN_JSON_MAX_DEPTH + 1;
    size_t offset = 0;
    for (size_t i = 0; i < levels; ++i) {
        memcpy(objects + offset, "{\"a\":", 5);
        offset += 5;
    }
    objects[offset++] = '1';
    for (size_t i = 0; i < levels; ++i) objects[offset++] = '}';
    objects[offset] = '\0';
    assert(parse(objects, &value) == LANLAN_JSON_ERR_DEPTH);
}

/* ----------------------------------------------------------------- objects -- */

static void test_object_lookup(void) {
    const char *text = "{\"a\":1,\"b\":{\"c\":[true,null]},\"d\":\"x\",\"unknown\":9}";
    lanlan_json_t json = json_of(text);
    lanlan_json_value_t root = root_of(text);
    lanlan_json_value_t member;

    assert(root.type == LANLAN_JSON_TYPE_OBJECT);
    assert(lanlan_json_object_get(&json, &root, "a", &member) == LANLAN_JSON_OK);
    assert(member.type == LANLAN_JSON_TYPE_NUMBER);
    int64_t integer = 0;
    assert(lanlan_json_get_i64(&json, &member, &integer) == LANLAN_JSON_OK && integer == 1);

    assert(lanlan_json_object_get(&json, &root, "b", &member) == LANLAN_JSON_OK);
    assert(member.type == LANLAN_JSON_TYPE_OBJECT);
    lanlan_json_value_t inner;
    assert(lanlan_json_object_get(&json, &member, "c", &inner) == LANLAN_JSON_OK);
    assert(inner.type == LANLAN_JSON_TYPE_ARRAY);
    assert(lanlan_json_array_size(&json, &inner) == 2);

    /* Absent members are NOT_FOUND, not a parse failure. */
    assert(lanlan_json_object_get(&json, &root, "missing", &member) == LANLAN_JSON_ERR_NOT_FOUND);
    /* An escaped key matches its decoded name. */
    const char *escaped = "{\"\\u0061\":7}";
    lanlan_json_t escaped_json = json_of(escaped);
    lanlan_json_value_t escaped_root = root_of(escaped);
    assert(lanlan_json_object_get(&escaped_json, &escaped_root, "a", &member)
           == LANLAN_JSON_OK);

    /* Duplicate keys: the LAST one wins, as documented. */
    const char *duplicate = "{\"a\":1,\"b\":2,\"a\":3}";
    lanlan_json_t duplicate_json = json_of(duplicate);
    lanlan_json_value_t duplicate_root = root_of(duplicate);
    assert(lanlan_json_object_get(&duplicate_json, &duplicate_root, "a", &member)
           == LANLAN_JSON_OK);
    assert(lanlan_json_get_i64(&duplicate_json, &member, &integer) == LANLAN_JSON_OK);
    assert(integer == 3);

    /* A non-object is a type error, including for an array. */
    lanlan_json_t array_json = json_of("[1]");
    lanlan_json_value_t array_root = root_of("[1]");
    assert(lanlan_json_object_get(&array_json, &array_root, "a", &member)
           == LANLAN_JSON_ERR_TYPE);
}

static void test_arrays(void) {
    const char *text = "{\"items\":[1,\"two\",false,null,{\"k\":\"v\"}],\"empty\":[]}";
    lanlan_json_t json = json_of(text);
    lanlan_json_value_t root = root_of(text);
    lanlan_json_value_t array;
    assert(lanlan_json_object_get(&json, &root, "items", &array) == LANLAN_JSON_OK);
    assert(lanlan_json_array_size(&json, &array) == 5);

    lanlan_json_array_iter_t iter;
    assert(lanlan_json_array_begin(&json, &array, &iter) == LANLAN_JSON_OK);
    lanlan_json_value_t element;
    size_t index = 0;
    const lanlan_json_type_t expected[5] = {LANLAN_JSON_TYPE_NUMBER, LANLAN_JSON_TYPE_STRING,
                                            LANLAN_JSON_TYPE_BOOL, LANLAN_JSON_TYPE_NULL,
                                            LANLAN_JSON_TYPE_OBJECT};
    while (lanlan_json_array_next(&json, &iter, &element) == LANLAN_JSON_OK) {
        assert(index < 5);
        assert(element.type == expected[index]);
        ++index;
    }
    assert(index == 5);
    /* The iterator keeps reporting the end. */
    assert(lanlan_json_array_next(&json, &iter, &element) == LANLAN_JSON_ERR_NOT_FOUND);

    /* Random access, and the index error past the end. */
    assert(lanlan_json_array_at(&json, &array, 1, &element) == LANLAN_JSON_OK);
    assert(element.type == LANLAN_JSON_TYPE_STRING);
    char out[16];
    assert(lanlan_json_get_string(&json, &element, out, sizeof(out), NULL) == LANLAN_JSON_OK);
    assert(strcmp(out, "two") == 0);
    assert(lanlan_json_array_at(&json, &array, 5, &element) == LANLAN_JSON_ERR_INDEX);

    /* An empty array has size 0 and its iterator ends immediately. */
    lanlan_json_value_t empty;
    assert(lanlan_json_object_get(&json, &root, "empty", &empty) == LANLAN_JSON_OK);
    assert(lanlan_json_array_size(&json, &empty) == 0);
    assert(lanlan_json_array_begin(&json, &empty, &iter) == LANLAN_JSON_OK);
    assert(lanlan_json_array_next(&json, &iter, &element) == LANLAN_JSON_ERR_NOT_FOUND);

    /* A non-array is rejected by the iterator. */
    lanlan_json_value_t not_array;
    assert(lanlan_json_object_get(&json, &root, "items", &not_array) == LANLAN_JSON_OK);
    lanlan_json_t scalar_json = json_of("3");
    lanlan_json_value_t scalar_root = root_of("3");
    assert(lanlan_json_array_begin(&scalar_json, &scalar_root, &iter) == LANLAN_JSON_ERR_TYPE);
}

static void test_status_names(void) {
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_OK), "ok") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_TRUNCATED), "truncated") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_SYNTAX), "syntax") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_DEPTH), "depth") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_STRING), "string") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_NUMBER), "number") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_TOO_LARGE), "too_large") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_TRAILING), "trailing") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_TYPE), "type") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_NOT_FOUND), "not_found") == 0);
    assert(strcmp(lanlan_json_status_name(LANLAN_JSON_ERR_INDEX), "index") == 0);
    assert(strcmp(lanlan_json_status_name((lanlan_json_status_t)99), "unknown") == 0);
    assert(strcmp(lanlan_json_type_name(LANLAN_JSON_TYPE_OBJECT), "object") == 0);
    assert(strcmp(lanlan_json_type_name(LANLAN_JSON_TYPE_ARRAY), "array") == 0);
    assert(strcmp(lanlan_json_type_name(LANLAN_JSON_TYPE_STRING), "string") == 0);
    assert(strcmp(lanlan_json_type_name(LANLAN_JSON_TYPE_NUMBER), "number") == 0);
    assert(strcmp(lanlan_json_type_name(LANLAN_JSON_TYPE_BOOL), "bool") == 0);
    assert(strcmp(lanlan_json_type_name(LANLAN_JSON_TYPE_NULL), "null") == 0);
}

/* Every rejected document is replayed with a canary so a failure can never read
 * past the caller's buffer; `parse()` already asserts that on every call. */
static void test_failures_never_run_past_the_buffer(void) {
    static const char *const broken[] = {
        "{",       "[",       "\"",      "{\"a\"",  "{\"a\":",  "[1,",
        "{\"a\":1", "\"\\",   "\"\\u",   "\"\\uD83D", "tru",   "01",
        "{} {}",   "[[[[[[[[[[", "{\"a\":\"b}", "nul",
    };
    lanlan_json_value_t value;
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        assert(parse(broken[i], &value) != LANLAN_JSON_OK);
    }
}

int main(void) {
    test_scalars_and_root_types();
    test_empty_and_truncated();
    test_syntax_and_trailing();
    test_numbers();
    test_string_escapes_and_utf8();
    test_string_rejections();
    test_limits();
    test_depth();
    test_object_lookup();
    test_arrays();
    test_status_names();
    test_failures_never_run_past_the_buffer();
    puts("Lanlan json: PASS (shapes, truncation, escapes, surrogates, depth, limits, numbers, "
         "duplicates, UTF-8, canary)");
    return 0;
}

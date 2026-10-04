/* Cyber Lanlan time arithmetic: UTC epoch seconds plus a UTC offset in minutes
 * to local date/time and back, RFC 3339 parsing and formatting, and clock-trust
 * evaluation.
 *
 * No libc time functions and no timezone database are used: the service sends
 * `utc_offset_minutes` in every sync response (see
 * docs/applications/cyber-lanlan-service.md section 6.1) and the device applies
 * it directly. Everything here is integer arithmetic over int64_t, so the
 * behaviour is identical on the host and on the ESP32-C3. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The longest text form is "YYYY-MM-DDTHH:MM:SS.fffffff+HH:MM" plus NUL. */
#define LANLAN_RFC3339_MAX 40

/* Offsets outside this range are not real UTC offsets. Some zones use :45. */
#define LANLAN_TIME_OFFSET_MIN (-12 * 60)
#define LANLAN_TIME_OFFSET_MAX (14 * 60)

/* Sane floor for a trusted clock: 2020-01-01T00:00:00Z. A device that lost its
 * RTC or received a nonsense response reports a time below this, and any time
 * at or below the floor must never be used to date a record or ring a
 * reminder. Kept as an explicit constant so the value is auditable. */
#define LANLAN_TIME_TRUST_FLOOR_EPOCH ((int64_t)1577836800)

/* Widest range this module accepts; outside it the service data is treated as
 * corrupt. 2001-09-09T01:46:40Z to 2100-01-01T00:00:00Z. */
#define LANLAN_TIME_MIN_EPOCH ((int64_t)1000000000)
#define LANLAN_TIME_MAX_EPOCH ((int64_t)4102444800)

typedef enum {
    LANLAN_TIME_OK = 0,
    LANLAN_TIME_ERR_RANGE,   /* seconds outside LANLAN_TIME_MIN/MAX_EPOCH */
    LANLAN_TIME_ERR_OFFSET,  /* offset outside the real-world range */
    LANLAN_TIME_ERR_FORMAT   /* malformed or unsupported text */
} lanlan_time_status_t;

typedef struct {
    int64_t day;           /* local day index: days since 1970-01-01, floor */
    int year;              /* 1970.. */
    unsigned month;        /* 1..12 */
    unsigned day_in_month; /* 1..31; the day index lives in `day` */
} lanlan_date_t;

const char *lanlan_time_status_name(lanlan_time_status_t status);

/* --------------------------------------------------------- day arithmetic -- */

/* Floor division/modulo: C's / truncates towards zero, which would break every
 * negative (pre-1970) value. */
int64_t lanlan_time_floor_div(int64_t value, int64_t divisor);
int64_t lanlan_time_floor_mod(int64_t value, int64_t divisor);

/* Days-since-epoch <-> civil date (Howard Hinnant's algorithm). Valid for the
 * whole int32 date range; callers stay inside LANLAN_TIME_MIN/MAX_EPOCH. */
int64_t lanlan_time_days_from_civil(int year, unsigned month, unsigned day);
void lanlan_time_civil_from_days(int64_t day, lanlan_date_t *date);

/* ---------------------------------------------------- epoch <-> local time -- */

/* Seconds within the local day (0..86399). */
lanlan_time_status_t lanlan_time_local_seconds(int64_t epoch, int16_t offset_min,
                                               int32_t *seconds_of_day);
/* Local wall clock, hour 0..23 and minute 0..59. */
lanlan_time_status_t lanlan_time_local_hhmm(int64_t epoch, int16_t offset_min, int *hour,
                                            int *minute);
/* Local date, including its floor day index used for reminder instances. */
lanlan_time_status_t lanlan_time_local_date(int64_t epoch, int16_t offset_min,
                                            lanlan_date_t *date);
/* Inverse of the two functions above for a wall-clock minute: the offset is
 * applied in reverse, so the result is a UTC epoch second at :00. */
lanlan_time_status_t lanlan_time_from_local(int64_t local_day, int hour, int minute,
                                            int16_t offset_min, int64_t *epoch);

/* ------------------------------------------------------------------ ISO -- */

/* Parses "YYYY-MM-DDTHH:MM:SSZ". Also accepts a fractional part and a numeric
 * "±HH:MM" offset, and a lowercase "t"/"z". The offset is applied to normalize
 * the result to UTC. `offset_min` is optional and receives the parsed offset
 * (0 for "Z"), not the caller's local offset. */
lanlan_time_status_t lanlan_time_parse_rfc3339(const char *text, int64_t *epoch,
                                               int16_t *offset_min);
/* Writes "YYYY-MM-DDTHH:MM:SSZ" for a UTC second. Returns the byte length, or 0
 * when the buffer is too small (needs LANLAN_RFC3339_MAX). */
size_t lanlan_time_format_rfc3339(int64_t epoch, char *out, size_t out_size);

/* ------------------------------------------------------------- HH:MM text -- */

/* Strict "HH:MM" with HH 00..23 and MM 00..59. Text of any other length or
 * shape is rejected; "7:30" is not accepted because the cache stores a
 * fixed-width field. */
bool lanlan_time_parse_hhmm(const char *text, int *hour, int *minute);
/* Formats "HH:MM"; returns the byte length or 0 when out is too small (>= 6). */
size_t lanlan_time_format_hhmm(int hour, int minute, char *out, size_t out_size);
/* Minutes since local midnight, or -1 for a malformed string. */
int lanlan_time_hhmm_minutes(const char *text);

/* --------------------------------------------------------------- reminders -- */

/* 0 when `now` is exactly at `reminder`; negative when early, positive when
 * late. Both arguments are "HH:MM" and are validated, so -1 is also the
 * malformed-input error value; callers must validate first. */
int lanlan_time_hhmm_compare(const char *reminder, const char *now);
/* True when the current local time is at or past `reminder` on the same local
 * day. */
bool lanlan_time_hhmm_due(const char *reminder, const char *now);

/* ------------------------------------------------------------ clock trust -- */

/* A clock is trusted only when a successful SNTP or service response has been
 * recorded AND the resulting time is at or above LANLAN_TIME_TRUST_FLOOR_EPOCH.
 * Both conditions are required: the flag alone would trust a bogus response,
 * and a plausible epoch alone would trust a device that never synchronized. */
bool lanlan_time_clock_trusted(bool synchronized, int64_t epoch);

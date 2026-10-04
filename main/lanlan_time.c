/* Cyber Lanlan time arithmetic implementation. See lanlan_time.h. */
#include "lanlan_time.h"

#include <stdio.h>
#include <string.h>

const char *lanlan_time_status_name(lanlan_time_status_t status) {
    switch (status) {
    case LANLAN_TIME_OK: return "ok";
    case LANLAN_TIME_ERR_RANGE: return "range";
    case LANLAN_TIME_ERR_OFFSET: return "offset";
    case LANLAN_TIME_ERR_FORMAT: return "format";
    }
    return "unknown";
}

int64_t lanlan_time_floor_div(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) --quotient;
    return quotient;
}

int64_t lanlan_time_floor_mod(int64_t value, int64_t divisor) {
    return value - lanlan_time_floor_div(value, divisor) * divisor;
}

/* Days from 1970-01-01 for a proleptic Gregorian date. Shifts the year so that
 * March is month 0, which makes the leap day of the shifted year the last day
 * and removes the special case from the month arithmetic. */
int64_t lanlan_time_days_from_civil(int year, unsigned month, unsigned day) {
    int64_t y = year;
    y -= month <= 2;
    int64_t era = lanlan_time_floor_div(y, 400);
    unsigned yoe = (unsigned)(y - era * 400);                       /* 0..399 */
    unsigned doy = (153u * (month + (month > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;        /* 0..146096 */
    return era * 146097 + (int64_t)doe - 719468;
}

void lanlan_time_civil_from_days(int64_t day, lanlan_date_t *date) {
    if (!date) return;
    int64_t z = day + 719468;
    int64_t era = lanlan_time_floor_div(z, 146097);
    unsigned doe = (unsigned)(z - era * 146097);                    /* 0..146096 */
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);      /* 0..365 */
    unsigned mp = (5u * doy + 2u) / 153u;                           /* 0..11 */
    unsigned d = doy - (153u * mp + 2u) / 5u + 1u;                  /* 1..31 */
    unsigned m = mp + (mp < 10u ? 3u : (unsigned)-9);               /* 1..12 */
    date->day = day;
    date->year = (int)(y + (m <= 2));
    date->month = m;
    date->day_in_month = d;
}

static lanlan_time_status_t check_offset(int16_t offset_min) {
    if (offset_min < LANLAN_TIME_OFFSET_MIN || offset_min > LANLAN_TIME_OFFSET_MAX) {
        return LANLAN_TIME_ERR_OFFSET;
    }
    return LANLAN_TIME_OK;
}

static lanlan_time_status_t check_epoch(int64_t epoch) {
    if (epoch < LANLAN_TIME_MIN_EPOCH || epoch > LANLAN_TIME_MAX_EPOCH) {
        return LANLAN_TIME_ERR_RANGE;
    }
    return LANLAN_TIME_OK;
}

lanlan_time_status_t lanlan_time_local_seconds(int64_t epoch, int16_t offset_min,
                                               int32_t *seconds_of_day) {
    lanlan_time_status_t status = check_offset(offset_min);
    if (status != LANLAN_TIME_OK) return status;
    status = check_epoch(epoch);
    if (status != LANLAN_TIME_OK) return status;
    int64_t local = epoch + (int64_t)offset_min * 60;
    if (seconds_of_day) *seconds_of_day = (int32_t)lanlan_time_floor_mod(local, 86400);
    return LANLAN_TIME_OK;
}

lanlan_time_status_t lanlan_time_local_hhmm(int64_t epoch, int16_t offset_min, int *hour,
                                            int *minute) {
    int32_t seconds = 0;
    lanlan_time_status_t status = lanlan_time_local_seconds(epoch, offset_min, &seconds);
    if (status != LANLAN_TIME_OK) return status;
    if (hour) *hour = (int)(seconds / 3600);
    if (minute) *minute = (int)((seconds % 3600) / 60);
    return LANLAN_TIME_OK;
}

lanlan_time_status_t lanlan_time_local_date(int64_t epoch, int16_t offset_min,
                                            lanlan_date_t *date) {
    lanlan_time_status_t status = check_offset(offset_min);
    if (status != LANLAN_TIME_OK) return status;
    status = check_epoch(epoch);
    if (status != LANLAN_TIME_OK) return status;
    int64_t local = epoch + (int64_t)offset_min * 60;
    int64_t day = lanlan_time_floor_div(local, 86400);
    if (date) lanlan_time_civil_from_days(day, date);
    return LANLAN_TIME_OK;
}

lanlan_time_status_t lanlan_time_from_local(int64_t local_day, int hour, int minute,
                                            int16_t offset_min, int64_t *epoch) {
    lanlan_time_status_t status = check_offset(offset_min);
    if (status != LANLAN_TIME_OK) return status;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return LANLAN_TIME_ERR_FORMAT;
    int64_t value = local_day * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60
                    - (int64_t)offset_min * 60;
    status = check_epoch(value);
    if (status != LANLAN_TIME_OK) return status;
    if (epoch) *epoch = value;
    return LANLAN_TIME_OK;
}

/* ------------------------------------------------------------------ ISO -- */

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Reads exactly `count` digits at *pos and advances it. */
static bool read_digits(const char *text, size_t *pos, size_t count, int *value) {
    int result = 0;
    for (size_t i = 0; i < count; ++i) {
        char c = text[*pos + i];
        if (!is_digit(c)) return false;
        result = result * 10 + (c - '0');
    }
    *pos += count;
    *value = result;
    return true;
}

lanlan_time_status_t lanlan_time_parse_rfc3339(const char *text, int64_t *epoch,
                                               int16_t *offset_min) {
    if (!text) return LANLAN_TIME_ERR_FORMAT;
    size_t pos = 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!read_digits(text, &pos, 4, &year)) return LANLAN_TIME_ERR_FORMAT;
    if (text[pos++] != '-') return LANLAN_TIME_ERR_FORMAT;
    if (!read_digits(text, &pos, 2, &month)) return LANLAN_TIME_ERR_FORMAT;
    if (text[pos++] != '-') return LANLAN_TIME_ERR_FORMAT;
    if (!read_digits(text, &pos, 2, &day)) return LANLAN_TIME_ERR_FORMAT;
    if (text[pos] != 'T' && text[pos] != 't') return LANLAN_TIME_ERR_FORMAT;
    ++pos;
    if (!read_digits(text, &pos, 2, &hour)) return LANLAN_TIME_ERR_FORMAT;
    if (text[pos++] != ':') return LANLAN_TIME_ERR_FORMAT;
    if (!read_digits(text, &pos, 2, &minute)) return LANLAN_TIME_ERR_FORMAT;
    if (text[pos++] != ':') return LANLAN_TIME_ERR_FORMAT;
    if (!read_digits(text, &pos, 2, &second)) return LANLAN_TIME_ERR_FORMAT;

    if (text[pos] == '.') {
        ++pos;
        size_t digits = 0;
        while (is_digit(text[pos])) {
            ++pos;
            ++digits;
        }
        if (digits == 0) return LANLAN_TIME_ERR_FORMAT;
    }

    int16_t offset = 0;
    if (text[pos] == 'Z' || text[pos] == 'z') {
        ++pos;
    } else if (text[pos] == '+' || text[pos] == '-') {
        int sign = text[pos] == '-' ? -1 : 1;
        ++pos;
        int offset_hour = 0, offset_minute = 0;
        if (!read_digits(text, &pos, 2, &offset_hour)) return LANLAN_TIME_ERR_FORMAT;
        if (text[pos++] != ':') return LANLAN_TIME_ERR_FORMAT;
        if (!read_digits(text, &pos, 2, &offset_minute)) return LANLAN_TIME_ERR_FORMAT;
        if (offset_hour > 14 || offset_minute > 59) return LANLAN_TIME_ERR_FORMAT;
        int total = sign * (offset_hour * 60 + offset_minute);
        if (total < LANLAN_TIME_OFFSET_MIN || total > LANLAN_TIME_OFFSET_MAX) {
            return LANLAN_TIME_ERR_FORMAT;
        }
        offset = (int16_t)total;
    } else {
        return LANLAN_TIME_ERR_FORMAT;
    }
    if (text[pos] != '\0') return LANLAN_TIME_ERR_FORMAT;

    /* Reject impossible dates instead of normalizing them, so a corrupted
     * server string cannot silently shift into another month. */
    if (month < 1 || month > 12 || day < 1 || day > 31) return LANLAN_TIME_ERR_FORMAT;
    if (hour > 23 || minute > 59 || second > 59) return LANLAN_TIME_ERR_FORMAT;
    static const unsigned month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned max_day = month_days[month - 1];
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        if (leap) max_day = 29;
    }
    if ((unsigned)day > max_day) return LANLAN_TIME_ERR_FORMAT;

    int64_t value = lanlan_time_days_from_civil(year, (unsigned)month, (unsigned)day) * 86400
                    + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
    /* The text describes a local wall clock at `offset`; convert it to UTC. */
    value -= (int64_t)offset * 60;
    lanlan_time_status_t status = check_epoch(value);
    if (status != LANLAN_TIME_OK) return status;
    if (epoch) *epoch = value;
    if (offset_min) *offset_min = offset;
    return LANLAN_TIME_OK;
}

size_t lanlan_time_format_rfc3339(int64_t epoch, char *out, size_t out_size) {
    if (!out || out_size < LANLAN_RFC3339_MAX) return 0;
    if (check_epoch(epoch) != LANLAN_TIME_OK) {
        out[0] = '\0';
        return 0;
    }
    lanlan_date_t date = {0};
    lanlan_time_civil_from_days(lanlan_time_floor_div(epoch, 86400), &date);
    int32_t seconds = (int32_t)lanlan_time_floor_mod(epoch, 86400);
    int written = snprintf(out, out_size, "%04d-%02u-%02uT%02d:%02d:%02dZ", date.year, date.month,
                           date.day_in_month, (int)(seconds / 3600), (int)((seconds % 3600) / 60),
                           (int)(seconds % 60));
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)written;
}

/* ------------------------------------------------------------- HH:MM text -- */

bool lanlan_time_parse_hhmm(const char *text, int *hour, int *minute) {
    if (!text || strlen(text) != 5) return false;
    if (!is_digit(text[0]) || !is_digit(text[1]) || text[2] != ':' || !is_digit(text[3])
        || !is_digit(text[4])) {
        return false;
    }
    int h = (text[0] - '0') * 10 + (text[1] - '0');
    int m = (text[3] - '0') * 10 + (text[4] - '0');
    if (h > 23 || m > 59) return false;
    if (hour) *hour = h;
    if (minute) *minute = m;
    return true;
}

size_t lanlan_time_format_hhmm(int hour, int minute, char *out, size_t out_size) {
    if (!out || out_size < 6) return 0;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        out[0] = '\0';
        return 0;
    }
    int written = snprintf(out, out_size, "%02d:%02d", hour, minute);
    return written < 0 ? 0 : (size_t)written;
}

int lanlan_time_hhmm_minutes(const char *text) {
    int hour = 0, minute = 0;
    if (!lanlan_time_parse_hhmm(text, &hour, &minute)) return -1;
    return hour * 60 + minute;
}

int lanlan_time_hhmm_compare(const char *reminder, const char *now) {
    int reminder_minutes = lanlan_time_hhmm_minutes(reminder);
    int now_minutes = lanlan_time_hhmm_minutes(now);
    if (reminder_minutes < 0 || now_minutes < 0) return -1;
    return now_minutes - reminder_minutes;
}

bool lanlan_time_hhmm_due(const char *reminder, const char *now) {
    return lanlan_time_hhmm_compare(reminder, now) >= 0;
}

/* ------------------------------------------------------------ clock trust -- */

bool lanlan_time_clock_trusted(bool synchronized, int64_t epoch) {
    return synchronized && epoch >= LANLAN_TIME_TRUST_FLOOR_EPOCH
           && epoch <= LANLAN_TIME_MAX_EPOCH;
}

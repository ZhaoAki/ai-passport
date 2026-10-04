/* Host tests for main/lanlan_time.c.
 * Build:  cc -std=c11 -Wall -Wextra -Werror -Imain \
 *             tests/test_lanlan_time.c main/lanlan_time.c -o /tmp/t && /tmp/t
 * (main/lanlan_time.c has no other dependency besides libc.) */
#include "lanlan_time.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_civil_round_trip(void) {
    /* A few anchor dates, including the epoch itself and leap days. */
    const struct {
        int year;
        unsigned month;
        unsigned day;
        int64_t expected;
    } anchors[] = {
        {1970, 1, 1, 0},
        {1970, 1, 2, 1},
        {1969, 12, 31, -1},
        {2000, 2, 29, 11016},
        {2000, 3, 1, 11017},
        {2024, 2, 29, 19782},
        {2024, 3, 1, 19783},
        {2025, 10, 9, 20370},
        {2100, 1, 1, 47482},
    };
    for (size_t i = 0; i < sizeof(anchors) / sizeof(anchors[0]); ++i) {
        int64_t day = lanlan_time_days_from_civil(anchors[i].year, anchors[i].month,
                                                  anchors[i].day);
        assert(day == anchors[i].expected);
        lanlan_date_t date = {0};
        lanlan_time_civil_from_days(day, &date);
        assert(date.day == day);
        assert(date.year == anchors[i].year);
        assert(date.month == anchors[i].month);
        assert(date.day_in_month == anchors[i].day);
    }

    /* Every day across a leap year and a month boundary round-trips. */
    int64_t start = lanlan_time_days_from_civil(2024, 1, 1);
    for (int64_t offset = 0; offset < 366; ++offset) {
        lanlan_date_t date = {0};
        lanlan_time_civil_from_days(start + offset, &date);
        assert(lanlan_time_days_from_civil(date.year, date.month, date.day_in_month)
               == start + offset);
    }
}

static void test_floor_division(void) {
    assert(lanlan_time_floor_div(7, 3) == 2);
    assert(lanlan_time_floor_div(-7, 3) == -3);
    assert(lanlan_time_floor_div(6, 3) == 2);
    assert(lanlan_time_floor_div(-6, 3) == -2);
    assert(lanlan_time_floor_mod(7, 3) == 1);
    assert(lanlan_time_floor_mod(-7, 3) == 2);
    assert(lanlan_time_floor_mod(-86400, 86400) == 0);
    assert(lanlan_time_floor_mod(-1, 86400) == 86399);
}

static void test_local_conversions(void) {
    const int64_t epoch = 1760000000; /* 2025-10-09T08:53:20Z */
    int hour = 0, minute = 0;
    assert(lanlan_time_local_hhmm(epoch, 480, &hour, &minute) == LANLAN_TIME_OK);
    assert(hour == 16 && minute == 53);
    lanlan_date_t date = {0};
    assert(lanlan_time_local_date(epoch, 480, &date) == LANLAN_TIME_OK);
    assert(date.year == 2025 && date.month == 10 && date.day_in_month == 9);
    assert(date.day == 20370); /* absolute day index, not the day of the month */

    /* Negative offsets: 08:53 UTC is 03:53 in UTC-5, still the same date. */
    assert(lanlan_time_local_hhmm(epoch, -300, &hour, &minute) == LANLAN_TIME_OK);
    assert(hour == 3 && minute == 53);

    /* An epoch just after local midnight must not roll the date backwards. */
    assert(lanlan_time_local_hhmm(1760000000, -720, &hour, &minute) == LANLAN_TIME_OK);
    assert(hour == 20 && minute == 53);
    assert(lanlan_time_local_date(epoch, -720, &date) == LANLAN_TIME_OK);
    assert(date.day_in_month == 8 && date.day == 20369);

    /* Month boundary: 2025-10-31T20:00:00Z is 2025-11-01 in UTC+8. */
    int64_t month_end = lanlan_time_days_from_civil(2025, 10, 31) * 86400 + 20 * 3600;
    assert(lanlan_time_local_date(month_end, 480, &date) == LANLAN_TIME_OK);
    assert(date.year == 2025 && date.month == 11 && date.day_in_month == 1);
    assert(lanlan_time_local_date(month_end, 0, &date) == LANLAN_TIME_OK);
    assert(date.month == 10 && date.day_in_month == 31);

    /* Year boundary: 2025-12-31T20:00:00Z is 2026-01-01 in UTC+8. */
    int64_t year_end = lanlan_time_days_from_civil(2025, 12, 31) * 86400 + 20 * 3600;
    assert(lanlan_time_local_date(year_end, 480, &date) == LANLAN_TIME_OK);
    assert(date.year == 2026 && date.month == 1 && date.day_in_month == 1);
    assert(lanlan_time_local_date(year_end, -300, &date) == LANLAN_TIME_OK);
    assert(date.year == 2025 && date.day_in_month == 31);

    /* The MAX/MIN endpoints are accepted; one step outside is not. */
    assert(lanlan_time_local_hhmm(LANLAN_TIME_MIN_EPOCH, 0, &hour, &minute) == LANLAN_TIME_OK);
    assert(lanlan_time_local_hhmm(LANLAN_TIME_MAX_EPOCH, 0, &hour, &minute) == LANLAN_TIME_OK);
    assert(lanlan_time_local_hhmm(LANLAN_TIME_MIN_EPOCH - 1, 0, &hour, &minute)
           == LANLAN_TIME_ERR_RANGE);
    assert(lanlan_time_local_hhmm(LANLAN_TIME_MAX_EPOCH + 1, 0, &hour, &minute)
           == LANLAN_TIME_ERR_RANGE);
    assert(lanlan_time_local_hhmm(epoch, (int16_t)-13 * 60, &hour, &minute)
           == LANLAN_TIME_ERR_OFFSET);
    assert(lanlan_time_local_hhmm(epoch, (int16_t)15 * 60, &hour, &minute)
           == LANLAN_TIME_ERR_OFFSET);
    /* Half-hour and 45-minute zones are real and must stay accepted. */
    assert(lanlan_time_local_hhmm(epoch, 330, &hour, &minute) == LANLAN_TIME_OK);   /* +05:30 */
    assert(lanlan_time_local_hhmm(epoch, 345, &hour, &minute) == LANLAN_TIME_OK);   /* +05:45 */
    assert(lanlan_time_local_hhmm(epoch, -210, &hour, &minute) == LANLAN_TIME_OK);  /* -03:30 */

    assert(lanlan_time_local_seconds(epoch, 480, NULL) == LANLAN_TIME_OK);
    int32_t seconds = 0;
    assert(lanlan_time_local_seconds(epoch, 0, &seconds) == LANLAN_TIME_OK);
    assert(seconds == 8 * 3600 + 53 * 60 + 20);
}

static void test_local_to_epoch(void) {
    int64_t epoch = 0;
    /* The inverse is exact only to the minute: 2025-10-09 16:53 +08:00 is
     * 2025-10-09T08:53:00Z, twenty seconds before the 08:53:20 anchor used by
     * the epoch->local tests (the API takes a wall clock, not seconds). */
    assert(lanlan_time_from_local(20370, 16, 53, 480, &epoch) == LANLAN_TIME_OK);
    assert(epoch == 1759999980);
    assert(epoch + 20 == 1760000000);
    int hour = 0, minute = 0;
    assert(lanlan_time_local_hhmm(epoch, 480, &hour, &minute) == LANLAN_TIME_OK);
    assert(hour == 16 && minute == 53);

    /* A local wall-clock time is not unique; the offset disambiguates it. */
    int64_t other = 0;
    assert(lanlan_time_from_local(20370, 16, 53, 0, &other) == LANLAN_TIME_OK);
    assert(other == 1760028780); /* 16:53 in UTC */

    assert(lanlan_time_from_local(20370, 24, 0, 480, &epoch) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_from_local(20370, 0, 60, 480, &epoch) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_from_local(20370, -1, 0, 480, &epoch) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_from_local(0, 12, 0, (int16_t)9000, &epoch) == LANLAN_TIME_ERR_OFFSET);
    assert(lanlan_time_from_local(1000000, 12, 0, 0, &epoch) == LANLAN_TIME_ERR_RANGE);
}

static void test_rfc3339_accept(void) {
    int64_t epoch = 0;
    int16_t offset = 1;
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20Z", &epoch, &offset) == LANLAN_TIME_OK);
    assert(epoch == 1760000000 && offset == 0);
    assert(lanlan_time_parse_rfc3339("1970-01-01T00:00:00Z", NULL, NULL) == LANLAN_TIME_ERR_RANGE);

    /* Numeric offsets normalize to UTC. */
    assert(lanlan_time_parse_rfc3339("2025-10-09T16:53:20+08:00", &epoch, &offset)
           == LANLAN_TIME_OK);
    assert(epoch == 1760000000 && offset == 480);
    assert(lanlan_time_parse_rfc3339("2025-10-09T03:53:20-05:00", &epoch, &offset)
           == LANLAN_TIME_OK);
    assert(epoch == 1760000000 && offset == -300);
    assert(lanlan_time_parse_rfc3339("2025-10-09T14:23:20+05:30", &epoch, &offset)
           == LANLAN_TIME_OK);
    assert(epoch == 1760000000 && offset == 330);

    /* Fractional seconds are accepted and truncated to whole seconds. */
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20.5Z", &epoch, NULL) == LANLAN_TIME_OK);
    assert(epoch == 1760000000);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20.123456789Z", &epoch, NULL)
           == LANLAN_TIME_OK);
    assert(epoch == 1760000000);

    /* Lowercase separators and leap days. */
    assert(lanlan_time_parse_rfc3339("2025-10-09t08:53:20z", &epoch, NULL) == LANLAN_TIME_OK);
    assert(lanlan_time_parse_rfc3339("2024-02-29T00:00:00Z", &epoch, NULL) == LANLAN_TIME_OK);
    assert(epoch == 19782 * 86400);
    /* 2000-02-29 is a real date but below the accepted window (10^9 s), which
     * starts at 2001-09-09; the calendar itself handles it (see the civil
     * round-trip test). */
    assert(lanlan_time_parse_rfc3339("2000-02-29T00:00:00Z", &epoch, NULL)
           == LANLAN_TIME_ERR_RANGE);

    /* The accepted window endpoints. */
    assert(lanlan_time_parse_rfc3339("2001-09-09T01:46:40Z", &epoch, NULL) == LANLAN_TIME_OK);
    assert(epoch == LANLAN_TIME_MIN_EPOCH);
    assert(lanlan_time_parse_rfc3339("2100-01-01T00:00:00Z", &epoch, NULL) == LANLAN_TIME_OK);
    assert(epoch == LANLAN_TIME_MAX_EPOCH);
}

static void test_rfc3339_reject(void) {
    int64_t epoch = 0;
    assert(lanlan_time_parse_rfc3339(NULL, &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09 08:53:20Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20.Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20+0800", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20Q", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20Zx", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-13-01T00:00:00Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-00-01T00:00:00Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-00T00:00:00Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-32T00:00:00Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-04-31T00:00:00Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-02-29T00:00:00Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T24:00:00Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:60:00Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:60Z", &epoch, NULL) == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20+15:00", &epoch, NULL)
           == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("2025-10-09T08:53:20-13:00", &epoch, NULL)
           == LANLAN_TIME_ERR_FORMAT);
    assert(lanlan_time_parse_rfc3339("1969-12-31T23:59:59Z", &epoch, NULL) == LANLAN_TIME_ERR_RANGE);
    assert(lanlan_time_parse_rfc3339("2100-01-01T00:00:01Z", &epoch, NULL)
           == LANLAN_TIME_ERR_RANGE);
}

static void test_rfc3339_format(void) {
    char text[LANLAN_RFC3339_MAX];
    assert(lanlan_time_format_rfc3339(1760000000, text, sizeof(text)) == 20);
    assert(strcmp(text, "2025-10-09T08:53:20Z") == 0);
    assert(lanlan_time_format_rfc3339(0, text, sizeof(text)) == 0);
    assert(lanlan_time_format_rfc3339(-1, text, sizeof(text)) == 0);
    assert(lanlan_time_format_rfc3339(1760000000, text, 8) == 0);
    assert(lanlan_time_format_rfc3339(1760000000, NULL, sizeof(text)) == 0);
    assert(lanlan_time_format_rfc3339(LANLAN_TIME_MIN_EPOCH, text, sizeof(text)) == 20);
    assert(strcmp(text, "2001-09-09T01:46:40Z") == 0);

    /* Format and parse are exact inverses over a sample of instants. */
    int64_t samples[] = {LANLAN_TIME_MIN_EPOCH, 951782400 + 1000000000, 19782 * 86400,
                         1760000000, 4102444799};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        char buffer[LANLAN_RFC3339_MAX];
        assert(lanlan_time_format_rfc3339(samples[i], buffer, sizeof(buffer)) > 0);
        int64_t parsed = 0;
        assert(lanlan_time_parse_rfc3339(buffer, &parsed, NULL) == LANLAN_TIME_OK);
        assert(parsed == samples[i]);
    }
}

static void test_hhmm(void) {
    int hour = -1, minute = -1;
    assert(lanlan_time_parse_hhmm("00:00", &hour, &minute));
    assert(hour == 0 && minute == 0);
    assert(lanlan_time_parse_hhmm("23:59", &hour, &minute));
    assert(hour == 23 && minute == 59);
    assert(lanlan_time_parse_hhmm("07:40", NULL, NULL));
    assert(!lanlan_time_parse_hhmm("24:00", &hour, &minute));
    assert(!lanlan_time_parse_hhmm("23:60", &hour, &minute));
    assert(!lanlan_time_parse_hhmm("7:40", &hour, &minute));
    assert(!lanlan_time_parse_hhmm("07:4", &hour, &minute));
    assert(!lanlan_time_parse_hhmm("07-40", &hour, &minute));
    assert(!lanlan_time_parse_hhmm("ab:cd", &hour, &minute));
    assert(!lanlan_time_parse_hhmm(NULL, &hour, &minute));
    assert(!lanlan_time_parse_hhmm("", &hour, &minute));

    char text[6];
    assert(lanlan_time_format_hhmm(7, 40, text, sizeof(text)) == 5);
    assert(strcmp(text, "07:40") == 0);
    assert(lanlan_time_format_hhmm(23, 59, text, sizeof(text)) == 5);
    assert(strcmp(text, "23:59") == 0);
    assert(lanlan_time_format_hhmm(24, 0, text, sizeof(text)) == 0);
    assert(lanlan_time_format_hhmm(0, 0, text, 4) == 0);

    assert(lanlan_time_hhmm_minutes("07:40") == 460);
    assert(lanlan_time_hhmm_minutes("00:00") == 0);
    assert(lanlan_time_hhmm_minutes("23:59") == 1439);
    assert(lanlan_time_hhmm_minutes("bad") == -1);

    assert(lanlan_time_hhmm_compare("07:40", "07:40") == 0);
    assert(lanlan_time_hhmm_compare("07:40", "07:39") == -1);
    assert(lanlan_time_hhmm_compare("07:40", "07:41") == 1);
    /* Early morning reminders are not due at any time on the same day. */
    assert(lanlan_time_hhmm_due("07:40", "07:39") == false);
    assert(lanlan_time_hhmm_due("07:40", "07:40") == true);
    assert(lanlan_time_hhmm_due("07:40", "23:59") == true);
    assert(lanlan_time_hhmm_due("07:40", "00:00") == false);
    assert(lanlan_time_hhmm_due("bad", "00:00") == false);
}

static void test_clock_trust(void) {
    /* Documented floor: 2020-01-01T00:00:00Z. */
    assert(LANLAN_TIME_TRUST_FLOOR_EPOCH == 1577836800);
    assert(lanlan_time_clock_trusted(true, LANLAN_TIME_TRUST_FLOOR_EPOCH));
    assert(lanlan_time_clock_trusted(true, LANLAN_TIME_TRUST_FLOOR_EPOCH + 1));
    assert(!lanlan_time_clock_trusted(true, LANLAN_TIME_TRUST_FLOOR_EPOCH - 1));
    assert(!lanlan_time_clock_trusted(true, 0));
    assert(!lanlan_time_clock_trusted(true, -1));
    /* A plausible time is not enough: the device must have synchronized. */
    assert(!lanlan_time_clock_trusted(false, 1760000000));
    assert(!lanlan_time_clock_trusted(false, LANLAN_TIME_TRUST_FLOOR_EPOCH - 1));
    /* A nonsense future time is also rejected. */
    assert(!lanlan_time_clock_trusted(true, LANLAN_TIME_MAX_EPOCH + 1));

    assert(strcmp(lanlan_time_status_name(LANLAN_TIME_ERR_RANGE), "range") == 0);
    assert(strcmp(lanlan_time_status_name(LANLAN_TIME_OK), "ok") == 0);
}

int main(void) {
    test_civil_round_trip();
    test_floor_division();
    test_local_conversions();
    test_local_to_epoch();
    test_rfc3339_accept();
    test_rfc3339_reject();
    test_rfc3339_format();
    test_hhmm();
    test_clock_trust();
    puts("Lanlan time: PASS (civil dates, offsets, RFC3339, HH:MM, trust floor)");
    return 0;
}

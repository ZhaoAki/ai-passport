/* Host tests for main/lanlan_reminder.c.
 * Build:  cc -std=c11 -Wall -Wextra -Werror -Imain \
 *             tests/test_lanlan_reminder.c main/lanlan_reminder.c main/lanlan_cache.c \
 *             main/lanlan_record.c main/lanlan_time.c main/lanlan_strings.c \
 *             -o /tmp/t && /tmp/t
 * Covers every rule of docs/applications/cyber-lanlan-service.md section 8,
 * including the restart, clock-jump, unmute and untrusted-clock cases. */
#include "lanlan_reminder.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void fill_id(lanlan_reminder_t *reminder, uint8_t tag) {
    for (size_t i = 0; i < LANLAN_ID_BYTES; ++i) reminder->id[i] = (uint8_t)(tag + i);
}

static lanlan_reminder_t make_reminder(uint8_t tag, const char *time_local, bool enabled) {
    lanlan_reminder_t reminder;
    memset(&reminder, 0, sizeof(reminder));
    fill_id(&reminder, tag);
    reminder.version = 1;
    reminder.category = LANLAN_CAT_CARE;
    reminder.subitem = LANLAN_SUB_BATH;
    reminder.enabled = enabled;
    reminder.last_rung_day = LANLAN_REMINDER_NO_RUNG;
    strcpy(reminder.time_local, time_local);
    return reminder;
}

static lanlan_local_now_t at(int64_t day, int hour, int minute) {
    lanlan_local_now_t now = {.local_day = day, .hour = hour, .minute = minute};
    return now;
}

static void test_basic_due_rules(void) {
    /* Disabled: never due, whatever the time is. */
    lanlan_reminder_t reminder = make_reminder(1, "07:40", false);
    lanlan_local_now_t now = at(20370, 8, 0);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_NOT_DUE);

    /* Enabled but no configured time: never due (every reminder starts with no
     * schedule, and the service rejects "enabled without a time"). */
    reminder = make_reminder(1, "", true);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_NOT_DUE);

    /* Enabled with a time, before the time: not due. */
    reminder = make_reminder(1, "07:40", true);
    now = at(20370, 7, 39);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_NOT_DUE);
    /* Exactly at the time and after it: due. */
    now = at(20370, 7, 40);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_DUE);
    now = at(20370, 23, 59);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_DUE);

    /* A second evaluation of the same instance before marking is still due: the
     * caller decides when to ring, the module only suppresses marked instances. */
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_DUE);

    /* Marking the instance suppresses it until the local day changes. */
    assert(lanlan_reminder_mark_rung(&reminder, &now));
    assert(lanlan_reminder_has_rung(&reminder));
    assert(reminder.last_rung_day == 20370);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_NOT_DUE);
    assert(!lanlan_reminder_mark_rung(&reminder, &now)); /* idempotent */
    assert(reminder.last_rung_day == 20370);
    /* The next local day is a new instance and may ring again. */
    lanlan_local_now_t tomorrow = at(20371, 7, 40);
    assert(lanlan_reminder_due(&reminder, &tomorrow, true) == LANLAN_REMINDER_DUE);
    assert(lanlan_reminder_mark_rung(&reminder, &tomorrow));
    assert(reminder.last_rung_day == 20371);

    /* Invalid arguments never produce a ring. */
    assert(lanlan_reminder_due(NULL, &now, true) == LANLAN_REMINDER_NOT_DUE);
    assert(lanlan_reminder_due(&reminder, NULL, true) == LANLAN_REMINDER_NOT_DUE);
    assert(!lanlan_reminder_mark_rung(NULL, &now));
    assert(!lanlan_reminder_mark_rung(&reminder, NULL));
    /* An out-of-range injected hour cannot be formatted: no ring. */
    lanlan_local_now_t bad = at(20370, 99, 0);
    assert(lanlan_reminder_due(&reminder, &bad, true) == LANLAN_REMINDER_NOT_DUE);
    assert(!lanlan_reminder_has_rung(NULL));
}

static void test_untrusted_clock_never_rings(void) {
    lanlan_reminder_t reminder = make_reminder(1, "07:40", true);
    lanlan_local_now_t now = at(20370, 8, 0);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_DUE);
    /* With an unverified clock the device cannot know which instance "now" is,
     * so it never rings - even though the due state is otherwise satisfied. */
    assert(lanlan_reminder_due(&reminder, &now, false) == LANLAN_REMINDER_NOT_DUE);
    assert(!lanlan_reminder_should_ring(&reminder, &now, false, true, false));
    /* Muting never hides the due state, it only suppresses the sound. */
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_DUE);
    assert(!lanlan_reminder_should_ring(&reminder, &now, true, true, true));
    assert(!lanlan_reminder_should_ring(&reminder, &now, true, false, false));
    assert(lanlan_reminder_should_ring(&reminder, &now, true, true, false));
}

static void test_restart_reload_does_not_rering(void) {
    /* The persisted state is what a restart reloads; the instance must stay
     * suppressed, because the ring already happened before the reboot. */
    lanlan_reminder_t reminder = make_reminder(1, "07:40", true);
    lanlan_local_now_t now = at(20370, 8, 0);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_DUE);
    assert(lanlan_reminder_mark_rung(&reminder, &now));

    /* Serialize and reload: a copy is what a restart would see. */
    lanlan_reminder_t reloaded = reminder;
    assert(lanlan_reminder_due(&reloaded, &now, true) == LANLAN_REMINDER_NOT_DUE);
    assert(reloaded.last_rung_day == 20370);

    /* The cache decoder preserves the field: check through a real cache blob. */
    lanlan_cache_t cache;
    lanlan_cache_clear(&cache);
    cache.reminders[0] = reminder;
    cache.reminder_count = 1;
    uint8_t blob[LANLAN_CACHE_BLOB_BYTES];
    assert(lanlan_cache_encode(&cache, blob, sizeof(blob)) == LANLAN_CACHE_BLOB_BYTES);
    lanlan_cache_decode_result_t result;
    assert(lanlan_cache_decode(blob, sizeof(blob), &result) == LANLAN_CACHE_OK);
    assert(result.cache.reminders[0].last_rung_day == 20370);
    assert(lanlan_reminder_due(&result.cache.reminders[0], &now, true)
           == LANLAN_REMINDER_NOT_DUE);
}

static void test_clock_jumps_across_a_date_boundary(void) {
    /* The device persists ONE rung instance per reminder, so the guarantee is
     * exactly "an instance rings at most once": repeating the same local day
     * cannot ring twice; only dates beyond the high-water mark can ring.
     * These cases pin forward progress and rollback suppression. */
    lanlan_reminder_t reminder = make_reminder(1, "07:40", true);
    lanlan_local_now_t now = at(20370, 8, 0);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_DUE);
    assert(lanlan_reminder_mark_rung(&reminder, &now));
    assert(reminder.last_rung_day == 20370);

    /* Same local day, evaluated again (a refresh, a re-poll, a restart): the
     * stored instance matches, so it must not ring again. */
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_NOT_DUE);
    assert(!lanlan_reminder_mark_rung(&reminder, &now));
    lanlan_local_now_t same_day_late = at(20370, 23, 0);
    assert(lanlan_reminder_due(&reminder, &same_day_late, true) == LANLAN_REMINDER_NOT_DUE);

    /* The clock jumps FORWARD over a date boundary: the new local day is a new
     * instance and rings once. */
    lanlan_local_now_t later = at(20372, 8, 0);
    assert(lanlan_reminder_due(&reminder, &later, true) == LANLAN_REMINDER_DUE);
    assert(lanlan_reminder_mark_rung(&reminder, &later));
    assert(reminder.last_rung_day == 20372);

    /* Jumping backwards onto a day that already rang AND is still the stored
     * instance is suppressed: repeating the stored day never rings twice. */
    assert(lanlan_reminder_due(&reminder, &later, true) == LANLAN_REMINDER_NOT_DUE);
    assert(!lanlan_reminder_mark_rung(&reminder, &later));

    /* A day that never rang is a new instance and rings once, which moves the
     * stored instance to that day. */
    lanlan_local_now_t other = at(20375, 8, 0);
    assert(lanlan_reminder_due(&reminder, &other, true) == LANLAN_REMINDER_DUE);
    assert(lanlan_reminder_mark_rung(&reminder, &other));
    assert(reminder.last_rung_day == 20375);
    assert(lanlan_reminder_due(&reminder, &other, true) == LANLAN_REMINDER_NOT_DUE);

    /* Clock rollback keeps the high-water mark, including after a reload. */
    lanlan_local_now_t back = at(20370, 8, 0);
    assert(lanlan_reminder_due(&reminder, &back, true) == LANLAN_REMINDER_NOT_DUE);
    assert(!lanlan_reminder_mark_rung(&reminder, &back));
    assert(reminder.last_rung_day == 20375);
    assert(lanlan_reminder_due(&reminder, &other, true) == LANLAN_REMINDER_NOT_DUE);
    lanlan_local_now_t next = at(20376, 8, 0);
    assert(lanlan_reminder_due(&reminder, &next, true) == LANLAN_REMINDER_DUE);

}

static void test_time_change_on_the_same_day(void) {
    lanlan_reminder_t reminder = make_reminder(1, "07:40", true);
    lanlan_local_now_t now = at(20370, 8, 0);

    /* The instance already rang at 07:40. Moving the time later must not make it
     * ring a second time on the same day. */
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_DUE);
    assert(lanlan_reminder_mark_rung(&reminder, &now));
    assert(lanlan_reminder_apply_time_change(&reminder, "09:00"));
    assert(strcmp(reminder.time_local, "09:00") == 0);
    assert(reminder.last_rung_day == 20370); /* rung instance preserved */
    lanlan_local_now_t nine = at(20370, 9, 30);
    assert(lanlan_reminder_due(&reminder, &nine, true) == LANLAN_REMINDER_NOT_DUE);

    /* Before the instance rang, moving the time forward keeps it eligible for
     * exactly one ring at the new time. */
    lanlan_reminder_t pending = make_reminder(2, "07:40", true);
    lanlan_local_now_t before = at(20370, 7, 0);
    assert(lanlan_reminder_due(&pending, &before, true) == LANLAN_REMINDER_NOT_DUE);
    assert(lanlan_reminder_apply_time_change(&pending, "09:00"));
    assert(lanlan_reminder_due(&pending, &before, true) == LANLAN_REMINDER_NOT_DUE);
    lanlan_local_now_t nine_five = at(20370, 9, 5);
    assert(lanlan_reminder_due(&pending, &nine_five, true) == LANLAN_REMINDER_DUE);
    assert(lanlan_reminder_mark_rung(&pending, &nine_five));
    /* Moving the time backwards afterwards still does not re-arm today. */
    assert(lanlan_reminder_apply_time_change(&pending, "08:00"));
    assert(lanlan_reminder_due(&pending, &nine_five, true) == LANLAN_REMINDER_NOT_DUE);

    /* Invalid or unchanged times are refused and change nothing. */
    lanlan_reminder_t stable = make_reminder(3, "07:40", true);
    assert(!lanlan_reminder_apply_time_change(&stable, "07:40"));
    assert(!lanlan_reminder_apply_time_change(&stable, "24:00"));
    assert(!lanlan_reminder_apply_time_change(&stable, "7:40"));
    assert(!lanlan_reminder_apply_time_change(&stable, "noon"));
    assert(!lanlan_reminder_apply_time_change(NULL, "08:00"));
    assert(strcmp(stable.time_local, "07:40") == 0);
    /* Clearing the time disables the ring without touching the rung state. */
    assert(lanlan_reminder_apply_time_change(&stable, ""));
    assert(strcmp(stable.time_local, "") == 0);
    assert(lanlan_reminder_due(&stable, &nine_five, true) == LANLAN_REMINDER_NOT_DUE);
}

static void test_enable_disable_around_a_rung_instance(void) {
    lanlan_reminder_t reminder = make_reminder(1, "07:40", true);
    lanlan_local_now_t now = at(20370, 8, 0);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_DUE);
    assert(lanlan_reminder_mark_rung(&reminder, &now));

    /* Muting (disable) and unmuting later the same day must not re-arm it. */
    assert(lanlan_reminder_apply_enabled(&reminder, false));
    assert(!reminder.enabled);
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_NOT_DUE);
    assert(lanlan_reminder_apply_enabled(&reminder, true));
    assert(lanlan_reminder_due(&reminder, &now, true) == LANLAN_REMINDER_NOT_DUE);
    assert(reminder.last_rung_day == 20370);

    /* A reminder that was disabled through its due time and enabled after it
     * still rings once for the instance, because nothing rang yet. */
    lanlan_reminder_t late = make_reminder(2, "07:40", false);
    assert(lanlan_reminder_due(&late, &now, true) == LANLAN_REMINDER_NOT_DUE);
    assert(lanlan_reminder_apply_enabled(&late, true));
    assert(lanlan_reminder_due(&late, &now, true) == LANLAN_REMINDER_DUE);

    /* Redundant changes report "no change"; NULL is refused. */
    lanlan_reminder_t stable = make_reminder(3, "07:40", true);
    assert(!lanlan_reminder_apply_enabled(&stable, true));
    assert(lanlan_reminder_apply_enabled(&stable, false));
    assert(!lanlan_reminder_apply_enabled(&stable, false));
    assert(!lanlan_reminder_apply_enabled(NULL, true));
}

static void test_apply_update_preserves_the_rung_instance(void) {
    lanlan_reminder_t target = make_reminder(1, "07:40", true);
    target.last_rung_day = 20370;
    target.version = 3;
    lanlan_reminder_t incoming = make_reminder(1, "08:30", true);
    incoming.version = 4;
    incoming.custom_name[0] = '\0';

    lanlan_reminder_apply_update(&target, &incoming);
    assert(target.version == 4);
    assert(strcmp(target.time_local, "08:30") == 0);
    /* The server has no column for the rung instance, so a refresh must not
     * clear it: clearing would let the same instance ring twice. */
    assert(target.last_rung_day == 20370);

    /* An older revision is ignored. */
    lanlan_reminder_t older = make_reminder(1, "06:00", true);
    older.version = 2;
    lanlan_reminder_apply_update(&target, &older);
    assert(target.version == 4 && strcmp(target.time_local, "08:30") == 0);

    lanlan_reminder_apply_update(NULL, &incoming);
    lanlan_reminder_apply_update(&target, NULL);
}

static void test_cache_wide_due_enumeration(void) {
    lanlan_cache_t cache;
    lanlan_cache_clear(&cache);
    cache.reminders[0] = make_reminder(1, "07:40", true);
    cache.reminders[0].last_rung_day = 20370;
    cache.reminders[1] = make_reminder(2, "08:00", true);
    cache.reminders[2] = make_reminder(3, "09:00", false);
    cache.reminders[3] = make_reminder(4, "", true);
    cache.reminder_count = 4;

    lanlan_local_now_t now = at(20370, 8, 30);
    /* Only reminder 2 is due: 1 already rang, 3 is disabled, 4 has no time. */
    assert(lanlan_reminder_count_due(&cache, &now, true) == 1);
    assert(lanlan_reminder_count_due(&cache, &now, false) == 0);
    assert(lanlan_reminder_count_due(NULL, &now, true) == 0);
    assert(lanlan_reminder_count_due(&cache, NULL, true) == 0);

    /* Moving to the next day makes 1 due again (new instance) and keeps the
     * still-disabled and time-less reminders out. */
    lanlan_local_now_t tomorrow = at(20371, 9, 30);
    assert(lanlan_reminder_count_due(&cache, &tomorrow, true) == 2);
}

int main(void) {
    test_basic_due_rules();
    test_untrusted_clock_never_rings();
    test_restart_reload_does_not_rering();
    test_clock_jumps_across_a_date_boundary();
    test_time_change_on_the_same_day();
    test_enable_disable_around_a_rung_instance();
    test_apply_update_preserves_the_rung_instance();
    test_cache_wide_due_enumeration();
    puts("Lanlan reminder: PASS (due rules, restart, clock jumps, time/enable changes, trust)");
    return 0;
}

/* Cyber Lanlan reminder evaluation implementation. See lanlan_reminder.h. */
#include "lanlan_reminder.h"

#include <string.h>
/* The instance identity is the local day only, stored as the day index so the
 * comparison is integer-exact across month, year and offset boundaries. The
 * device persists exactly ONE instance per reminder, which gives this rule:
 *   - evaluating the stored day again (a refresh, a repeated poll, a restart)
 *     never rings a second time;
 *   - any other day is a new instance and may ring once, which replaces the
 *     stored day.
 * A backwards clock correction therefore cannot re-ring the day it restores if
 * that day is still the stored one, but an older day the device has since
 * forgotten can ring once again. Bounding every instance to a single ring is
 * what the design promises; keeping a full rung history is not. */
static bool instance_already_rung(const lanlan_reminder_t *reminder, int64_t local_day) {
    return reminder->last_rung_day == local_day;
}

lanlan_reminder_due_t lanlan_reminder_due(const lanlan_reminder_t *reminder,
                                           const lanlan_local_now_t *now, bool clock_trusted) {
    if (!reminder || !now) return LANLAN_REMINDER_NOT_DUE;
    if (!reminder->enabled) return LANLAN_REMINDER_NOT_DUE;
    /* Every reminder starts with time_local == "" and no schedule is preset. */
    if (reminder->time_local[0] == '\0') return LANLAN_REMINDER_NOT_DUE;
    /* With an unverified clock the device cannot know which instance "now" is,
     * so it never rings; it still renders the cached reminder and its time. */
    if (!clock_trusted) return LANLAN_REMINDER_NOT_DUE;
    char now_text[6];
    if (lanlan_time_format_hhmm(now->hour, now->minute, now_text, sizeof(now_text)) == 0) {
        return LANLAN_REMINDER_NOT_DUE;
    }
    if (!lanlan_time_hhmm_due(reminder->time_local, now_text)) return LANLAN_REMINDER_NOT_DUE;
    if (instance_already_rung(reminder, now->local_day)) return LANLAN_REMINDER_NOT_DUE;
    return LANLAN_REMINDER_DUE;
}

bool lanlan_reminder_should_ring(const lanlan_reminder_t *reminder,
                                 const lanlan_local_now_t *now, bool clock_trusted,
                                 bool reminder_sound_enabled, bool globally_muted) {
    if (!reminder_sound_enabled || globally_muted) return false;
    return lanlan_reminder_due(reminder, now, clock_trusted) == LANLAN_REMINDER_DUE;
}

int lanlan_reminder_count_due(const lanlan_cache_t *cache, const lanlan_local_now_t *now,
                              bool clock_trusted) {
    if (!cache) return 0;
    int count = 0;
    for (uint32_t i = 0; i < cache->reminder_count && i < LANLAN_CACHE_REMINDER_CAPACITY; ++i) {
        if (lanlan_reminder_due(&cache->reminders[i], now, clock_trusted)
            == LANLAN_REMINDER_DUE) {
            ++count;
        }
    }
    return count;
}

bool lanlan_reminder_mark_rung(lanlan_reminder_t *reminder, const lanlan_local_now_t *now) {
    if (!reminder || !now) return false;
    if (instance_already_rung(reminder, now->local_day)) return false;
    reminder->last_rung_day = now->local_day;
    return true;
}

void lanlan_reminder_apply_update(lanlan_reminder_t *target, const lanlan_reminder_t *incoming) {
    if (!target || !incoming) return;
    /* An older revision must never replace a newer one. An equal version is a
     * replayed batch and keeps the state the device already has. */
    if (incoming->version < target->version) return;
    int64_t preserved_rung = target->last_rung_day;
    *target = *incoming;
    target->last_rung_day = preserved_rung;
}

bool lanlan_reminder_apply_time_change(lanlan_reminder_t *reminder, const char *new_time_local) {
    if (!reminder) return false;
    const char *candidate = new_time_local ? new_time_local : "";
    if (candidate[0] != '\0' && !lanlan_time_parse_hhmm(candidate, NULL, NULL)) return false;
    if (strcmp(reminder->time_local, candidate) == 0) return false;
    /* The rung instance is preserved on purpose: see the header contract. */
    size_t length = strlen(candidate);
    if (length > sizeof(reminder->time_local) - 1) return false;
    memcpy(reminder->time_local, candidate, length + 1);
    return true;
}

bool lanlan_reminder_apply_enabled(lanlan_reminder_t *reminder, bool enabled) {
    if (!reminder || reminder->enabled == enabled) return false;
    reminder->enabled = enabled;
    return true;
}

bool lanlan_reminder_has_rung(const lanlan_reminder_t *reminder) {
    return reminder && reminder->last_rung_day != LANLAN_REMINDER_NO_RUNG;
}

/* Cyber Lanlan reminder due evaluation and rung suppression.
 *
 * Rules (docs/applications/cyber-lanlan-service.md section 8 and
 * docs/applications/cyber-lanlan.md section 5):
 *   - an instance is (reminder id, local date);
 *   - a reminder is due only when it is enabled, a time is configured, the
 *     clock is trusted, the local time is at or past the configured time, and
 *     the current local day is later than the persisted last-rung day;
 *   - the persisted instance survives restarts and refreshes, so the stored
 *     local day never rings twice;
 *   - a backwards clock correction never re-arms an earlier day;
 *   - changing the time on the same day never rings the same instance twice;
 *   - muting or untrusting the clock hides nothing visually but never rings.
 *
 * Everything is pure: "now" and the clock-trust flag are injected by the
 * caller, so this module has no timer, no RTC and no ESP-IDF dependency. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lanlan_cache.h"
#include "lanlan_time.h"

typedef struct {
    int64_t local_day;   /* local day index; see lanlan_time_local_date() */
    int hour;            /* 0..23 */
    int minute;          /* 0..59 */
} lanlan_local_now_t;

typedef enum {
    LANLAN_REMINDER_NOT_DUE = 0,
    LANLAN_REMINDER_DUE
} lanlan_reminder_due_t;

/* Classifies one instance. `muted` suppresses the audible ring but never the
 * visual due state; callers pass it to lanlan_reminder_should_ring(). */
lanlan_reminder_due_t lanlan_reminder_due(const lanlan_reminder_t *reminder,
                                           const lanlan_local_now_t *now, bool clock_trusted);
/* True when the due reminder should also play its sound: due, not globally
 * muted and reminder audio not explicitly disabled by the caller's flag.
 * Muting never changes the returned due state. */
bool lanlan_reminder_should_ring(const lanlan_reminder_t *reminder,
                                 const lanlan_local_now_t *now, bool clock_trusted,
                                 bool reminder_sound_enabled, bool globally_muted);
/* Number of due reminders in the cache; used by the settings/status view. */
int lanlan_reminder_count_due(const lanlan_cache_t *cache, const lanlan_local_now_t *now,
                              bool clock_trusted);

/* --------------------------------------------------- state transitions -- */

/* Marks the instance (id, now->local_day) as rung. Returns true when the state
 * changed; a second call for the same instance is a no-op, which is what makes
 * a restart, a refresh or a repeated poll idempotent. */
bool lanlan_reminder_mark_rung(lanlan_reminder_t *reminder, const lanlan_local_now_t *now);
/* Applies a server reminder revision while preserving the locally persisted
 * rung instance: the server has no column for it, and losing it would let a
 * refresh ring the same instance again. */
void lanlan_reminder_apply_update(lanlan_reminder_t *target, const lanlan_reminder_t *incoming);
/* Applies a local time change. The rung instance is deliberately preserved, so
 * a change made after the instance already rang cannot ring it a second time;
 * a change made before the instance rang keeps it eligible for one ring at the
 * new time. Returns true when the stored time changed. */
bool lanlan_reminder_apply_time_change(lanlan_reminder_t *reminder, const char *new_time_local);
/* Applies an enable/disable change. Disabling and re-enabling around an
 * already-rung instance does not re-arm it; only a new local day does. */
bool lanlan_reminder_apply_enabled(lanlan_reminder_t *reminder, bool enabled);
/* True when the stored rung day is a plausible local day (not the sentinel). */
bool lanlan_reminder_has_rung(const lanlan_reminder_t *reminder);

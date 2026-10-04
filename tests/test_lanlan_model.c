/* Host tests for main/lanlan_model.c.
 * Build:  cc -std=c11 -Wall -Wextra -Werror -Imain \
 *             tests/test_lanlan_model.c main/lanlan_model.c main/lanlan_cache.c \
 *             main/lanlan_record.c main/lanlan_reminder.c main/lanlan_time.c \
 *             main/lanlan_strings.c -o /tmp/t && /tmp/t
 * Covers the navigation bounds, the wake-only first gesture, the long-press
 * wake, the companion "never a record action" invariant and the sync view. */
#include "lanlan_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static lanlan_model_t s_model;
static lanlan_records_view_t s_view;

static lanlan_action_t press(lanlan_model_t *model, lanlan_key_t key) {
    lanlan_key_event_t event;
    memset(&event, 0, sizeof(event));
    event.key = key;
    event.woke_screen = !model->active;
    /* Every gesture in these tests happens with the screen on unless the test
     * explicitly turns it off first. */
    return lanlan_model_handle_key(model, &event);
}

static lanlan_record_t make_record(uint8_t tag, int64_t occurred, lanlan_category_t category,
                                   lanlan_subitem_t subitem) {
    lanlan_record_t record;
    memset(&record, 0, sizeof(record));
    for (size_t i = 0; i < LANLAN_ID_BYTES; ++i) record.id[i] = (uint8_t)(tag + i + 1);
    record.version = 1;
    record.category = category;
    record.subitem = subitem;
    record.time_confidence = LANLAN_TIME_CONFIDENCE_TRUSTED;
    record.occurred_epoch = occurred;
    record.occurred_tz_offset_min = 480;
    record.created_epoch = occurred + 10;
    record.status = LANLAN_STATUS_ACTIVE;
    record.seq = tag;
    lanlan_record_set_amount_unknown(&record);
    return record;
}

static void load_records(lanlan_model_t *model, int count) {
    lanlan_cache_t cache;
    lanlan_cache_clear(&cache);
    for (int i = 0; i < count && i < LANLAN_CACHE_MAX_RECORDS; ++i) {
        /* Newest first once sorted. */
        cache.records[i] = make_record((uint8_t)(i + 1), 1760000000 + (count - i),
                                       LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    }
    cache.record_count = (uint32_t)count;
    lanlan_cache_sort_records(&cache, NULL);
    lanlan_model_load_cache(model, &cache);
}

static void test_home_navigation_and_entries(void) {
    lanlan_model_init(&s_model);
    assert(s_model.page == LANLAN_PAGE_HOME);
    assert(s_model.home_entry == LANLAN_HOME_ENTRY_TODAY);

    /* The four entries cycle in both directions and wrap. */
    for (int i = 0; i < LANLAN_HOME_ENTRY_COUNT; ++i) {
        assert(s_model.home_entry == (lanlan_home_entry_t)i);
        press(&s_model, LANLAN_KEY_DOWN);
    }
    assert(s_model.home_entry == LANLAN_HOME_ENTRY_TODAY);
    press(&s_model, LANLAN_KEY_UP);
    assert(s_model.home_entry == LANLAN_HOME_ENTRY_SETTINGS);

    /* Home's long press is deliberately unassigned. */
    assert(press(&s_model, LANLAN_KEY_OK_LONG) == LANLAN_ACTION_NONE);
    assert(s_model.page == LANLAN_PAGE_HOME);

    /* Entering each entry. */
    s_model.home_entry = LANLAN_HOME_ENTRY_COMPANION;
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_NONE);
    assert(s_model.page == LANLAN_PAGE_COMPANION);
    press(&s_model, LANLAN_KEY_OK_LONG);
    assert(s_model.page == LANLAN_PAGE_HOME);

    s_model.home_entry = LANLAN_HOME_ENTRY_SETTINGS;
    press(&s_model, LANLAN_KEY_OK_CLICK);
    assert(s_model.page == LANLAN_PAGE_SETTINGS);
    press(&s_model, LANLAN_KEY_OK_LONG);
    assert(s_model.page == LANLAN_PAGE_HOME);

    s_model.home_entry = LANLAN_HOME_ENTRY_RECORDS;
    press(&s_model, LANLAN_KEY_OK_CLICK);
    assert(s_model.page == LANLAN_PAGE_RECORDS);

    /* NULL and unknown pages never crash. */
    assert(lanlan_model_handle_key(NULL, NULL) == LANLAN_ACTION_NONE);
    lanlan_model_set_page(&s_model, (lanlan_page_t)99);
    assert(s_model.page == LANLAN_PAGE_RECORDS);
    lanlan_model_set_page(NULL, LANLAN_PAGE_HOME);
    lanlan_model_init(NULL);
    lanlan_model_clear(NULL);
    lanlan_model_load_cache(&s_model, NULL);
    lanlan_model_set_clock(&s_model, NULL, false);
    lanlan_model_set_active(NULL, true);
}

static void test_records_navigation_bounds(void) {
    lanlan_model_init(&s_model);
    load_records(&s_model, 12);
    s_model.page = LANLAN_PAGE_RECORDS;
    s_model.record_index = 0;
    s_model.list_offset = 0;

    /* DOWN moves forward and wraps at the end. */
    for (int i = 0; i < 12; ++i) {
        assert(s_model.record_index == i);
        press(&s_model, LANLAN_KEY_DOWN);
    }
    assert(s_model.record_index == 0);
    press(&s_model, LANLAN_KEY_UP);
    assert(s_model.record_index == 11);
    /* The window stayed inside the list and inside its bounds. */
    assert(s_model.list_offset >= 0);
    assert(s_model.list_offset + LANLAN_MODEL_LIST_ROWS <= 12);

    /* An empty cache keeps the selection at zero and does not move it. */
    lanlan_cache_t empty;
    lanlan_cache_clear(&empty);
    lanlan_model_load_cache(&s_model, &empty);
    s_model.page = LANLAN_PAGE_RECORDS;
    assert(press(&s_model, LANLAN_KEY_DOWN) == LANLAN_ACTION_NONE);
    assert(s_model.record_index == 0 && s_model.list_offset == 0);
    /* OK on an empty list opens nothing. */
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_NONE);
    assert(s_model.page == LANLAN_PAGE_RECORDS);
    /* Long press still returns home. */
    assert(press(&s_model, LANLAN_KEY_OK_LONG) == LANLAN_ACTION_BACK_HOME);
    assert(s_model.page == LANLAN_PAGE_HOME);
}

static void test_detail_toggles_only_the_view(void) {
    lanlan_model_init(&s_model);
    load_records(&s_model, 3);
    s_model.page = LANLAN_PAGE_RECORDS;
    s_model.record_index = 1;
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_RECORD_OPEN);
    assert(s_model.page == LANLAN_PAGE_DETAIL);
    assert(s_model.detail_full == 0);

    /* UP/DOWN change the record under the detail page without leaving it. */
    assert(press(&s_model, LANLAN_KEY_DOWN) == LANLAN_ACTION_NONE);
    assert(s_model.record_index == 2 && s_model.page == LANLAN_PAGE_DETAIL);
    assert(press(&s_model, LANLAN_KEY_UP) == LANLAN_ACTION_NONE);
    assert(s_model.record_index == 1);

    /* OK only switches presentation: no record action is ever produced. */
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_DETAIL_TOGGLE_VIEW);
    assert(s_model.detail_full == 1 && s_model.page == LANLAN_PAGE_DETAIL);
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_DETAIL_TOGGLE_VIEW);
    assert(s_model.detail_full == 0);
    assert(!lanlan_model_pending_record_action(&s_model));

    /* Long press goes back to the list, not home. */
    assert(press(&s_model, LANLAN_KEY_OK_LONG) == LANLAN_ACTION_BACK_HOME);
    assert(s_model.page == LANLAN_PAGE_RECORDS);
    assert(press(&s_model, LANLAN_KEY_OK_LONG) == LANLAN_ACTION_BACK_HOME);
    assert(s_model.page == LANLAN_PAGE_HOME);
}

static void test_settings_rows(void) {
    lanlan_model_init(&s_model);
    s_model.page = LANLAN_PAGE_SETTINGS;
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_REFRESH);

    /* The list order is the documented one: actions and toggles, then the two
     * timeout rows, then the read-only information rows. */
    assert(LANLAN_SETTINGS_ROW_REFRESH == 0);
    assert(LANLAN_SETTINGS_ROW_MUTE == 1);
    assert(LANLAN_SETTINGS_ROW_REMINDER_SOUND == 2);
    assert(LANLAN_SETTINGS_ROW_REMINDER_LIST == 3);
    assert(LANLAN_SETTINGS_ROW_DIM == 4);
    assert(LANLAN_SETTINGS_ROW_SCREEN_OFF == 5);
    assert(LANLAN_SETTINGS_ROW_TIMEZONE == 6);
    assert(LANLAN_SETTINGS_ROW_SYNC == 7);
    assert(LANLAN_SETTINGS_ROW_STORAGE == 8);
    assert(LANLAN_SETTINGS_ROW_COUNT == 9);

    /* Rows move one at a time. DOWN on a timeout row changes that row's value
     * instead of moving (its own test covers that), so navigation is walked in
     * the three sections the list has. */
    for (int i = LANLAN_SETTINGS_ROW_REFRESH; i < LANLAN_SETTINGS_ROW_DIM; ++i) {
        assert((int)s_model.settings_row == i);
        press(&s_model, LANLAN_KEY_DOWN);
    }
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_DIM);
    /* Each gesture on a timeout row steps that row's value and keeps the
     * selection, so the list does not scroll away while a value is adjusted.
     * The default dim is 30 s; DOWN/UP walk the allowed steps and clamp. */
    assert(s_model.dim_seconds == LANLAN_TIMEOUT_DIM_DEFAULT_SECONDS);
    press(&s_model, LANLAN_KEY_DOWN);
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_DIM);
    assert(s_model.dim_seconds == 60);
    assert(s_model.screen_off_seconds == 90);   /* repaired above the new dim */
    press(&s_model, LANLAN_KEY_UP);
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_DIM);
    assert(s_model.dim_seconds == 30);
    assert(s_model.screen_off_seconds == 90);
    press(&s_model, LANLAN_KEY_UP);
    assert(s_model.dim_seconds == 15);
    press(&s_model, LANLAN_KEY_UP);             /* clamped at the lowest step */
    assert(s_model.dim_seconds == 15);
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_DIM);
    press(&s_model, LANLAN_KEY_DOWN);
    assert(s_model.dim_seconds == 30);
    assert(s_model.screen_off_seconds > s_model.dim_seconds);

    /* On the screen-off row the same gestures step its value. */
    s_model.settings_row = LANLAN_SETTINGS_ROW_SCREEN_OFF;
    const int screen_off_now = s_model.screen_off_seconds;
    press(&s_model, LANLAN_KEY_DOWN);
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_SCREEN_OFF);
    assert(s_model.screen_off_seconds > screen_off_now);
    press(&s_model, LANLAN_KEY_UP);
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_SCREEN_OFF);
    assert(s_model.screen_off_seconds == screen_off_now);

    /* The read-only rows still move one at a time and the list wraps. */
    s_model.settings_row = LANLAN_SETTINGS_ROW_TIMEZONE;
    for (int i = LANLAN_SETTINGS_ROW_TIMEZONE; i < LANLAN_SETTINGS_ROW_COUNT; ++i) {
        assert((int)s_model.settings_row == i);
        press(&s_model, LANLAN_KEY_DOWN);
    }
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_REFRESH);
    press(&s_model, LANLAN_KEY_UP);
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_STORAGE);
    press(&s_model, LANLAN_KEY_DOWN);
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_REFRESH);

    /* Refresh asks for a sync; the toggles flip in place. */
    s_model.settings_row = LANLAN_SETTINGS_ROW_REFRESH;
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_SETTINGS_REFRESH);
    s_model.settings_row = LANLAN_SETTINGS_ROW_MUTE;
    assert(!s_model.globally_muted);
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_NONE);
    assert(s_model.globally_muted);
    press(&s_model, LANLAN_KEY_OK_CLICK);
    assert(!s_model.globally_muted);
    s_model.settings_row = LANLAN_SETTINGS_ROW_REMINDER_SOUND;
    assert(!s_model.reminder_sound_enabled);
    press(&s_model, LANLAN_KEY_OK_CLICK);
    assert(s_model.reminder_sound_enabled);

    /* The reminder list row navigates and the read-only rows do nothing. */
    s_model.settings_row = LANLAN_SETTINGS_ROW_REMINDER_LIST;
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_SETTINGS_REMINDER_LIST);
    assert(s_model.page == LANLAN_PAGE_REMINDERS);
    assert(press(&s_model, LANLAN_KEY_OK_LONG) == LANLAN_ACTION_BACK_HOME);
    assert(s_model.page == LANLAN_PAGE_SETTINGS);
    /* The timeout rows sit with the toggles, before the read-only rows. */
    assert(LANLAN_SETTINGS_ROW_DIM > LANLAN_SETTINGS_ROW_REMINDER_LIST);
    assert(LANLAN_SETTINGS_ROW_SCREEN_OFF == LANLAN_SETTINGS_ROW_DIM + 1);
    assert(LANLAN_SETTINGS_ROW_SCREEN_OFF < LANLAN_SETTINGS_ROW_TIMEZONE);
    for (int row = LANLAN_SETTINGS_ROW_TIMEZONE; row <= LANLAN_SETTINGS_ROW_STORAGE; ++row) {
        s_model.settings_row = (lanlan_settings_row_t)row;
        assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_NONE);
        assert(s_model.page == LANLAN_PAGE_SETTINGS);
    }
    assert(press(&s_model, LANLAN_KEY_OK_LONG) == LANLAN_ACTION_BACK_HOME);
    assert(s_model.page == LANLAN_PAGE_HOME);
}

static void test_reminder_page(void) {
    lanlan_model_init(&s_model);
    lanlan_cache_t cache;
    lanlan_cache_clear(&cache);
    for (int i = 0; i < LANLAN_CACHE_MAX_REMINDERS; ++i) {
        memset(&cache.reminders[i], 0, sizeof(cache.reminders[i]));
        for (size_t b = 0; b < LANLAN_ID_BYTES; ++b) {
            cache.reminders[i].id[b] = (uint8_t)(i * 16 + b + 1);
        }
        cache.reminders[i].version = 1;
        cache.reminders[i].category = LANLAN_CAT_CARE;
        cache.reminders[i].subitem = LANLAN_SUB_BATH;
        cache.reminders[i].last_rung_day = LANLAN_REMINDER_NO_RUNG;
        strcpy(cache.reminders[i].time_local, "07:40");
    }
    cache.reminder_count = LANLAN_CACHE_MAX_REMINDERS;
    lanlan_model_load_cache(&s_model, &cache);
    s_model.page = LANLAN_PAGE_REMINDERS;

    for (int i = 0; i < LANLAN_CACHE_MAX_REMINDERS; ++i) {
        assert(s_model.reminder_index == i);
        press(&s_model, LANLAN_KEY_DOWN);
    }
    assert(s_model.reminder_index == 0);
    press(&s_model, LANLAN_KEY_UP);
    assert(s_model.reminder_index == LANLAN_CACHE_MAX_REMINDERS - 1);
    assert(s_model.reminder_offset >= 0);
    assert(s_model.reminder_offset + LANLAN_MODEL_LIST_ROWS <= LANLAN_CACHE_MAX_REMINDERS);

    /* OK does nothing here: dismissing is not completion. */
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_NONE);
    assert(s_model.page == LANLAN_PAGE_REMINDERS);
    assert(press(&s_model, LANLAN_KEY_OK_LONG) == LANLAN_ACTION_BACK_HOME);
    assert(s_model.page == LANLAN_PAGE_SETTINGS);
}

static void test_status_page(void) {
    lanlan_model_init(&s_model);
    s_model.page = LANLAN_PAGE_STATUS;
    assert(press(&s_model, LANLAN_KEY_UP) == LANLAN_ACTION_NONE);
    assert(press(&s_model, LANLAN_KEY_DOWN) == LANLAN_ACTION_NONE);
    assert(press(&s_model, LANLAN_KEY_OK_CLICK) == LANLAN_ACTION_STATUS_RETRY);
    assert(press(&s_model, LANLAN_KEY_OK_LONG) == LANLAN_ACTION_BACK_HOME);
    assert(s_model.page == LANLAN_PAGE_HOME);
}

/* A whole-struct snapshot is the strongest way to prove the wake gesture
 * changed nothing; it also covers every field a future edit might add. */
static lanlan_model_t snapshot(const lanlan_model_t *model) { return *model; }

static void test_wake_only_first_gesture(void) {
    lanlan_model_init(&s_model);
    load_records(&s_model, 3);

    const lanlan_key_t keys[] = {LANLAN_KEY_UP, LANLAN_KEY_DOWN, LANLAN_KEY_OK_CLICK,
                                 LANLAN_KEY_OK_LONG};
    const lanlan_page_t pages[] = {LANLAN_PAGE_HOME, LANLAN_PAGE_RECORDS, LANLAN_PAGE_DETAIL,
                                   LANLAN_PAGE_COMPANION, LANLAN_PAGE_SETTINGS,
                                   LANLAN_PAGE_REMINDERS, LANLAN_PAGE_STATUS};

    /* Every key on every page, with the screen off: the gesture is consumed
     * entirely and only turns the display on. */
    for (size_t p = 0; p < sizeof(pages) / sizeof(pages[0]); ++p) {
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
            lanlan_model_init(&s_model);
            load_records(&s_model, 3);
            s_model.page = pages[p];
            if (s_model.page == LANLAN_PAGE_RECORDS) s_model.record_index = 1;
            if (s_model.page == LANLAN_PAGE_COMPANION) s_model.character = LANLAN_CHARACTER_BLINK;
            lanlan_model_set_active(&s_model, false);

            lanlan_model_t before = snapshot(&s_model);
            lanlan_key_event_t event = {.key = keys[k], .woke_screen = true};
            assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
            assert(s_model.active);
            /* Nothing changed except the active flag. */
            before.active = true;
            assert(memcmp(&before, &s_model, sizeof(before)) == 0);
            assert(s_model.page == pages[p]);
            assert(!lanlan_model_pending_record_action(&s_model));

            /* The same gesture again (display now on) is delivered normally: it
             * must not be swallowed by a second wake. */
            lanlan_action_t second = lanlan_model_handle_key(&s_model, &event);
            assert(second != LANLAN_ACTION_WAKE_ONLY);
            if (keys[k] == LANLAN_KEY_OK_LONG) {
                /* Long press backs out of every page; on home it is unassigned. */
                assert(second == (pages[p] == LANLAN_PAGE_HOME ? LANLAN_ACTION_NONE
                                                               : LANLAN_ACTION_BACK_HOME));
            }
        }
    }

    /* The concrete cases the rule exists for: the waking gesture must not move
     * the selection, open a record, toggle a setting or pet the character. */
    lanlan_model_init(&s_model);
    load_records(&s_model, 3);
    s_model.page = LANLAN_PAGE_HOME;
    s_model.home_entry = LANLAN_HOME_ENTRY_RECORDS;
    lanlan_model_set_active(&s_model, false);
    lanlan_key_event_t event = {.key = LANLAN_KEY_OK_CLICK, .woke_screen = true};
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
    assert(s_model.page == LANLAN_PAGE_HOME);
    assert(s_model.home_entry == LANLAN_HOME_ENTRY_RECORDS);
    /* The next click opens the selected entry normally. */
    event.woke_screen = false;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_NONE);
    assert(s_model.page == LANLAN_PAGE_RECORDS);

    /* A records-page DOWN must not move the highlight on the waking press. */
    s_model.page = LANLAN_PAGE_RECORDS;
    s_model.record_index = 1;
    lanlan_model_set_active(&s_model, false);
    event.key = LANLAN_KEY_DOWN;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
    assert(s_model.record_index == 1);
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_NONE);
    assert(s_model.record_index == 2);

    /* Settings: the waking gesture must not toggle or step anything. */
    lanlan_model_init(&s_model);
    s_model.page = LANLAN_PAGE_SETTINGS;
    s_model.settings_row = LANLAN_SETTINGS_ROW_MUTE;
    lanlan_model_set_active(&s_model, false);
    event.key = LANLAN_KEY_OK_CLICK;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
    assert(!s_model.globally_muted);
    s_model.settings_row = LANLAN_SETTINGS_ROW_DIM;
    s_model.page = LANLAN_PAGE_SETTINGS;
    lanlan_model_set_active(&s_model, false);
    int dim = s_model.dim_seconds;
    event.key = LANLAN_KEY_DOWN;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
    assert(s_model.dim_seconds == dim);
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_DIM);

    /* Companion: the waking OK must not pet the character. */
    lanlan_model_init(&s_model);
    s_model.page = LANLAN_PAGE_COMPANION;
    s_model.character = LANLAN_CHARACTER_IDLE;
    lanlan_model_set_active(&s_model, false);
    event.key = LANLAN_KEY_OK_CLICK;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
    assert(s_model.companion_mood == 0);
    assert(s_model.character == LANLAN_CHARACTER_IDLE);
    assert(!lanlan_model_pending_record_action(&s_model));
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_COMPANION_PET);

    /* A dimmed display is still ON (active == true), so the first gesture is
     * delivered normally and is never consumed. */
    lanlan_model_init(&s_model);
    load_records(&s_model, 3);
    s_model.page = LANLAN_PAGE_RECORDS;
    s_model.record_index = 0;
    lanlan_model_set_active(&s_model, true); /* dimmed, not off */
    event.key = LANLAN_KEY_DOWN;
    event.woke_screen = false;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_NONE);
    assert(s_model.record_index == 1);
    /* Even a caller that wrongly reports a wake flag cannot suppress a gesture
     * while the model is active: the model owns the rule. */
    event.woke_screen = true;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_NONE);
    assert(s_model.record_index == 2);
    event.key = LANLAN_KEY_OK_CLICK;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_RECORD_OPEN);
    assert(s_model.page == LANLAN_PAGE_DETAIL);

    /* set_active(false) re-arms the rule for the next gesture, and set_active
     * back to true disarms it. */
    lanlan_model_set_active(&s_model, false);
    event.key = LANLAN_KEY_UP;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
    assert(s_model.active);
    lanlan_model_set_active(&s_model, false);
    assert(!s_model.active);
    lanlan_model_set_active(&s_model, true);
    event.key = LANLAN_KEY_UP;
    assert(lanlan_model_handle_key(&s_model, &event) != LANLAN_ACTION_WAKE_ONLY);
}

static void test_companion_never_emits_a_record_action(void) {
    lanlan_model_init(&s_model);
    s_model.page = LANLAN_PAGE_COMPANION;
    s_model.character = LANLAN_CHARACTER_IDLE;

    /* Frame cycling stays inside the state set. */
    for (int i = 0; i < LANLAN_CHARACTER_COUNT; ++i) {
        assert((int)s_model.character == i);
        assert(press(&s_model, LANLAN_KEY_DOWN) == LANLAN_ACTION_NONE);
    }
    assert(s_model.character == LANLAN_CHARACTER_IDLE);
    press(&s_model, LANLAN_KEY_UP);
    assert(s_model.character == LANLAN_CHARACTER_HAPPY);

    /* OK pets the character: one dedicated action, never a record action. */
    lanlan_action_t action = press(&s_model, LANLAN_KEY_OK_CLICK);
    assert(action == LANLAN_ACTION_COMPANION_PET);
    assert(!lanlan_action_is_record_write(action));
    assert(!lanlan_model_pending_record_action(&s_model));
    assert(s_model.companion_mood == 1);
    assert(s_model.character == LANLAN_CHARACTER_HAPPY);

    /* The invariant holds for every page and every key, including the ones the
     * enum reserves for record writes. */
    const lanlan_page_t pages[] = {LANLAN_PAGE_HOME, LANLAN_PAGE_RECORDS, LANLAN_PAGE_DETAIL,
                                   LANLAN_PAGE_COMPANION, LANLAN_PAGE_SETTINGS,
                                   LANLAN_PAGE_REMINDERS, LANLAN_PAGE_STATUS};
    const lanlan_key_t keys[] = {LANLAN_KEY_UP, LANLAN_KEY_DOWN, LANLAN_KEY_OK_CLICK,
                                 LANLAN_KEY_OK_LONG};
    load_records(&s_model, 3);
    for (size_t p = 0; p < sizeof(pages) / sizeof(pages[0]); ++p) {
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
            s_model.page = pages[p];
            lanlan_model_set_active(&s_model, true);
            action = press(&s_model, keys[k]);
            assert(!lanlan_action_is_record_write(action));
            assert(!lanlan_model_pending_record_action(&s_model));
        }
    }
    /* A NULL model still reports the safe answer. */
    assert(!lanlan_model_pending_record_action(NULL));
    assert(lanlan_action_is_record_write(LANLAN_ACTION_RECORD_CREATE));
    assert(lanlan_action_is_record_write(LANLAN_ACTION_RECORD_REVOKE));
    assert(!lanlan_action_is_record_write(LANLAN_ACTION_COMPANION_PET));
    assert(!lanlan_action_is_record_write(LANLAN_ACTION_RECORD_OPEN));
}

/* The two timeout rows exist at the documented positions and expose exact
 * labels plus one value text per allowed step. */
static void test_settings_timeout_rows(void) {
    lanlan_model_init(&s_model);
    assert(LANLAN_SETTINGS_ROW_COUNT == 9);
    assert(LANLAN_SETTINGS_ROW_DIM == 4);
    assert(LANLAN_SETTINGS_ROW_SCREEN_OFF == 5);
    assert(strcmp(lanlan_view_settings_row_label(LANLAN_SETTINGS_ROW_DIM),
                  LANLAN_STR_SETTINGS_ROW_DIM)
           == 0);
    assert(strcmp(lanlan_view_settings_row_label(LANLAN_SETTINGS_ROW_SCREEN_OFF),
                  LANLAN_STR_SETTINGS_ROW_SCREEN_OFF)
           == 0);
    assert(strcmp(LANLAN_STR_SETTINGS_ROW_DIM, "调暗") == 0);
    assert(strcmp(LANLAN_STR_SETTINGS_ROW_SCREEN_OFF, "熄屏") == 0);

    /* Documented allowed steps, ascending and exact. */
    const int dims[LANLAN_DIM_STEP_COUNT] = {15, 30, 60, 120};
    const int offs[LANLAN_SCREEN_OFF_STEP_COUNT] = {60, 90, 180, 300};
    for (int i = 0; i < LANLAN_DIM_STEP_COUNT; ++i) {
        assert(LANLAN_DIM_STEPS_SECONDS[i] == dims[i]);
        assert(lanlan_model_dim_step_index(dims[i]) == i);
        assert(strcmp(lanlan_view_dim_text(dims[i]),
                      i == 0 ? LANLAN_STR_SETTINGS_VALUE_DIM_15S
                             : (i == 1 ? LANLAN_STR_SETTINGS_VALUE_DIM_30S
                                       : (i == 2 ? LANLAN_STR_SETTINGS_VALUE_DIM_60S
                                                 : LANLAN_STR_SETTINGS_VALUE_DIM_120S)))
               == 0);
    }
    for (int i = 0; i < LANLAN_SCREEN_OFF_STEP_COUNT; ++i) {
        assert(LANLAN_SCREEN_OFF_STEPS_SECONDS[i] == offs[i]);
        assert(lanlan_model_screen_off_step_index(offs[i]) == i);
        assert(strcmp(lanlan_view_screen_off_text(offs[i]),
                      i == 0 ? LANLAN_STR_SETTINGS_VALUE_SCREEN_OFF_60S
                             : (i == 1 ? LANLAN_STR_SETTINGS_VALUE_SCREEN_OFF_90S
                                       : (i == 2 ? LANLAN_STR_SETTINGS_VALUE_SCREEN_OFF_180S
                                                 : LANLAN_STR_SETTINGS_VALUE_SCREEN_OFF_300S)))
               == 0);
    }
    /* The rendered value text uses the minute form where the step is whole
     * minutes, and the UI never has to format anything itself. */
    assert(strcmp(lanlan_view_dim_text(60), "1 分钟") == 0);
    assert(strcmp(lanlan_view_dim_text(120), "2 分钟") == 0);
    assert(strcmp(lanlan_view_screen_off_text(180), "3 分钟") == 0);
    assert(strcmp(lanlan_view_screen_off_text(300), "5 分钟") == 0);
    assert(strcmp(lanlan_view_dim_text(15), "15 秒") == 0);
    assert(strcmp(lanlan_view_screen_off_text(90), "90 秒") == 0);
    /* The shared values read per row: 60 is "1 分钟" as a dim step and "60 秒"
     * as a screen-off step, and 120 is "2 分钟" against "not a screen-off step". */
    assert(strcmp(lanlan_view_dim_text(60), "1 分钟") == 0);
    assert(strcmp(lanlan_view_screen_off_text(60), "60 秒") == 0);
    assert(strcmp(lanlan_view_dim_text(120), "2 分钟") == 0);
    assert(lanlan_view_screen_off_text(120)[0] == '\0');
    assert(strcmp(lanlan_view_screen_off_text(15), "15 秒") != 0);
    assert(lanlan_view_screen_off_text(15)[0] == '\0');
    /* A value that is not an allowed step has no selectable text. */
    assert(lanlan_model_dim_step_index(45) == -1);
    assert(lanlan_model_screen_off_step_index(45) == -1);
    assert(lanlan_view_timeout_text(45)[0] == '\0');
    assert(lanlan_view_timeout_text(0)[0] == '\0');
    assert(lanlan_view_timeout_text(-5)[0] == '\0');
    assert(lanlan_view_dim_text(45)[0] == '\0');
    assert(lanlan_view_screen_off_text(45)[0] == '\0');
    /* The row helper picks the right table for each row. */
    assert(strcmp(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_DIM), "30 秒")
           == 0);
    assert(strcmp(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_SCREEN_OFF),
                  "90 秒")
           == 0);
    s_model.dim_seconds = 60;
    s_model.screen_off_seconds = 90;
    assert(strcmp(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_DIM), "1 分钟")
           == 0);
    assert(strcmp(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_SCREEN_OFF),
                  "90 秒")
           == 0);
    s_model.dim_seconds = LANLAN_TIMEOUT_DIM_DEFAULT_SECONDS;
    s_model.screen_off_seconds = LANLAN_TIMEOUT_SCREEN_OFF_DEFAULT_SECONDS;

    /* Defaults satisfy the invariant and render through the row helper. */
    assert(s_model.dim_seconds == LANLAN_TIMEOUT_DIM_DEFAULT_SECONDS);
    assert(s_model.screen_off_seconds == LANLAN_TIMEOUT_SCREEN_OFF_DEFAULT_SECONDS);
    assert(lanlan_model_timeouts_valid(s_model.dim_seconds, s_model.screen_off_seconds));
    assert(strcmp(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_DIM),
                  LANLAN_STR_SETTINGS_VALUE_DIM_30S)
           == 0);
    assert(strcmp(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_SCREEN_OFF),
                  LANLAN_STR_SETTINGS_VALUE_SCREEN_OFF_90S)
           == 0);
    /* Non-timeout rows keep their own values, and rows without one are empty. */
    assert(strcmp(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_MUTE),
                  LANLAN_STR_SETTINGS_VALUE_OFF)
           == 0);
    s_model.globally_muted = true;
    assert(strcmp(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_MUTE),
                  LANLAN_STR_SETTINGS_VALUE_ON)
           == 0);
    s_model.globally_muted = false;
    assert(strcmp(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_REMINDER_SOUND),
                  LANLAN_STR_SETTINGS_VALUE_OFF)
           == 0);
    assert(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_REFRESH)[0] == '\0');
    assert(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_REMINDER_LIST)[0]
           == '\0');
    assert(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_SYNC)[0] == '\0');
    assert(lanlan_view_settings_row_value_text(&s_model, -1)[0] == '\0');
    assert(lanlan_view_settings_row_value_text(&s_model, LANLAN_SETTINGS_ROW_COUNT)[0] == '\0');
    assert(lanlan_view_settings_row_value_text(NULL, LANLAN_SETTINGS_ROW_DIM)[0] == '\0');
}

static void test_settings_timeout_stepping_and_rule(void) {
    lanlan_model_t model;
    lanlan_model_init(&model);
    assert(lanlan_model_set_timeouts(&model, 15, 60));

    /* Stepping up walks the allowed steps and clamps at the top. */
    assert(lanlan_model_step_dim(&model, 1) && model.dim_seconds == 30);
    assert(model.screen_off_seconds == 60); /* 30 < 60, no repair needed */
    assert(lanlan_model_step_dim(&model, 1) && model.dim_seconds == 60);
    assert(model.screen_off_seconds == 90); /* 60 is not < 60 -> repaired */
    assert(lanlan_model_timeouts_valid(model.dim_seconds, model.screen_off_seconds));
    assert(lanlan_model_step_dim(&model, 1) && model.dim_seconds == 120);
    assert(model.screen_off_seconds == 180); /* repaired to the next step */
    /* 120 is the largest dim step: further up is refused and changes nothing. */
    assert(!lanlan_model_step_dim(&model, 1));
    assert(model.dim_seconds == 120 && model.screen_off_seconds == 180);

    /* Stepping down clamps at the bottom. */
    assert(lanlan_model_step_dim(&model, -1) && model.dim_seconds == 60);
    assert(model.screen_off_seconds == 180); /* lowering dim never changes it */
    assert(lanlan_model_step_dim(&model, -1) && model.dim_seconds == 30);
    assert(lanlan_model_step_dim(&model, -1) && model.dim_seconds == 15);
    assert(!lanlan_model_step_dim(&model, -1));
    assert(model.dim_seconds == 15);

    /* Screen-off steps clamp at the top and at the smallest allowed step. */
    model.screen_off_seconds = 300;
    assert(!lanlan_model_step_screen_off(&model, 1));
    assert(model.screen_off_seconds == 300);
    assert(lanlan_model_step_screen_off(&model, -1) && model.screen_off_seconds == 180);
    assert(lanlan_model_step_screen_off(&model, -1) && model.screen_off_seconds == 90);
    assert(lanlan_model_step_screen_off(&model, -1) && model.screen_off_seconds == 60);
    assert(!lanlan_model_step_screen_off(&model, -1)); /* 60 is the smallest step */

    /* A screen-off step down that would land on or below dim is refused, not
     * clamped: with dim 120 and screen-off 180, 90 is not allowed. */
    assert(lanlan_model_set_timeouts(&model, 120, 180));
    assert(!lanlan_model_step_screen_off(&model, -1));
    assert(model.screen_off_seconds == 180 && model.dim_seconds == 120);
    assert(lanlan_model_timeouts_valid(model.dim_seconds, model.screen_off_seconds));
    /* Lowering dim away from the barrier makes the step legal again. */
    assert(lanlan_model_step_dim(&model, -1) && model.dim_seconds == 60);
    assert(lanlan_model_step_screen_off(&model, -1) && model.screen_off_seconds == 90);
    assert(lanlan_model_step_dim(&model, -1) && model.dim_seconds == 30);
    assert(lanlan_model_step_screen_off(&model, -1) && model.screen_off_seconds == 60);
    assert(lanlan_model_timeouts_valid(model.dim_seconds, model.screen_off_seconds));

    /* Raising dim to the maximum repairs screen-off to the largest step, and
     * from there the dim step is refused so the invariant can never break. */
    lanlan_model_init(&model);
    assert(lanlan_model_set_timeouts(&model, 15, 60));
    assert(lanlan_model_step_dim(&model, 1));                 /* 30 / 60 */
    assert(lanlan_model_step_dim(&model, 1));                 /* 60 / 90 */
    assert(lanlan_model_step_dim(&model, 1));                 /* 120 / 180 */
    assert(!lanlan_model_step_dim(&model, 1));                /* at the top */
    assert(model.dim_seconds == 120 && model.screen_off_seconds == 180);
    assert(lanlan_model_timeouts_valid(model.dim_seconds, model.screen_off_seconds));

    /* The whole reachable space satisfies the invariant. This walks every dim
     * step from every starting screen-off step in both directions. */
    for (int o = 0; o < LANLAN_SCREEN_OFF_STEP_COUNT; ++o) {
        for (int d = 0; d < LANLAN_DIM_STEP_COUNT; ++d) {
            if (LANLAN_DIM_STEPS_SECONDS[d] >= LANLAN_SCREEN_OFF_STEPS_SECONDS[o]) continue;
            assert(lanlan_model_set_timeouts(&model, LANLAN_DIM_STEPS_SECONDS[d],
                                             LANLAN_SCREEN_OFF_STEPS_SECONDS[o]));
            for (int step = 0; step < 8; ++step) {
                lanlan_model_step_dim(&model, 1);
                assert(model.screen_off_seconds > model.dim_seconds);
                assert(lanlan_model_timeouts_valid(model.dim_seconds, model.screen_off_seconds));
            }
            for (int step = 0; step < 8; ++step) {
                lanlan_model_step_dim(&model, -1);
                assert(model.screen_off_seconds > model.dim_seconds);
            }
            for (int step = 0; step < 8; ++step) {
                lanlan_model_step_screen_off(&model, 1);
                assert(model.screen_off_seconds > model.dim_seconds);
            }
            for (int step = 0; step < 8; ++step) {
                lanlan_model_step_screen_off(&model, -1);
                assert(model.screen_off_seconds > model.dim_seconds);
                assert(lanlan_model_timeouts_valid(model.dim_seconds, model.screen_off_seconds));
            }
        }
    }

    /* An invalid pair is refused outright, so no caller can install one. */
    lanlan_model_init(&model);
    assert(!lanlan_model_set_timeouts(&model, 45, 90));     /* not a dim step */
    assert(!lanlan_model_set_timeouts(&model, 30, 45));     /* not a screen-off step */
    assert(!lanlan_model_set_timeouts(&model, 60, 60));     /* not strictly greater */
    assert(!lanlan_model_set_timeouts(&model, 120, 90));    /* smaller than dim */
    assert(!lanlan_model_set_timeouts(&model, -1, 90));
    assert(!lanlan_model_set_timeouts(&model, 30, 0));
    assert(!lanlan_model_set_timeouts(NULL, 30, 90));
    assert(model.dim_seconds == LANLAN_TIMEOUT_DIM_DEFAULT_SECONDS);
    assert(model.screen_off_seconds == LANLAN_TIMEOUT_SCREEN_OFF_DEFAULT_SECONDS);
    assert(!lanlan_model_timeouts_valid(60, 60));
    assert(!lanlan_model_timeouts_valid(120, 90));
    assert(!lanlan_model_timeouts_valid(31, 90));
    assert(lanlan_model_timeouts_valid(120, 180));
    assert(lanlan_model_timeouts_valid(15, 60));
    assert(!lanlan_model_step_dim(NULL, 1));
    assert(!lanlan_model_step_screen_off(NULL, 1));

    /* The vertical gestures on a timeout row change the value, not the
     * selection; OK has nothing to toggle there. */
    lanlan_model_init(&model);
    model.page = LANLAN_PAGE_SETTINGS;
    model.settings_row = LANLAN_SETTINGS_ROW_DIM;
    int before = model.dim_seconds;
    lanlan_key_event_t event = {.key = LANLAN_KEY_DOWN, .woke_screen = false};
    assert(lanlan_model_handle_key(&model, &event) == LANLAN_ACTION_NONE);
    assert(model.settings_row == LANLAN_SETTINGS_ROW_DIM);
    assert(model.dim_seconds > before);
    assert(model.screen_off_seconds > model.dim_seconds);
    event.key = LANLAN_KEY_UP;
    assert(lanlan_model_handle_key(&model, &event) == LANLAN_ACTION_NONE);
    assert(model.dim_seconds == before);
    assert(model.settings_row == LANLAN_SETTINGS_ROW_DIM);
    event.key = LANLAN_KEY_OK_CLICK;
    assert(lanlan_model_handle_key(&model, &event) == LANLAN_ACTION_NONE);
    assert(model.dim_seconds == before);
    /* On any other row the same gesture still moves the selection. */
    model.settings_row = LANLAN_SETTINGS_ROW_MUTE;
    event.key = LANLAN_KEY_DOWN;
    lanlan_model_handle_key(&model, &event);
    assert(model.settings_row == LANLAN_SETTINGS_ROW_REMINDER_SOUND);
    /* The screen-off row behaves like the dim row. */
    model.settings_row = LANLAN_SETTINGS_ROW_SCREEN_OFF;
    before = model.screen_off_seconds;
    event.key = LANLAN_KEY_DOWN;
    lanlan_model_handle_key(&model, &event);
    assert(model.screen_off_seconds > before);
    assert(model.settings_row == LANLAN_SETTINGS_ROW_SCREEN_OFF);
    assert(model.screen_off_seconds > model.dim_seconds);
}

/* UTF-8-safe occurrence counter for the "category appears once" assertions. */
static int count_substring(const char *haystack, const char *needle) {
    int count = 0;
    size_t length = strlen(needle);
    if (length == 0) return 0;
    for (const char *at = haystack; (at = strstr(at, needle)) != NULL; at += length) ++count;
    return count;
}

/* Builds one record into a fresh view and renders its list row. */
static void render_one_record_row(lanlan_category_t category, lanlan_subitem_t subitem,
                                  const char *custom_name, double amount, lanlan_unit_t unit,
                                  uint16_t duration_minutes, const char *note, char *out,
                                  size_t out_size) {
    lanlan_cache_t cache;
    lanlan_cache_clear(&cache);
    cache.records[0] = make_record(1, 1760000000, category, subitem);
    if (custom_name != NULL) {
        snprintf(cache.records[0].custom_name, sizeof(cache.records[0].custom_name), "%s",
                 custom_name);
    }
    if (amount > 0.0) {
        lanlan_record_set_amount(&cache.records[0], amount, unit);
    } else {
        lanlan_record_set_amount_unknown(&cache.records[0]);
    }
    cache.records[0].duration_minutes = duration_minutes;
    if (note != NULL) {
        lanlan_record_set_note(&cache.records[0], note);
    }
    cache.record_count = 1;
    assert(lanlan_record_is_valid(&cache.records[0]) == LANLAN_RECORD_OK);
    assert(lanlan_cache_is_consistent(&cache));

    lanlan_records_view_t view;
    memset(&view, 0, sizeof(view));
    view.cache = cache;
    view.clock_trusted = true;
    view.utc_offset_min = 480;
    lanlan_view_record_row(&view, 0, out, out_size);
}

/* The list row must name the category exactly once and then at most one real
 * detail, so the records page never shows "喂食 · 喂食120 克". */
static void test_record_row_text(void) {
    char row[128];
    char text[64];

    /* 1760000000 at +08:00 is 16:53 local. */
    render_one_record_row(LANLAN_CAT_MEAL, LANLAN_SUB_NONE, NULL, 120.0, LANLAN_UNIT_G, 0, NULL,
                          row, sizeof(row));
    assert(strcmp(row, "16:53 · 喂食 · 120 克") == 0);
    /* The category must appear exactly once (counted after the whole
     * multi-byte token, because a single character of it is not a match). */
    assert(count_substring(row, LANLAN_STR_CATEGORIES_MEAL) == 1);
    assert(count_substring(row, LANLAN_STR_SUBITEMS_BATH) == 0);

    /* Unknown amount: no fabricated "0" and no dangling separator. */
    render_one_record_row(LANLAN_CAT_MEAL, LANLAN_SUB_NONE, NULL, 0.0, LANLAN_UNIT_NONE, 0, NULL,
                          row, sizeof(row));
    assert(strcmp(row, "16:53 · 喂食") == 0);
    assert(strstr(row, LANLAN_STR_QUANTITY_UNKNOWN) == NULL);
    assert(strstr(row, "0") == NULL);

    assert(count_substring(row, LANLAN_STR_CATEGORIES_MEAL) == 1);

    render_one_record_row(LANLAN_CAT_WATER, LANLAN_SUB_NONE, NULL, 250.0, LANLAN_UNIT_ML, 0, NULL,
                          row, sizeof(row));
    assert(strcmp(row, "16:53 · 喝水 · 250 毫升") == 0);
    assert(count_substring(row, LANLAN_STR_CATEGORIES_WATER) == 1);

    /* A care preset keeps its fixed sub-item label, not the category twice. */
    render_one_record_row(LANLAN_CAT_CARE, LANLAN_SUB_BATH, NULL, 0.0, LANLAN_UNIT_NONE, 0, NULL,
                          row, sizeof(row));
    assert(strcmp(row, "16:53 · 护理 · 洗澡") == 0);
    assert(count_substring(row, LANLAN_STR_CATEGORIES_CARE) == 1);

    /* A cleaning preset with a custom label shows the label, not the preset. */
    render_one_record_row(LANLAN_CAT_CLEANING, LANLAN_SUB_EAR, "擦耳朵", 0.0, LANLAN_UNIT_NONE, 0,
                          NULL, row, sizeof(row));
    assert(strcmp(row, "16:53 · 清洁 · 擦耳朵") == 0);
    assert(count_substring(row, LANLAN_STR_CATEGORIES_CLEANING) == 1);
    /* The preset label is replaced by the custom name, not appended to it. */
    assert(count_substring(row, LANLAN_STR_SUBITEMS_EAR) == 0);

    /* Walk carries its duration as the detail. */
    render_one_record_row(LANLAN_CAT_WALK, LANLAN_SUB_NONE, NULL, 0.0, LANLAN_UNIT_NONE, 25, NULL,
                          row, sizeof(row));
    assert(strcmp(row, "16:53 · 遛狗 · 25 分钟") == 0);
    assert(count_substring(row, LANLAN_STR_CATEGORIES_WALK) == 1);

    /* Other is defined by its custom name. */
    render_one_record_row(LANLAN_CAT_OTHER, LANLAN_SUB_NONE, "剪毛", 0.0, LANLAN_UNIT_NONE, 0, NULL,
                          row, sizeof(row));
    assert(strcmp(row, "16:53 · 其他 · 剪毛") == 0);
    assert(count_substring(row, LANLAN_STR_CATEGORIES_OTHER) == 1);

    /* A note is not repeated on every row; only its explicit cut is hinted. A
     * remote note is clamped into the bounded preview as it is read, so its
     * marker is what a list row can show. */
    lanlan_record_t long_note = make_record(2, 1760000000, LANLAN_CAT_MEAL, LANLAN_SUB_NONE);
    char remote_note[256];
    remote_note[0] = '\0';
    for (int i = 0; i < 40; ++i) strcat(remote_note, "好");
    assert(strlen(remote_note) == 120);   /* far past the 48-byte device preview */
    assert(lanlan_record_set_note(&long_note, remote_note));
    assert(long_note.note_truncated);
    lanlan_cache_t note_cache;
    lanlan_cache_clear(&note_cache);
    note_cache.records[0] = long_note;
    note_cache.record_count = 1;
    lanlan_records_view_t note_view;
    memset(&note_view, 0, sizeof(note_view));
    note_view.cache = note_cache;
    note_view.clock_trusted = true;
    lanlan_view_record_row(&note_view, 0, row, sizeof(row));
    snprintf(text, sizeof(text), "16:53 · %s · %s", LANLAN_STR_CATEGORIES_MEAL,
             LANLAN_RECORD_FIXED_LABELS[9]);
    assert(strcmp(row, text) == 0);
    assert(count_substring(row, LANLAN_STR_CATEGORIES_MEAL) == 1);
    assert(lanlan_record_format_local_time(&note_cache.records[0], text, sizeof(text)) > 0);
    assert(strcmp(text, "16:53") == 0);

    /* A record with a sub-item AND an amount keeps the name as the detail. */
    render_one_record_row(LANLAN_CAT_CLEANING, LANLAN_SUB_LITTER, NULL, 0.0, LANLAN_UNIT_NONE, 0,
                          NULL, row, sizeof(row));
    assert(strcmp(row, "16:53 · 清洁 · 清猫砂") == 0);
}

/* One reminder per cache slot, with distinct times so a shifted window is
 * immediately visible in the rendered text. */
static void build_reminder_window(lanlan_cache_t *cache, int count) {
    static const char *const times[] = {"07:00", "07:30", "08:00", "08:30",
                                        "09:00", "09:30", "10:00"};
    lanlan_cache_clear(cache);
    for (int i = 0; i < count && i < LANLAN_CACHE_MAX_REMINDERS; ++i) {
        memset(&cache->reminders[i], 0, sizeof(cache->reminders[i]));
        cache->reminders[i].id[0] = (uint8_t)(i + 1);
        cache->reminders[i].id[1] = 0xA0;
        cache->reminders[i].version = 1;
        cache->reminders[i].category = LANLAN_CAT_CARE;
        cache->reminders[i].subitem = (i % 3 == 0) ? LANLAN_SUB_BATH
                                    : (i % 3 == 1) ? LANLAN_SUB_TEETH
                                                   : LANLAN_SUB_COMB;
        cache->reminders[i].enabled = true;
        cache->reminders[i].last_rung_day = LANLAN_REMINDER_NO_RUNG;
        strcpy(cache->reminders[i].time_local, times[i % 7]);
    }
    cache->reminder_count = (uint32_t)count;
    assert(lanlan_cache_is_consistent(cache));
}

static void test_reminder_row_window_and_markers(void) {
    const int count = 7;
    lanlan_cache_t cache;
    build_reminder_window(&cache, count);

    lanlan_records_view_t view;
    memset(&view, 0, sizeof(view));
    view.cache = cache;
    view.clock_trusted = true;
    /* At 08:10 the reminders at 07:00, 07:30 and 08:00 are due. */
    lanlan_local_now_t now = {.local_day = 20370, .hour = 8, .minute = 10};

    lanlan_reminder_row_t rows[LANLAN_MODEL_LIST_ROWS];
    char expected[64];

    /* Offset 0: rows 0..4 map to cache entries 0..4. */
    view.reminder_offset = 0;
    lanlan_view_reminder_rows(&view, &now, rows, LANLAN_MODEL_LIST_ROWS);
    for (int i = 0; i < LANLAN_MODEL_LIST_ROWS; ++i) {
        const lanlan_reminder_t *entry = &cache.reminders[i];
        snprintf(expected, sizeof(expected), "%s %s",
                 i % 3 == 0 ? LANLAN_STR_SUBITEMS_BATH
                            : (i % 3 == 1 ? LANLAN_STR_SUBITEMS_TEETH
                                          : LANLAN_STR_SUBITEMS_COMB),
                 entry->time_local);
        assert(strcmp(rows[i].row, expected) == 0);
        /* The row itself never repeats the due badge text. */
        assert(strstr(rows[i].row, LANLAN_STR_REMINDERS_DUE) == NULL);
    }
    /* Due state comes only from the flag: 07:00, 07:30, 08:00 are due; 08:30 and
     * 09:00 are not. */
    assert(rows[0].due && rows[1].due && rows[2].due);
    assert(!rows[3].due && !rows[4].due);

    /* Offset 2: row i must now describe cache entry 2 + i, not entry i. This is
     * the scrolled case the preview harness caught. */
    view.reminder_offset = 2;
    lanlan_view_reminder_rows(&view, &now, rows, LANLAN_MODEL_LIST_ROWS);
    for (int i = 0; i < LANLAN_MODEL_LIST_ROWS; ++i) {
        const lanlan_reminder_t *entry = &cache.reminders[2 + i];
        int slot = 2 + i;
        snprintf(expected, sizeof(expected), "%s %s",
                 slot % 3 == 0 ? LANLAN_STR_SUBITEMS_BATH
                               : (slot % 3 == 1 ? LANLAN_STR_SUBITEMS_TEETH
                                                : LANLAN_STR_SUBITEMS_COMB),
                 entry->time_local);
        assert(strcmp(rows[i].row, expected) == 0);
        assert(strstr(rows[i].row, entry->time_local) != NULL);
    }
    /* Only cache entry 2 (08:00) of this window is due; 08:30 and later are
     * still ahead, so the window did not shift the due flags either. */
    assert(strstr(rows[0].row, "08:00") != NULL && rows[0].due);
    assert(strstr(rows[1].row, "08:30") != NULL && !rows[1].due);
    assert(!rows[2].due && !rows[3].due && !rows[4].due);
    /* The last window slot (cache entry 6, 10:00) is still inside the cache. */
    assert(strstr(rows[4].row, "10:00") != NULL);

    /* A window that runs past the end leaves the trailing rows blank. */
    view.reminder_offset = 5;
    lanlan_view_reminder_rows(&view, &now, rows, LANLAN_MODEL_LIST_ROWS);
    assert(strstr(rows[0].row, "09:30") != NULL);
    assert(strstr(rows[1].row, "10:00") != NULL);
    assert(rows[2].row[0] == '\0' && !rows[2].due);
    assert(rows[3].row[0] == '\0' && rows[4].row[0] == '\0');

    /* A disabled reminder is marked exactly once, in the row. The view owns a
     * copy of the cache, so the changed reminder must be reloaded into it. */
    cache.reminders[3].enabled = false;
    view.cache = cache;
    view.reminder_offset = 3;
    lanlan_view_reminder_rows(&view, &now, rows, LANLAN_MODEL_LIST_ROWS);
    /* Cache entry 3 is 洗澡 (slot 3 % 3 == 0) at 08:30, now disabled. */
    assert(strcmp(rows[0].row, LANLAN_STR_SUBITEMS_BATH " 08:30 · "
                  LANLAN_STR_REMINDERS_DISABLED) == 0);
    assert(!rows[0].due);
    assert(strstr(rows[0].row, LANLAN_STR_REMINDERS_DUE) == NULL);
    assert(strstr(rows[0].row, LANLAN_STR_REMINDERS_SOUND_OFF) == NULL);
    /* The next row is a different reminder and carries no disabled marker. */
    assert(strstr(rows[1].row, LANLAN_STR_REMINDERS_DISABLED) == NULL);

    /* An untrusted clock suppresses the due flag but still renders the rows. */
    view.reminder_offset = 0;
    view.clock_trusted = false;
    lanlan_view_reminder_rows(&view, &now, rows, LANLAN_MODEL_LIST_ROWS);
    assert(!rows[0].due && !rows[1].due && !rows[2].due);
    assert(rows[0].row[0] != '\0');

    /* Out-of-range and NULL inputs are harmless. */
    view.reminder_offset = 0;
    lanlan_view_reminder_rows(&view, &now, NULL, 0);
    lanlan_view_reminder_rows(NULL, &now, rows, LANLAN_MODEL_LIST_ROWS);
    assert(rows[0].row[0] == '\0');
    lanlan_view_reminder_rows(&view, &now, rows, 0);

    /* A reminder with no configured time is not due and shows the placeholder. */
    view.cache.reminders[0].time_local[0] = '\0';
    lanlan_view_reminder_rows(&view, &now, rows, LANLAN_MODEL_LIST_ROWS);
    assert(!rows[0].due);
    assert(strstr(rows[0].row, "--:--") != NULL);
}

static void test_view_model_titles_and_rows(void) {
    lanlan_model_init(&s_model);
    load_records(&s_model, 3);
    lanlan_cache_t cache = s_model.cache;
    memset(&s_view, 0, sizeof(s_view));
    s_view.cache = cache;
    s_view.clock_trusted = true;
    s_view.utc_offset_min = 480;
    s_view.last_sync_epoch = 1760000000;
    s_view.now_epoch = 1760000000 + 120;
    s_view.cursor = 41;
    s_view.battery_percent = 78;

    /* Page titles come from the generated string table and none is empty. */
    for (int page = LANLAN_PAGE_HOME; page <= LANLAN_PAGE_STATUS; ++page) {
        const char *title = lanlan_view_page_title((lanlan_page_t)page);
        assert(title != NULL && title[0] != '\0');
    }
    assert(strcmp(lanlan_view_page_title(LANLAN_PAGE_HOME), LANLAN_STR_PAGES_HOME) == 0);
    assert(strcmp(lanlan_view_page_title((lanlan_page_t)99), LANLAN_STR_PAGES_STATUS) == 0);

    /* Settings and home labels are never empty inside their bounds. */
    for (int row = 0; row < LANLAN_SETTINGS_ROW_COUNT; ++row) {
        const char *label = lanlan_view_settings_row_label(row);
        assert(label != NULL && label[0] != '\0');
    }
    assert(lanlan_view_settings_row_label(-1)[0] == '\0');
    assert(lanlan_view_settings_row_label(LANLAN_SETTINGS_ROW_COUNT)[0] == '\0');
    for (int entry = 0; entry < LANLAN_HOME_ENTRY_COUNT; ++entry) {
        assert(lanlan_view_home_entry_label(entry)[0] != '\0');
    }
    assert(lanlan_view_home_entry_label(-1)[0] == '\0');
    assert(strcmp(lanlan_view_character_state_label(LANLAN_CHARACTER_IDLE),
                  LANLAN_STR_COMPANION_STATE_IDLE)
           == 0);
    assert(strcmp(lanlan_view_character_state_label(LANLAN_CHARACTER_HAPPY),
                  LANLAN_STR_COMPANION_STATE_HAPPY)
           == 0);
    assert(lanlan_view_character_state_label(LANLAN_CHARACTER_COUNT)[0] == '\0');

    /* Reminder rows and record rows have their own focused tests below. */

    /* Empty states differ with the clock, and a record row is never empty. */
    lanlan_records_view_t empty_view;
    memset(&empty_view, 0, sizeof(empty_view));
    empty_view.clock_trusted = true;
    assert(strcmp(lanlan_view_records_empty_text(&empty_view), LANLAN_STR_RECORDS_EMPTY) == 0);
    empty_view.clock_trusted = false;
    assert(strcmp(lanlan_view_records_empty_text(&empty_view),
                  LANLAN_STR_RECORDS_EMPTY_UNTRUSTED_CLOCK)
           == 0);
    assert(strcmp(lanlan_view_reminders_empty_text(&empty_view), LANLAN_STR_REMINDERS_EMPTY) == 0);

    char row[128];
    lanlan_view_record_row(&s_view, 0, row, sizeof(row));
    assert(row[0] != '\0');
    assert(strstr(row, LANLAN_STR_CATEGORIES_MEAL) != NULL);
    /* An out-of-range index renders the empty state instead of a partial row. */
    lanlan_view_record_row(&s_view, 40, row, sizeof(row));
    assert(strcmp(row, lanlan_view_records_empty_text(&s_view)) == 0);
    lanlan_view_record_row(&s_view, -1, row, sizeof(row));
    assert(strcmp(row, lanlan_view_records_empty_text(&s_view)) == 0);
    lanlan_view_record_row(NULL, 0, row, sizeof(row));
    assert(row[0] == '\0');
    lanlan_view_record_row(&s_view, 0, NULL, sizeof(row));
    assert(lanlan_view_record(&s_view, 0) != NULL);
    assert(lanlan_view_record(NULL, 0) == NULL);
    assert(lanlan_view_reminder(&s_view, 0) != NULL);
    assert(lanlan_view_reminder(NULL, 0) == NULL);
    assert(lanlan_view_row_index(2, 3) == 1);
    assert(lanlan_view_row_index(2, 0) == 0);
    assert(lanlan_view_clamp_offset(12, 99, LANLAN_MODEL_LIST_ROWS) == 12 - LANLAN_MODEL_LIST_ROWS);
    assert(lanlan_view_clamp_offset(0, 5, LANLAN_MODEL_LIST_ROWS) == 0);
    assert(lanlan_view_clamp_offset(3, -1, LANLAN_MODEL_LIST_ROWS) == 0);
}

static void test_sync_status_text_with_age(void) {
    lanlan_records_view_t view;
    memset(&view, 0, sizeof(view));
    char text[96];

    /* Never synced. */
    view.now_epoch = 1760000000;
    lanlan_view_sync_status_text(&view, text, sizeof(text));
    assert(strcmp(text, LANLAN_STR_SYNC_NEVER) == 0);

    /* Fresh: under the 5-minute band. */
    view.last_sync_epoch = 1760000000;
    view.now_epoch = 1760000000 + 10;
    lanlan_view_sync_status_text(&view, text, sizeof(text));
    assert(strcmp(text, LANLAN_STR_SYNC_JUST_NOW) == 0);

    /* Minutes band, with the age value present. */
    view.now_epoch = 1760000000 + 600;
    lanlan_view_sync_status_text(&view, text, sizeof(text));
    assert(strstr(text, "10") != NULL);
    assert(strstr(text, LANLAN_STR_SYNC_MINUTES_AGO) != NULL);
    assert(strstr(text, LANLAN_STR_SYNC_AGE_PREFIX) != NULL);

    /* Hours band. */
    view.now_epoch = 1760000000 + 7200;
    lanlan_view_sync_status_text(&view, text, sizeof(text));
    assert(strstr(text, "2") != NULL);
    assert(strstr(text, LANLAN_STR_SYNC_HOURS_AGO) != NULL);

    /* A clock that moved backwards must not print a negative age. */
    view.now_epoch = 1760000000 - 5000;
    lanlan_view_sync_status_text(&view, text, sizeof(text));
    assert(strcmp(text, LANLAN_STR_SYNC_JUST_NOW) == 0);

    /* Small buffer: still terminated, never empty. */
    view.now_epoch = 1760000000 + 7200;
    lanlan_view_sync_status_text(&view, text, 8);
    assert(text[7] == '\0' && text[0] != '\0');
    text[0] = 'x';
    lanlan_view_sync_status_text(NULL, text, sizeof(text));
    assert(text[0] == '\0');
    lanlan_view_sync_status_text(&view, NULL, sizeof(text));

    /* Detail line: last sync time, cached count and cursor. */
    view.last_sync_epoch = 1760000000;
    view.cursor = 412;
    view.cache.record_count = 7;
    lanlan_view_sync_detail_text(&view, text, sizeof(text));
    assert(strstr(text, "2025-10-09T08:53:20Z") != NULL);
    assert(strstr(text, "412") != NULL);
    assert(strstr(text, "7") != NULL);
    char empty_detail[96];
    view.last_sync_epoch = 0;
    lanlan_view_sync_detail_text(&view, empty_detail, sizeof(empty_detail));
    assert(strstr(empty_detail, "--") != NULL);
    lanlan_view_sync_detail_text(NULL, text, sizeof(text));
    assert(text[0] == '\0');
    lanlan_view_sync_detail_text(&view, NULL, sizeof(text));

    /* Battery: "-1" is hidden, not drawn as a number or as 0%. */
    lanlan_view_battery_text(78, text, sizeof(text));
    assert(strcmp(text, "78%") == 0);
    lanlan_view_battery_text(0, text, sizeof(text));
    assert(strcmp(text, "0%") == 0);
    lanlan_view_battery_text(-1, text, sizeof(text));
    assert(strcmp(text, "--") == 0);
    assert(strcmp(text, "0%") != 0);
    lanlan_view_battery_text(-1, NULL, sizeof(text));
    lanlan_view_battery_text(-1, text, 0);
}

int main(void) {
    test_home_navigation_and_entries();
    test_records_navigation_bounds();
    test_detail_toggles_only_the_view();
    test_settings_rows();
    test_settings_timeout_rows();
    test_settings_timeout_stepping_and_rule();
    test_reminder_page();
    test_status_page();
    test_wake_only_first_gesture();
    test_companion_never_emits_a_record_action();
    test_view_model_titles_and_rows();
    test_record_row_text();
    test_reminder_row_window_and_markers();
    test_sync_status_text_with_age();
    puts("Lanlan model: PASS (navigation bounds, wake-only gesture, companion invariant, row text, reminder window, timeouts, view model)");
    return 0;
}

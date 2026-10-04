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

    /* Rows cycle and stay inside the documented bounds. */
    for (int i = 0; i < LANLAN_SETTINGS_ROW_COUNT; ++i) {
        assert((int)s_model.settings_row == i);
        press(&s_model, LANLAN_KEY_DOWN);
    }
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_REFRESH);
    press(&s_model, LANLAN_KEY_UP);
    assert(s_model.settings_row == LANLAN_SETTINGS_ROW_STORAGE);

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

static void test_wake_only_first_gesture(void) {
    lanlan_model_init(&s_model);
    s_model.page = LANLAN_PAGE_HOME;
    s_model.home_entry = LANLAN_HOME_ENTRY_TODAY;

    /* The display is off. */
    lanlan_model_set_active(&s_model, false);
    /* A long press only wakes the screen and must not also deliver a click:
     * the home page would otherwise enter the selected entry. */
    lanlan_key_event_t event = {.key = LANLAN_KEY_OK_LONG, .woke_screen = true};
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
    assert(s_model.active);
    assert(s_model.page == LANLAN_PAGE_HOME);
    assert(s_model.home_entry == LANLAN_HOME_ENTRY_TODAY);

    /* The release queued behind it is an ordinary click for the application and
     * must be handled normally; the model itself never fabricates one. */
    lanlan_model_set_active(&s_model, false);
    event.key = LANLAN_KEY_OK_CLICK;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_RECORD_OPEN);
    assert(s_model.page == LANLAN_PAGE_RECORDS);

    /* UP/DOWN also wake the screen and still navigate. */
    lanlan_model_set_active(&s_model, false);
    event.key = LANLAN_KEY_OK_LONG;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
    assert(s_model.page == LANLAN_PAGE_RECORDS);
    lanlan_model_set_active(&s_model, false);
    event.key = LANLAN_KEY_DOWN;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_NONE);
    assert(s_model.active);

    /* With the screen already on, a long press is delivered normally again. */
    event.key = LANLAN_KEY_OK_LONG;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_BACK_HOME);
    assert(s_model.page == LANLAN_PAGE_HOME);
    /* An event that claims it woke the screen while the model is active is just
     * a normal key: the flag is informational for the backlight, not a second
     * suppression path. */
    event.key = LANLAN_KEY_DOWN;
    event.woke_screen = true;
    lanlan_model_set_active(&s_model, true);
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_NONE);
    assert(s_model.home_entry == LANLAN_HOME_ENTRY_RECORDS);

    /* Waking on the companion page must not deliver the pet action. */
    s_model.page = LANLAN_PAGE_COMPANION;
    s_model.character = LANLAN_CHARACTER_IDLE;
    lanlan_model_set_active(&s_model, false);
    event.key = LANLAN_KEY_OK_LONG;
    event.woke_screen = true;
    assert(lanlan_model_handle_key(&s_model, &event) == LANLAN_ACTION_WAKE_ONLY);
    assert(s_model.page == LANLAN_PAGE_COMPANION);
    assert(s_model.character == LANLAN_CHARACTER_IDLE);
    assert(!lanlan_model_pending_record_action(&s_model));
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

    /* Reminder rows carry the configured time and the due state. */
    lanlan_reminder_row_t rows[LANLAN_MODEL_LIST_ROWS];
    lanlan_local_now_t now = {.local_day = 20370, .hour = 8, .minute = 0};
    lanlan_cache_t with_reminder;
    lanlan_cache_clear(&with_reminder);
    memset(&with_reminder.reminders[0], 0, sizeof(with_reminder.reminders[0]));
    with_reminder.reminders[0].id[0] = 1;
    with_reminder.reminders[0].version = 1;
    with_reminder.reminders[0].category = LANLAN_CAT_CARE;
    with_reminder.reminders[0].subitem = LANLAN_SUB_TEETH;
    with_reminder.reminders[0].enabled = true;
    with_reminder.reminders[0].last_rung_day = LANLAN_REMINDER_NO_RUNG;
    strcpy(with_reminder.reminders[0].time_local, "07:40");
    with_reminder.reminder_count = 1;
    lanlan_records_view_t reminder_view = {.cache = with_reminder, .clock_trusted = true};
    lanlan_view_reminder_rows(&reminder_view, &now, rows, LANLAN_MODEL_LIST_ROWS);
    assert(rows[0].due);
    assert(strstr(rows[0].row, "07:40") != NULL);
    assert(strstr(rows[0].row, LANLAN_STR_REMINDERS_DUE) != NULL);
    assert(strstr(rows[0].row, LANLAN_STR_SUBITEMS_TEETH) != NULL);
    /* An untrusted clock never shows a due row. */
    reminder_view.clock_trusted = false;
    lanlan_view_reminder_rows(&reminder_view, &now, rows, LANLAN_MODEL_LIST_ROWS);
    assert(!rows[0].due);
    assert(strstr(rows[0].row, LANLAN_STR_REMINDERS_SOUND_OFF) != NULL);
    /* Rows beyond the cache are blank rather than stale. */
    assert(rows[1].row[0] == '\0' && !rows[1].due);
    lanlan_view_reminder_rows(NULL, &now, rows, LANLAN_MODEL_LIST_ROWS);
    lanlan_view_reminder_rows(&reminder_view, &now, NULL, 0);

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
    test_reminder_page();
    test_status_page();
    test_wake_only_first_gesture();
    test_companion_never_emits_a_record_action();
    test_view_model_titles_and_rows();
    test_sync_status_text_with_age();
    puts("Lanlan model: PASS (navigation bounds, wake-only gesture, companion invariant, view model)");
    return 0;
}

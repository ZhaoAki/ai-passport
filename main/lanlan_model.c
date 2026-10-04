/* Cyber Lanlan model and view model implementation. See lanlan_model.h. */
#include "lanlan_model.h"

#include "lanlan_record.h"
#include "lanlan_strings.h"

#include <stdio.h>
#include <string.h>

#define LANLAN_SYNC_AGE_FRESH_SECONDS 300
#define LANLAN_SYNC_AGE_STALE_SECONDS 3600

/* -------------------------------------------------------------- lifecycle -- */

void lanlan_model_clear(lanlan_model_t *model) {
    if (!model) return;
    memset(model, 0, sizeof(*model));
    model->page = LANLAN_PAGE_HOME;
    model->active = true;
    model->character = LANLAN_CHARACTER_IDLE;
    model->detail_full = 0;
}

void lanlan_model_init(lanlan_model_t *model) { lanlan_model_clear(model); }

static int clamp_index(int value, int count) {
    if (count <= 0) return 0;
    if (value < 0) return 0;
    if (value >= count) return count - 1;
    return value;
}

void lanlan_model_load_cache(lanlan_model_t *model, const lanlan_cache_t *cache) {
    if (!model || !cache) return;
    model->cache = *cache;
    int record_count = (int)model->cache.record_count;
    int reminder_count = (int)model->cache.reminder_count;
    model->record_index = clamp_index(model->record_index, record_count);
    model->reminder_index = clamp_index(model->reminder_index, reminder_count);
    model->list_offset = lanlan_view_clamp_offset(record_count, model->list_offset,
                                                  LANLAN_MODEL_LIST_ROWS);
    model->reminder_offset = lanlan_view_clamp_offset(reminder_count, model->reminder_offset,
                                                      LANLAN_MODEL_LIST_ROWS);
}

void lanlan_model_set_clock(lanlan_model_t *model, const lanlan_local_now_t *now,
                            bool clock_trusted) {
    if (!model) return;
    if (now) model->now = *now;
    model->clock_trusted = clock_trusted;
}

void lanlan_model_set_active(lanlan_model_t *model, bool active) {
    if (model) model->active = active;
}

void lanlan_model_set_page(lanlan_model_t *model, lanlan_page_t page) {
    if (!model) return;
    if (page < LANLAN_PAGE_HOME || page > LANLAN_PAGE_STATUS) return;
    model->page = page;
}

/* ------------------------------------------------------------- invariant -- */

bool lanlan_action_is_record_write(lanlan_action_t action) {
    return action == LANLAN_ACTION_RECORD_CREATE || action == LANLAN_ACTION_RECORD_REVOKE;
}

bool lanlan_model_pending_record_action(const lanlan_model_t *model) {
    (void)model;
    /* The device has no record input in the first generation, so no page can
     * leave a write pending. The companion page in particular reports its pet
     * action separately and must never be mistaken for a care record. */
    return false;
}

/* --------------------------------------------------------------- helpers -- */

static int cycle(int value, int count, int step) {
    if (count <= 0) return 0;
    int next = (value + step) % count;
    if (next < 0) next += count;
    return next;
}

/* Keeps the selected row inside the visible window by shifting the offset the
 * smallest amount that brings it back into view. */
static void ensure_visible(int *offset, int index, int count, int rows) {
    if (count <= 0) {
        *offset = 0;
        return;
    }
    if (index < *offset) *offset = index;
    if (index >= *offset + rows) *offset = index - rows + 1;
    *offset = lanlan_view_clamp_offset(count, *offset, rows);
}

/* Nudges the window when the selection lands exactly on its edge. The record
 * list uses this so a single UP/DOWN always moves the highlighted row by one
 * and only then scrolls, which matches the design's "previous / next record". */
static void scroll_window(int *offset, int index, int count, int rows) {
    if (count <= rows) {
        *offset = 0;
        return;
    }
    if (index == *offset && index > 0) *offset = index - 1;
    if (index == *offset + rows - 1 && index < count - 1) *offset = index - rows + 2;
    *offset = lanlan_view_clamp_offset(count, *offset, rows);
}

static void move_record_selection(lanlan_model_t *model, int step) {
    int count = (int)model->cache.record_count;
    if (count <= 0) {
        model->record_index = 0;
        model->list_offset = 0;
        return;
    }
    model->record_index = cycle(model->record_index, count, step);
    scroll_window(&model->list_offset, model->record_index, count, LANLAN_MODEL_LIST_ROWS);
}

static void move_reminder_selection(lanlan_model_t *model, int step) {
    int count = (int)model->cache.reminder_count;
    if (count <= 0) {
        model->reminder_index = 0;
        model->reminder_offset = 0;
        return;
    }
    model->reminder_index = cycle(model->reminder_index, count, step);
    ensure_visible(&model->reminder_offset, model->reminder_index, count, LANLAN_MODEL_LIST_ROWS);
}

static lanlan_action_t handle_home(lanlan_model_t *model, lanlan_key_t key) {
    switch (key) {
    case LANLAN_KEY_UP:
        model->home_entry = (lanlan_home_entry_t)cycle((int)model->home_entry,
                                                       LANLAN_HOME_ENTRY_COUNT, -1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_DOWN:
        model->home_entry = (lanlan_home_entry_t)cycle((int)model->home_entry,
                                                       LANLAN_HOME_ENTRY_COUNT, 1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_CLICK:
        switch (model->home_entry) {
        case LANLAN_HOME_ENTRY_TODAY:
            /* The summary is rendered from the same cache as the list. */
            model->page = LANLAN_PAGE_RECORDS;
            model->list_offset = 0;
            model->record_index = 0;
            return LANLAN_ACTION_RECORD_OPEN;
        case LANLAN_HOME_ENTRY_RECORDS:
            model->page = LANLAN_PAGE_RECORDS;
            model->list_offset = 0;
            model->record_index = 0;
            return LANLAN_ACTION_NONE;
        case LANLAN_HOME_ENTRY_COMPANION:
            model->page = LANLAN_PAGE_COMPANION;
            model->character = LANLAN_CHARACTER_IDLE;
            model->companion_mood = 0;
            return LANLAN_ACTION_NONE;
        case LANLAN_HOME_ENTRY_SETTINGS:
            model->page = LANLAN_PAGE_SETTINGS;
            return LANLAN_ACTION_NONE;
        case LANLAN_HOME_ENTRY_COUNT:
            break;
        }
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_LONG:
        /* The design leaves the home long press unassigned. */
        return LANLAN_ACTION_NONE;
    }
    return LANLAN_ACTION_NONE;
}

static lanlan_action_t handle_records(lanlan_model_t *model, lanlan_key_t key) {
    switch (key) {
    case LANLAN_KEY_UP:
        move_record_selection(model, -1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_DOWN:
        move_record_selection(model, 1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_CLICK:
        if (model->cache.record_count == 0) return LANLAN_ACTION_NONE;
        model->page = LANLAN_PAGE_DETAIL;
        model->detail_full = 0;
        return LANLAN_ACTION_RECORD_OPEN;
    case LANLAN_KEY_OK_LONG:
        model->page = LANLAN_PAGE_HOME;
        return LANLAN_ACTION_BACK_HOME;
    }
    return LANLAN_ACTION_NONE;
}

static lanlan_action_t handle_detail(lanlan_model_t *model, lanlan_key_t key) {
    switch (key) {
    case LANLAN_KEY_UP:
        move_record_selection(model, -1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_DOWN:
        move_record_selection(model, 1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_CLICK:
        /* Only the presentation switches; the record itself never changes. */
        model->detail_full = !model->detail_full;
        return LANLAN_ACTION_DETAIL_TOGGLE_VIEW;
    case LANLAN_KEY_OK_LONG:
        model->page = LANLAN_PAGE_RECORDS;
        return LANLAN_ACTION_BACK_HOME;
    }
    return LANLAN_ACTION_NONE;
}

static lanlan_action_t handle_companion(lanlan_model_t *model, lanlan_key_t key) {
    switch (key) {
    case LANLAN_KEY_UP:
    case LANLAN_KEY_DOWN:
        model->character = (lanlan_character_t)cycle((int)model->character,
                                                     LANLAN_CHARACTER_COUNT,
                                                     key == LANLAN_KEY_UP ? -1 : 1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_CLICK:
        /* Petting the character is a virtual interaction: it changes mood and
         * plays a short sound, and it never creates a care record. */
        model->companion_mood = 1;
        model->character = LANLAN_CHARACTER_HAPPY;
        return LANLAN_ACTION_COMPANION_PET;
    case LANLAN_KEY_OK_LONG:
        model->page = LANLAN_PAGE_HOME;
        return LANLAN_ACTION_BACK_HOME;
    }
    return LANLAN_ACTION_NONE;
}

static lanlan_action_t handle_settings(lanlan_model_t *model, lanlan_key_t key) {
    switch (key) {
    case LANLAN_KEY_UP:
        model->settings_row = (lanlan_settings_row_t)cycle((int)model->settings_row,
                                                           LANLAN_SETTINGS_ROW_COUNT, -1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_DOWN:
        model->settings_row = (lanlan_settings_row_t)cycle((int)model->settings_row,
                                                           LANLAN_SETTINGS_ROW_COUNT, 1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_CLICK:
        switch (model->settings_row) {
        case LANLAN_SETTINGS_ROW_REFRESH:
            return LANLAN_ACTION_SETTINGS_REFRESH;
        case LANLAN_SETTINGS_ROW_MUTE:
            model->globally_muted = !model->globally_muted;
            return LANLAN_ACTION_NONE;
        case LANLAN_SETTINGS_ROW_REMINDER_SOUND:
            model->reminder_sound_enabled = !model->reminder_sound_enabled;
            return LANLAN_ACTION_NONE;
        case LANLAN_SETTINGS_ROW_REMINDER_LIST:
            model->page = LANLAN_PAGE_REMINDERS;
            model->reminder_offset = 0;
            model->reminder_index = 0;
            return LANLAN_ACTION_SETTINGS_REMINDER_LIST;
        case LANLAN_SETTINGS_ROW_TIMEZONE:
        case LANLAN_SETTINGS_ROW_SYNC:
        case LANLAN_SETTINGS_ROW_STORAGE:
            /* Read-only rows: the value is rendered, nothing is changed here. */
            return LANLAN_ACTION_NONE;
        case LANLAN_SETTINGS_ROW_COUNT:
            break;
        }
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_LONG:
        model->page = LANLAN_PAGE_HOME;
        return LANLAN_ACTION_BACK_HOME;
    }
    return LANLAN_ACTION_NONE;
}

static lanlan_action_t handle_reminders(lanlan_model_t *model, lanlan_key_t key) {
    switch (key) {
    case LANLAN_KEY_UP:
        move_reminder_selection(model, -1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_DOWN:
        move_reminder_selection(model, 1);
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_CLICK:
        /* Dismissing a reminder is not completion, so OK does nothing here. */
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_LONG:
        model->page = LANLAN_PAGE_SETTINGS;
        return LANLAN_ACTION_BACK_HOME;
    }
    return LANLAN_ACTION_NONE;
}

static lanlan_action_t handle_status(lanlan_model_t *model, lanlan_key_t key) {
    switch (key) {
    case LANLAN_KEY_UP:
    case LANLAN_KEY_DOWN:
        return LANLAN_ACTION_NONE;
    case LANLAN_KEY_OK_CLICK:
        return LANLAN_ACTION_STATUS_RETRY;
    case LANLAN_KEY_OK_LONG:
        model->page = LANLAN_PAGE_HOME;
        return LANLAN_ACTION_BACK_HOME;
    }
    return LANLAN_ACTION_NONE;
}

lanlan_action_t lanlan_model_handle_key(lanlan_model_t *model, const lanlan_key_event_t *event) {
    if (!model || !event) return LANLAN_ACTION_NONE;
    if (!model->active) {
        /* The first gesture after the screen turned off only wakes it. A long
         * press is consumed entirely, so releasing it cannot also deliver a
         * click and the model is not left half-active. */
        model->active = true;
        if (event->key == LANLAN_KEY_OK_LONG) return LANLAN_ACTION_WAKE_ONLY;
    }
    switch (model->page) {
    case LANLAN_PAGE_HOME: return handle_home(model, event->key);
    case LANLAN_PAGE_RECORDS: return handle_records(model, event->key);
    case LANLAN_PAGE_DETAIL: return handle_detail(model, event->key);
    case LANLAN_PAGE_COMPANION: return handle_companion(model, event->key);
    case LANLAN_PAGE_SETTINGS: return handle_settings(model, event->key);
    case LANLAN_PAGE_REMINDERS: return handle_reminders(model, event->key);
    case LANLAN_PAGE_STATUS: return handle_status(model, event->key);
    }
    return LANLAN_ACTION_NONE;
}

/* ----------------------------------------------------------- view model -- */

int lanlan_view_clamp_offset(int count, int offset, int rows) {
    if (count <= 0 || rows <= 0) return 0;
    int max_offset = count > rows ? count - rows : 0;
    if (offset < 0) return 0;
    if (offset > max_offset) return max_offset;
    return offset;
}

int lanlan_view_row_index(int list_offset, int index) {
    int row = index - list_offset;
    return row < 0 ? 0 : row;
}

const lanlan_record_t *lanlan_view_record(const lanlan_records_view_t *view, int index) {
    if (!view) return NULL;
    return lanlan_cache_record_at(&view->cache, index);
}

const lanlan_reminder_t *lanlan_view_reminder(const lanlan_records_view_t *view, int index) {
    if (!view) return NULL;
    return lanlan_cache_reminder_at(&view->cache, index);
}

const char *lanlan_view_page_title(lanlan_page_t page) {
    switch (page) {
    case LANLAN_PAGE_HOME: return LANLAN_STR_PAGES_HOME;
    case LANLAN_PAGE_RECORDS: return LANLAN_STR_PAGES_RECORDS;
    case LANLAN_PAGE_DETAIL: return LANLAN_STR_PAGES_DETAIL;
    case LANLAN_PAGE_COMPANION: return LANLAN_STR_PAGES_COMPANION;
    case LANLAN_PAGE_SETTINGS: return LANLAN_STR_PAGES_SETTINGS;
    case LANLAN_PAGE_REMINDERS: return LANLAN_STR_PAGES_REMINDERS;
    case LANLAN_PAGE_STATUS: return LANLAN_STR_PAGES_STATUS;
    }
    return LANLAN_STR_PAGES_STATUS;
}

const char *lanlan_view_settings_row_label(int row) {
    static const char *const s_rows[LANLAN_SETTINGS_ROW_COUNT] = {
        LANLAN_STR_SETTINGS_ROW_REFRESH,
        LANLAN_STR_SETTINGS_ROW_MUTE,
        LANLAN_STR_SETTINGS_ROW_REMINDER_SOUND,
        LANLAN_STR_SETTINGS_ROW_REMINDER_LIST,
        LANLAN_STR_SETTINGS_ROW_TIMEZONE,
        LANLAN_STR_SETTINGS_ROW_SYNC,
        LANLAN_STR_SETTINGS_ROW_STORAGE,
    };
    if (row < 0 || row >= LANLAN_SETTINGS_ROW_COUNT) return "";
    return s_rows[row];
}

const char *lanlan_view_home_entry_label(int entry) {
    static const char *const s_entries[LANLAN_HOME_ENTRY_COUNT] = {
        LANLAN_STR_HOME_ENTRY_TODAY,
        LANLAN_STR_HOME_ENTRY_RECORDS,
        LANLAN_STR_HOME_ENTRY_COMPANION,
        LANLAN_STR_HOME_ENTRY_SETTINGS,
    };
    if (entry < 0 || entry >= LANLAN_HOME_ENTRY_COUNT) return "";
    return s_entries[entry];
}

const char *lanlan_view_character_state_label(lanlan_character_t character) {
    switch (character) {
    case LANLAN_CHARACTER_IDLE: return LANLAN_STR_COMPANION_STATE_IDLE;
    case LANLAN_CHARACTER_BLINK: return LANLAN_STR_COMPANION_STATE_BLINK;
    case LANLAN_CHARACTER_HAPPY: return LANLAN_STR_COMPANION_STATE_HAPPY;
    case LANLAN_CHARACTER_COUNT: break;
    }
    return "";
}

const char *lanlan_view_records_empty_text(const lanlan_records_view_t *view) {
    if (view && !view->clock_trusted) return LANLAN_STR_RECORDS_EMPTY_UNTRUSTED_CLOCK;
    return LANLAN_STR_RECORDS_EMPTY;
}

const char *lanlan_view_reminders_empty_text(const lanlan_records_view_t *view) {
    (void)view;
    return LANLAN_STR_REMINDERS_EMPTY;
}

void lanlan_view_record_row(const lanlan_records_view_t *view, int index, char *out,
                            size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!view) return;
    const lanlan_record_t *record = lanlan_cache_record_at(&view->cache, index);
    if (record->version == 0 && lanlan_record_id_is_zero(record->id)) {
        /* Out-of-range index: render the empty-state line instead of a
         * half-filled row so a stale selection cannot draw garbage. */
        snprintf(out, out_size, "%s", lanlan_view_records_empty_text(view));
        return;
    }
    char time_text[8];
    if (lanlan_record_format_local_time(record, time_text, sizeof(time_text)) == 0) {
        snprintf(time_text, sizeof(time_text), "--:--");
    }
    const char *category = lanlan_category_label(record->category);

    /* Second field: a REAL sub-item or custom name only. The one exception is a
     * custom name falling back to its category word: that is still a name the
     * caregiver chose, so it is kept while the category's OWN label is not. */
    char subitem[LANLAN_RECORD_CUSTOM_BYTES];
    lanlan_record_subitem_label(record, subitem, sizeof(subitem));
    if (strcmp(subitem, category) == 0) subitem[0] = '\0';

    char amount[32];
    if (!lanlan_record_amount_is_known(record)
        || lanlan_record_format_amount(record, amount, sizeof(amount)) == 0) {
        amount[0] = '\0';
    }
    char duration[24];
    if (lanlan_record_format_duration(record, duration, sizeof(duration)) == 0) {
        duration[0] = '\0';
    }

    /* Detail priority: sub-item or custom name, then the measured quantity
     * (amount before duration), then the note's truncation marker. Exactly one
     * detail is rendered so the row stays on a single line; the note text itself
     * is deliberately not duplicated here because every row in this list would
     * carry the same sentence. */
    const char *detail = NULL;
    if (subitem[0] != '\0') {
        detail = subitem;
    } else if (amount[0] != '\0') {
        detail = amount;
    } else if (duration[0] != '\0') {
        detail = duration;
    } else if (record->note_truncated) {
        detail = LANLAN_RECORD_FIXED_LABELS[9];
    }
    if (detail != NULL && detail[0] != '\0') {
        snprintf(out, out_size, "%s · %s · %s", time_text, category, detail);
    } else {
        snprintf(out, out_size, "%s · %s", time_text, category);
    }
    out[out_size - 1] = '\0';
}

void lanlan_view_reminder_rows(const lanlan_records_view_t *view, const lanlan_local_now_t *now,
                               lanlan_reminder_row_t *rows, size_t row_count) {
    if (!rows || row_count == 0) return;
    for (size_t i = 0; i < row_count; ++i) {
        rows[i].row[0] = '\0';
        rows[i].due = false;
    }
    if (!view) return;
    /* The renderer maps output row i to cache index reminder_offset + i, so this
     * function must fill the visible window, not the first rows of the cache. */
    const int offset = view->reminder_offset;
    for (size_t i = 0; i < row_count; ++i) {
        int index = offset + (int)i;
        if (index < 0 || index >= (int)view->cache.reminder_count) break;
        const lanlan_reminder_t *reminder = &view->cache.reminders[index];

        /* The row carries the reminder's identity and its configured time. The
         * due/disabled state is a separate marker: the caller draws the due
         * badge from rows[i].due, and a disabled reminder is marked here because
         * the caller cannot draw a due badge for it. */
        lanlan_record_t wrapper;
        memset(&wrapper, 0, sizeof(wrapper));
        wrapper.category = reminder->category;
        wrapper.subitem = reminder->subitem;
        memcpy(wrapper.custom_name, reminder->custom_name, sizeof(wrapper.custom_name));
        char subitem[LANLAN_RECORD_CUSTOM_BYTES];
        lanlan_record_subitem_label(&wrapper, subitem, sizeof(subitem));
        const char *category = lanlan_category_label(reminder->category);
        const char *name = subitem[0] != '\0' ? subitem : category;
        const char *time_text = reminder->time_local[0] != '\0' ? reminder->time_local : "--:--";

        rows[i].due = lanlan_reminder_due(reminder, now, view->clock_trusted)
                      == LANLAN_REMINDER_DUE;
        if (!reminder->enabled) {
            snprintf(rows[i].row, sizeof(rows[i].row), "%s %s · %s", name, time_text,
                     LANLAN_STR_REMINDERS_DISABLED);
        } else {
            snprintf(rows[i].row, sizeof(rows[i].row), "%s %s", name, time_text);
        }
        rows[i].row[sizeof(rows[i].row) - 1] = '\0';
    }
}

void lanlan_view_sync_status_text(const lanlan_records_view_t *view, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!view) return;
    if (view->last_sync_epoch <= 0) {
        snprintf(out, out_size, "%s", LANLAN_STR_SYNC_NEVER);
        return;
    }
    int64_t age = view->now_epoch - view->last_sync_epoch;
    if (age < 0) age = 0;
    const char *band = LANLAN_STR_SYNC_HOURS_AGO;
    int64_t value = 0;
    if (age < LANLAN_SYNC_AGE_FRESH_SECONDS) {
        band = LANLAN_STR_SYNC_JUST_NOW;
        value = -1;
    } else if (age < LANLAN_SYNC_AGE_STALE_SECONDS) {
        band = LANLAN_STR_SYNC_MINUTES_AGO;
        value = age / 60;
    } else {
        band = LANLAN_STR_SYNC_HOURS_AGO;
        value = age / 3600;
    }
    if (value < 0) {
        snprintf(out, out_size, "%s", band);
    } else {
        snprintf(out, out_size, "%s %lld %s", LANLAN_STR_SYNC_AGE_PREFIX, (long long)value,
                 band);
    }
    out[out_size - 1] = '\0';
}

void lanlan_view_sync_detail_text(const lanlan_records_view_t *view, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!view) return;
    char stamp[LANLAN_RFC3339_MAX];
    if (view->last_sync_epoch > 0) {
        lanlan_time_format_rfc3339(view->last_sync_epoch, stamp, sizeof(stamp));
    } else {
        snprintf(stamp, sizeof(stamp), "%s", "--");
    }
    snprintf(out, out_size, "%s %s · %s %u %s · %s %u", LANLAN_STR_SYNC_DETAIL, stamp,
             LANLAN_STR_SYNC_CACHE_COUNT, (unsigned)view->cache.record_count,
             LANLAN_STR_SYNC_RECORDS_UNIT, LANLAN_STR_SYNC_CURSOR, (unsigned)view->cursor);
    out[out_size - 1] = '\0';
}

void lanlan_view_battery_text(int battery_percent, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    /* The baseline renders "-1" as hidden, not as "0%" and not as a number. */
    if (battery_percent < 0) {
        snprintf(out, out_size, "%s", "--");
    } else {
        snprintf(out, out_size, "%d%%", battery_percent);
    }
    out[out_size - 1] = '\0';
}

/* Cyber Lanlan page and selection state machine plus the view-model helpers
 * that turn cache content into the strings the UI renders.
 *
 * The state machine is pure: it receives already de-bounced key events, reads
 * the cache, and returns one action for the application worker to perform. It
 * never touches LVGL, the network or storage, and it never allocates.
 *
 * Non-negotiable rule: the passport never creates records. No page produces a
 * record action, and the companion's OK (pet) action in particular must never
 * be reported as one; lanlan_model_pending_record_action() is the single
 * explicit invariant the application and the tests check. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lanlan_cache.h"
#include "lanlan_reminder.h"
#include "lanlan_time.h"

/* Home entries, in list order. */
typedef enum {
    LANLAN_HOME_ENTRY_TODAY = 0,
    LANLAN_HOME_ENTRY_RECORDS,
    LANLAN_HOME_ENTRY_COMPANION,
    LANLAN_HOME_ENTRY_SETTINGS,
    LANLAN_HOME_ENTRY_COUNT
} lanlan_home_entry_t;

/* Settings rows, in list order. */
typedef enum {
    LANLAN_SETTINGS_ROW_REFRESH = 0,
    LANLAN_SETTINGS_ROW_MUTE,
    LANLAN_SETTINGS_ROW_REMINDER_SOUND,
    LANLAN_SETTINGS_ROW_REMINDER_LIST,
    LANLAN_SETTINGS_ROW_TIMEZONE,
    LANLAN_SETTINGS_ROW_SYNC,
    LANLAN_SETTINGS_ROW_STORAGE,
    LANLAN_SETTINGS_ROW_COUNT
} lanlan_settings_row_t;

typedef enum {
    LANLAN_PAGE_HOME = 0,
    LANLAN_PAGE_RECORDS,
    LANLAN_PAGE_DETAIL,
    LANLAN_PAGE_COMPANION,
    LANLAN_PAGE_SETTINGS,
    LANLAN_PAGE_REMINDERS,
    LANLAN_PAGE_STATUS
} lanlan_page_t;

typedef enum {
    LANLAN_KEY_UP = 0,
    LANLAN_KEY_DOWN,
    LANLAN_KEY_OK_CLICK,
    LANLAN_KEY_OK_LONG
} lanlan_key_t;

/* Companion frames; the idle frame loops, blink overlays it and happy is the
 * short reaction to OK. */
typedef enum {
    LANLAN_CHARACTER_IDLE = 0,
    LANLAN_CHARACTER_BLINK,
    LANLAN_CHARACTER_HAPPY,
    LANLAN_CHARACTER_COUNT
} lanlan_character_t;

typedef enum {
    LANLAN_ACTION_NONE = 0,
    /* The first gesture after the screen turned off only woke the display. */
    LANLAN_ACTION_WAKE_ONLY,
    LANLAN_ACTION_BACK_HOME,
    LANLAN_ACTION_DETAIL_TOGGLE_VIEW,
    /* Open the record at model->record_index in the detail page. */
    LANLAN_ACTION_RECORD_OPEN,
    /* A pet interaction; always paired with LANLAN_ACTION_NONE for records. */
    LANLAN_ACTION_COMPANION_PET,
    LANLAN_ACTION_SETTINGS_REFRESH,
    LANLAN_ACTION_SETTINGS_REMINDER_LIST,
    LANLAN_ACTION_STATUS_RETRY,
    /* Forbidden values: kept in the enum so the invariant is testable. */
    LANLAN_ACTION_RECORD_CREATE,
    LANLAN_ACTION_RECORD_REVOKE
} lanlan_action_t;

/* One de-bounced key event. `woke_screen` is true when the display was off
 * before the gesture, so the model can apply the wake-only rule. */
typedef struct {
    lanlan_key_t key;
    bool woke_screen;
} lanlan_key_event_t;

typedef struct {
    lanlan_cache_t cache;   /* owned copy of the bounded cache */
    lanlan_page_t page;
    lanlan_home_entry_t home_entry;
    int record_index;       /* index into cache.records, newest first */
    int list_offset;        /* first visible row of the record list */
    int detail_full;        /* 0 compact, 1 full view */
    lanlan_character_t character;
    uint8_t companion_mood; /* 0 idle, 1 happy; driven by the pet action */
    lanlan_settings_row_t settings_row;
    int reminder_index;
    int reminder_offset;
    /* Local UI state. The application owns NVS; these fields exist so the
     * settings rows render the current value without a second lookup. */
    bool globally_muted;
    bool reminder_sound_enabled;
    int16_t utc_offset_min;
    bool active;              /* display on; false enables the wake-only rule */
    lanlan_local_now_t now;   /* last known local time, injected by the app */
    bool clock_trusted;
} lanlan_model_t;

/* Reminder rows the reminder page can show at once; the UI scrolls. */
#define LANLAN_MODEL_LIST_ROWS 5

/* Read-only view of the data the renderer needs. Owning everything by value
 * keeps the model independent from the cache's lifetime. */
typedef struct {
    lanlan_cache_t cache;
    bool clock_trusted;
    int16_t utc_offset_min;
    int64_t last_sync_epoch;   /* 0 when the device never synced successfully */
    int64_t now_epoch;
    uint32_t cursor;           /* last applied service cursor */
    int battery_percent;       /* -1 when bsp_battery_soc() is unavailable */
    /* First visible cache index of each list. The row helpers below fill the
     * window that starts at these offsets, which is the same window the
     * renderer draws, so a scrolled list stays aligned with its data. */
    int list_offset;           /* records page window */
    int reminder_offset;       /* reminder page window */
} lanlan_records_view_t;

/* Reminder row: display text plus the due state the page shows. */
typedef struct {
    char row[64];
    bool due;
} lanlan_reminder_row_t;

void lanlan_model_init(lanlan_model_t *model);
void lanlan_model_clear(lanlan_model_t *model);
/* Copies the live cache into the model and clamps every selection. */
void lanlan_model_load_cache(lanlan_model_t *model, const lanlan_cache_t *cache);
/* Installs the local time and clock-trust flag used by the view helpers. */
void lanlan_model_set_clock(lanlan_model_t *model, const lanlan_local_now_t *now,
                            bool clock_trusted);
void lanlan_model_set_active(lanlan_model_t *model, bool active);
/* Returns the action the application worker must run. Always returns
 * LANLAN_ACTION_NONE for a gesture that only woke the display. */
lanlan_action_t lanlan_model_handle_key(lanlan_model_t *model, const lanlan_key_event_t *event);
/* Tab and list navigation for the top bar / reminder page, independent of the
 * four keys; exposed so the UI can implement swipe-free scrolling. */
void lanlan_model_set_page(lanlan_model_t *model, lanlan_page_t page);

/* ------------------------------------------------------------- invariant -- */

/* Single explicit invariant entry point: the passport never has a record action
 * pending. Returns false and stays false for every page and every key,
 * including the companion page's OK. */
bool lanlan_model_pending_record_action(const lanlan_model_t *model);
/* True when `action` is one of the forbidden record-writing actions. */
bool lanlan_action_is_record_write(lanlan_action_t action);

/* ----------------------------------------------------------- view model -- */

const lanlan_record_t *lanlan_view_record(const lanlan_records_view_t *view, int index);
const lanlan_reminder_t *lanlan_view_reminder(const lanlan_records_view_t *view, int index);
/* Row index of the selected record inside the visible list window. */
int lanlan_view_row_index(int list_offset, int index);
int lanlan_view_clamp_offset(int count, int offset, int rows);

/* Page title; "status" is used for an unknown page. */
const char *lanlan_view_page_title(lanlan_page_t page);
/* Settings row label by row index. */
const char *lanlan_view_settings_row_label(int row);
/* Home entry label by entry index. */
const char *lanlan_view_home_entry_label(int entry);
const char *lanlan_view_character_state_label(lanlan_character_t character);
/* Empty-state line for the record list ("no records yet"). */
const char *lanlan_view_records_empty_text(const lanlan_records_view_t *view);
/* Empty-state line for the reminder list. */
const char *lanlan_view_reminders_empty_text(const lanlan_records_view_t *view);

/* One record list row: "07:40 · 喂食 · 120 克". The category appears exactly
 * once; the optional second detail is a real sub-item, custom name, amount,
 * duration or the note-truncation marker, never the category's own label. A
 * missing record yields the empty-state text, so a stale selection is harmless. */
void lanlan_view_record_row(const lanlan_records_view_t *view, int index, char *out,
                            size_t out_size);
/* One reminder list row per visible slot, starting at view->reminder_offset so
 * output row i belongs to cache index reminder_offset + i. row[i].row carries
 * the identity and the configured time, plus the disabled marker when the
 * reminder is off; row[i].due is the separate flag for the due badge, which is
 * the only place the due state is rendered. */
void lanlan_view_reminder_rows(const lanlan_records_view_t *view, const lanlan_local_now_t *now,
                               lanlan_reminder_row_t *rows, size_t row_count);
/* Top-bar sync text: failure, never-synced, or the data age. */
void lanlan_view_sync_status_text(const lanlan_records_view_t *view, char *out, size_t out_size);
/* Detailed sync line with the last successful time and the record count. */
void lanlan_view_sync_detail_text(const lanlan_records_view_t *view, char *out, size_t out_size);
/* "78%" or the hidden placeholder when bsp_battery_soc() returned -1. */
void lanlan_view_battery_text(int battery_percent, char *out, size_t out_size);

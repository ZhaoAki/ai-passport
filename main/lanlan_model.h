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
/* Settings rows in list order. The writable rows come first and the read-only
 * information rows stay last, so a longer list of toggles never pushes the
 * information off the bottom of the page. */
typedef enum {
    LANLAN_SETTINGS_ROW_REFRESH = 0,
    LANLAN_SETTINGS_ROW_MUTE,
    LANLAN_SETTINGS_ROW_REMINDER_SOUND,
    LANLAN_SETTINGS_ROW_REMINDER_LIST,
    LANLAN_SETTINGS_ROW_DIM,
    LANLAN_SETTINGS_ROW_SCREEN_OFF,
    LANLAN_SETTINGS_ROW_TIMEZONE,
    LANLAN_SETTINGS_ROW_SYNC,
    LANLAN_SETTINGS_ROW_STORAGE,
    LANLAN_SETTINGS_ROW_COUNT
} lanlan_settings_row_t;

/* ------------------------------------------- idle timeout configuration -- */

/* Bounded step sets for the two backlight timeouts. The lists are the complete
 * set the device supports; the console may set the same values, and the UI
 * steps through them with lanlan_model_step_dim()/step_screen_off(). */
#define LANLAN_DIM_STEP_COUNT 4
#define LANLAN_SCREEN_OFF_STEP_COUNT 4
/* The documented relationship: the screen must never turn off before or at the
 * dim timeout, so screen_off_seconds is always strictly greater than
 * dim_seconds. Raising dim above the current screen-off value therefore pushes
 * screen_off up to the next allowed step (see lanlan_model_set_dim()); a step
 * with no room left is refused instead of breaking the invariant. */
#define LANLAN_TIMEOUT_DIM_DEFAULT_SECONDS 30
#define LANLAN_TIMEOUT_SCREEN_OFF_DEFAULT_SECONDS 90

/* Allowed values in seconds, ascending. */
extern const int LANLAN_DIM_STEPS_SECONDS[LANLAN_DIM_STEP_COUNT];
extern const int LANLAN_SCREEN_OFF_STEPS_SECONDS[LANLAN_SCREEN_OFF_STEP_COUNT];

/* Index of value in the step set, or -1. A value that is not an exact step is
 * not selectable through the UI. */
int lanlan_model_dim_step_index(int seconds);
int lanlan_model_screen_off_step_index(int seconds);

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

/* One de-bounced key event.
 *
 * `woke_screen` is informational: the caller sets it when its own display state
 * was off before the gesture, and may use it for backlight bookkeeping. The
 * model does NOT read it: the wake-only rule is decided inside the model from
 * `model->active`, so the rule cannot be duplicated, forgotten or bypassed by a
 * caller. A caller that wants the pause behaviour must therefore not consume the
 * gesture itself.
 *
 * Evidence for the "release of a waking long press delivers no click" half:
 * components/bsp/src/bsp_button.c registers exactly BUTTON_PRESS_DOWN,
 * BUTTON_SINGLE_CLICK, BUTTON_DOUBLE_CLICK and BUTTON_LONG_PRESS_START
 * (register_callbacks, bsp_button.c:118-125) with long_press_time =
 * BSP_BTN_LONG_PRESS_MS (500 ms, bsp_pins.h:62). In the pinned
 * espressif__button/iot_button.c state machine, release after a triggered long
 * press walks PRESS_LONG_PRESS_UP_CHECK -> BUTTON_LONG_PRESS_UP ->
 * BUTTON_PRESS_UP -> BUTTON_PRESS_END (iot_button.c:278-318); BUTTON_SINGLE_CLICK
 * is emitted only from the short-press release path PRESS_REPEAT_DOWN_CHECK
 * (iot_button.c:181-186). Since the BSP registers no callback for
 * BUTTON_LONG_PRESS_UP, a physical press that triggered BSP_BTN_LONG produces no
 * further event on release, so no click can follow it. */
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
    /* Backlight timeouts in seconds. They live here so the UI can render them
     * without reaching into NVS; the persistence blob stays with the app. */
    int dim_seconds;
    int screen_off_seconds;
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

/* ---------------------------------------------------- timeout selection -- */

/* Installs both timeouts. `dim` must be an allowed dim step smaller than
 * `screen_off`, and `screen_off` an allowed screen-off step; otherwise the pair
 * is rejected and nothing changes, so the model can never hold a configuration
 * the device would not accept. */
bool lanlan_model_set_timeouts(lanlan_model_t *model, int dim_seconds, int screen_off_seconds);
/* Steps the dim value one step up (dir > 0) or down (dir < 0), clamping at the
 * ends. Raising dim above the current screen-off value also raises screen_off
 * to the next allowed step, so screen_off stays strictly greater than dim.
 * Returns false when nothing changed: at a clamp end, or when the step would
 * need a screen-off value beyond the largest allowed step. */
bool lanlan_model_step_dim(lanlan_model_t *model, int direction);
/* Steps the screen-off value one step up/down, clamping at the ends and never
 * letting it fall to or below dim. Returns false when nothing changed. */
bool lanlan_model_step_screen_off(lanlan_model_t *model, int direction);
/* True when the pair satisfies the documented invariant. */
bool lanlan_model_timeouts_valid(int dim_seconds, int screen_off_seconds);

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
/* Value text per timeout row: "30 秒" or "3 分钟", drawn from the generated
 * table so the UI has no formatting logic. Each helper accepts only the steps
 * of its own row, because 60 and 120 seconds are valid for both rows but read
 * differently ("1 分钟" dim against "60 秒" screen-off). Returns "" for a value
 * that is not an allowed step for that row. */
const char *lanlan_view_dim_text(int seconds);
const char *lanlan_view_screen_off_text(int seconds);
/* Convenience lookup for a value whose row is unknown; prefer the two helpers
 * above when the row is known. */
const char *lanlan_view_timeout_text(int seconds);
/* Value text for a settings row, or "" when the row has no text value (the
 * refresh action and the reminder-list navigation). The caller passes the model
 * so the toggles and the timeout rows resolve through one helper. */
const char *lanlan_view_settings_row_value_text(const lanlan_model_t *model, int row);
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

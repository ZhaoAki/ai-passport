/* Cyber Lanlan LVGL screens. See lanlan_ui.h for the contract.
 *
 * Visual language: warm cream background, white rounded cards, charcoal text and
 * a biscuit accent, laid out for the 240x320 panel. The top bar carries the page
 * title, the battery reading and the sync state (a coloured dot plus text); the
 * bottom line describes the current key actions. Nothing is placed above y=0 or
 * below y=316, so no widget overlaps the status bar. */

#include "lanlan_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lanlan_record.h"
#include "lanlan_sprites.h"
#include "lanlan_strings.h"
#include "lanlan_time.h"

LV_FONT_DECLARE(lanlan_font_16);
LV_FONT_DECLARE(lanlan_font_24);

static const char *TAG = "lanlan_ui";

/* ----------------------------------------------------------- visual style -- */

#define UI_WIDTH 240
#define UI_HEIGHT 320

#define UI_COLOR_BG 0xFFF7E8u        /* warm cream */
#define UI_COLOR_CARD 0xFFFFFFu
#define UI_COLOR_SELECTED 0xE8A15Cu  /* biscuit */
#define UI_COLOR_INK 0x3B3733u       /* charcoal */
#define UI_COLOR_MUTED 0x8B8177u
#define UI_COLOR_BODY 0xFFFFFFu
#define UI_COLOR_WARN 0xC4762Bu

#define UI_STATUS_HEIGHT 50
#define UI_BODY_TOP 56
#define UI_BODY_BOTTOM 288
#define UI_HINT_Y 292

/* ASCII-only key hint: the frozen string table has no generic "keys" entry, and
 * no Chinese may be introduced outside it. */
#define UI_HINT_KEYS "UP/DN  OK  HOLD=BACK"

#define UI_ROW_CORNER 10
#define UI_CARD_CORNER 12

#define UI_SPRITE_TICK_MS 50
#define UI_IDLE_SWAP_MS 900u
#define UI_BLINK_HOLD_MS 180u
#define UI_HAPPY_HOLD_MS 900u
#define UI_BARK_HOLD_MS 700u

/* --------------------------------------------------------------- state -- */

static lv_obj_t *s_screen;
static lv_obj_t *s_canvas;
static uint16_t *s_sprite_buffer;
static lv_timer_t *s_timer;

/* Companion animation states. The model's lanlan_character_t covers the three
 * navigation-selectable states; bark is a UI-only frame used to draw attention
 * (a reminder became due) without inventing a model state. */
typedef enum {
    UI_ANIM_IDLE = 0,
    UI_ANIM_BLINK,
    UI_ANIM_HAPPY,
    UI_ANIM_BARK
} ui_anim_t;

/* Only touched while the LVGL lock is held, which both the application worker
 * and the LVGL task's timer handler observe. */
static ui_anim_t s_animation = UI_ANIM_IDLE;
static uint32_t s_animation_until;
static uint32_t s_next_idle_swap;
static uint32_t s_next_blink;
static uint8_t s_idle_frame;
static uint32_t s_frame_shown = 0xFFFFFFFFu;
static uint32_t s_random = 0x1234567u;

/* ------------------------------------------------------------- primitives -- */

static uint32_t random_next(void) {
    /* Small xorshift, seeded from the frame counter; only used for the blink
     * interval, so its statistical quality is irrelevant. */
    s_random ^= s_random << 13;
    s_random ^= s_random >> 17;
    s_random ^= s_random << 5;
    return s_random;
}

static lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color,
                          int radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return NULL;
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* Every text widget names its font explicitly. The generated subset covers
 * printable ASCII plus the fixed inventory, so it is used for both scripts. */
static lv_obj_t *ui_text(lv_obj_t *parent, int x, int y, int width, int height, const char *value,
                         const lv_font_t *font, uint32_t color, lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return NULL;
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, value ? value : "");
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static lv_obj_t *ui_wrapped(lv_obj_t *parent, int x, int y, int width, int height,
                            const char *value, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = ui_text(parent, x, y, width, height, value, font, color, LV_TEXT_ALIGN_LEFT);
    if (label) lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    return label;
}

static void ui_dot(lv_obj_t *parent, int x, int y, uint32_t color) {
    lv_obj_t *dot = ui_panel(parent, x, y, 10, 10, color, LV_RADIUS_CIRCLE);
    if (dot) lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
}

static const char *hint_for_page(lanlan_page_t page) {
    switch (page) {
    case LANLAN_PAGE_COMPANION: return LANLAN_STR_COMPANION_PET_HINT;
    case LANLAN_PAGE_STATUS: return LANLAN_STR_STATUS_RETRY_HINT;
    default: return UI_HINT_KEYS;
    }
}

/* ------------------------------------------------------------- status bar -- */

static void render_status_bar(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    lv_obj_t *bar = ui_panel(screen, 0, 0, UI_WIDTH, UI_STATUS_HEIGHT, UI_COLOR_BG, 0);
    if (!bar) return;
    ui_text(bar, 12, 4, 156, 32, lanlan_view_page_title(state->model->page), &lanlan_font_24,
            UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);
    if (state->battery_percent >= 0) {
        char battery[8];
        lanlan_view_battery_text(state->battery_percent, battery, sizeof(battery));
        if (battery[0] != '\0') {
            ui_text(bar, 176, 10, 54, 20, battery, &lanlan_font_16, UI_COLOR_MUTED,
                    LV_TEXT_ALIGN_RIGHT);
        }
    }
    ui_dot(bar, 14, 36, state->status_color);
    ui_text(bar, 30, 32, 200, 18, state->status_text, &lanlan_font_16, UI_COLOR_MUTED,
            LV_TEXT_ALIGN_LEFT);
}

static void render_hint(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    lv_obj_t *hint = ui_panel(screen, 0, UI_HINT_Y, UI_WIDTH, UI_HEIGHT - UI_HINT_Y, UI_COLOR_BG, 0);
    if (!hint) return;
    const char *text = state->banner_text ? state->banner_text
                                          : hint_for_page(state->model->page);
    ui_text(hint, 12, 2, 216, 20, text, &lanlan_font_16,
            state->banner_text ? UI_COLOR_WARN : UI_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
}

/* --------------------------------------------------------------- helpers -- */

static void format_offset(int16_t minutes, char *out, size_t out_size) {
    int total = minutes;
    char sign = '+';
    if (total < 0) {
        sign = '-';
        total = -total;
    }
    snprintf(out, out_size, "%c%02d:%02d", sign, total / 60, total % 60);
}

static int count_local_day(const lanlan_records_view_t *view, const lanlan_local_now_t *now) {
    if (!view || !now) return 0;
    int count = 0;
    for (uint32_t i = 0; i < view->cache.record_count; ++i) {
        const lanlan_record_t *record = &view->cache.records[i];
        if (record->status != LANLAN_STATUS_ACTIVE) continue;
        lanlan_date_t date;
        if (lanlan_time_local_date(record->occurred_epoch, record->occurred_tz_offset_min, &date)
            != LANLAN_TIME_OK) {
            continue;
        }
        if (date.day == now->local_day) ++count;
    }
    return count;
}

/* Short age band for the settings row; the full sentence lives in the sync
 * detail string used by the status page. */
static const char *short_age_band(const lanlan_records_view_t *view) {
    if (!view || view->last_sync_epoch <= 0) return LANLAN_STR_SYNC_NEVER;
    int64_t age = view->now_epoch - view->last_sync_epoch;
    if (age < 0) age = 0;
    if (age < 300) return LANLAN_STR_SYNC_JUST_NOW;
    if (age < 3600) return LANLAN_STR_SYNC_MINUTES_AGO;
    return LANLAN_STR_SYNC_HOURS_AGO;
}

/* ------------------------------------------------------------ home page -- */

static void render_home(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    const lanlan_records_view_t *view = state->view;
    lv_obj_t *summary = ui_panel(screen, 12, UI_BODY_TOP, 216, 56, UI_COLOR_CARD, UI_CARD_CORNER);
    char line[96];
    if (view && view->clock_trusted && state->now) {
        int today = count_local_day(view, state->now);
        if (today > 0) {
            snprintf(line, sizeof(line), "%s %d %s", LANLAN_STR_HOME_ENTRY_TODAY, today,
                     LANLAN_STR_SYNC_RECORDS_UNIT);
        } else {
            snprintf(line, sizeof(line), "%s", LANLAN_STR_RECORDS_EMPTY);
        }
        ui_text(summary, 12, 6, 192, 20, line, &lanlan_font_24, UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);
    } else {
        ui_text(summary, 12, 6, 192, 20, LANLAN_STR_HOME_CLOCK_UNTRUSTED, &lanlan_font_16,
                UI_COLOR_WARN, LV_TEXT_ALIGN_LEFT);
    }
    char row[128];
    if (view && view->cache.record_count > 0) {
        lanlan_view_record_row(view, 0, row, sizeof(row));
    } else {
        snprintf(row, sizeof(row), "%s", LANLAN_STR_RECORDS_EMPTY);
    }
    ui_text(summary, 12, 30, 192, 20, row, &lanlan_font_16, UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    if (state->cache_rebuilt) {
        ui_text(screen, 12, 116, 216, 18, LANLAN_STR_STATUS_CACHE_REBUILT, &lanlan_font_16,
                UI_COLOR_WARN, LV_TEXT_ALIGN_LEFT);
    } else if (state->storage_limited) {
        ui_text(screen, 12, 116, 216, 18, LANLAN_STR_ERRORS_STORAGE, &lanlan_font_16,
                UI_COLOR_WARN, LV_TEXT_ALIGN_LEFT);
    }

    for (unsigned i = 0; i < LANLAN_HOME_ENTRY_COUNT; ++i) {
        bool selected = state->model->home_entry == (lanlan_home_entry_t)i;
        lv_obj_t *entry = ui_panel(screen, 12, 120 + 42 * (int)i, 216, 38,
                                   selected ? UI_COLOR_SELECTED : UI_COLOR_CARD, UI_ROW_CORNER);
        if (!entry) continue;
        ui_text(entry, 14, 9, 188, 20, lanlan_view_home_entry_label((int)i), &lanlan_font_16,
                selected ? UI_COLOR_BODY : UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);
    }
}

/* --------------------------------------------------------- records page -- */

static void render_records(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    const lanlan_records_view_t *view = state->view;
    uint32_t count = view ? view->cache.record_count : 0;
    if (count == 0) {
        const char *text = (view && !view->clock_trusted)
                               ? LANLAN_STR_RECORDS_EMPTY_UNTRUSTED_CLOCK
                               : LANLAN_STR_RECORDS_EMPTY;
        ui_wrapped(screen, 12, UI_BODY_TOP + 40, 216, 60, text, &lanlan_font_16, UI_COLOR_MUTED);
        return;
    }
    for (unsigned i = 0; i < LANLAN_MODEL_LIST_ROWS; ++i) {
        int index = state->model->list_offset + (int)i;
        if (index >= (int)count) break;
        bool selected = index == state->model->record_index;
        lv_obj_t *row = ui_panel(screen, 12, UI_BODY_TOP + 46 * (int)i, 216, 42,
                                 selected ? UI_COLOR_SELECTED : UI_COLOR_CARD, UI_ROW_CORNER);
        if (!row) continue;
        char text[128];
        lanlan_view_record_row(view, index, text, sizeof(text));
        ui_wrapped(row, 12, 4, 192, 34, text, &lanlan_font_16,
                   selected ? UI_COLOR_BODY : UI_COLOR_INK);
    }
    if ((int)count > state->model->list_offset + LANLAN_MODEL_LIST_ROWS) {
        ui_text(screen, 12, 274, 216, 16, LANLAN_STR_RECORDS_MORE_ROWS, &lanlan_font_16,
                UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    }
}

/* ---------------------------------------------------------- detail page -- */

static void render_detail(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    const lanlan_records_view_t *view = state->view;
    const lanlan_record_t *record = lanlan_view_record(view, state->model->record_index);
    if (!record || record->version == 0) {
        ui_wrapped(screen, 12, UI_BODY_TOP + 40, 216, 60, LANLAN_STR_RECORDS_EMPTY,
                   &lanlan_font_16, UI_COLOR_MUTED);
        return;
    }
    lv_obj_t *card = ui_panel(screen, 12, UI_BODY_TOP, 216, 196, UI_COLOR_CARD, UI_CARD_CORNER);
    if (!card) return;

    /* Large enough for the category, the date, the time and a full custom name
     * without the compiler having to assume a truncation. */
    char text[192];
    char date[32];
    char time[16];
    lanlan_record_format_local_date(record, date, sizeof(date));
    lanlan_record_format_local_time(record, time, sizeof(time));
    snprintf(text, sizeof(text), "%s", lanlan_category_label(record->category));
    ui_text(card, 12, 8, 192, 30, text, &lanlan_font_24, UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);

    char sub[LANLAN_RECORD_CUSTOM_BYTES * 4];
    lanlan_record_subitem_label(record, sub, sizeof(sub));
    snprintf(text, sizeof(text), "%s %s  %s", date, time, sub);
    ui_text(card, 12, 42, 192, 20, text, &lanlan_font_16, UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);

    /* The compact view stops after the time and the sub-item; the full view adds
     * the quantity, the performer, the note and the revision marker. */
    if (!state->model->detail_full) {
        snprintf(text, sizeof(text), "%s  %s", LANLAN_STR_DETAIL_VIEW_COMPACT,
                 LANLAN_STR_RECORDS_OPEN_DETAIL);
        ui_text(card, 12, 158, 192, 20, text, &lanlan_font_16, UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    } else {
        char amount[48];
        char duration[48];
        lanlan_record_format_amount(record, amount, sizeof(amount));
        lanlan_record_format_duration(record, duration, sizeof(duration));
        if (duration[0] != '\0') {
            snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_DURATION, duration);
        } else {
            snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_AMOUNT, amount);
        }
        ui_text(card, 12, 68, 192, 20, text, &lanlan_font_16, UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);

        snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_PERFORMER,
                 lanlan_caregiver_name(record->performed_by));
        ui_text(card, 12, 90, 192, 20, text, &lanlan_font_16, UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);

        char note[80];
        lanlan_record_note_preview(record, note, sizeof(note));
        snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_NOTE, note);
        ui_wrapped(card, 12, 112, 192, 40, text, &lanlan_font_16, UI_COLOR_MUTED);

        if (record->version > 1) {
            snprintf(text, sizeof(text), "%s  %s %s", LANLAN_STR_DETAIL_REVISED,
                     LANLAN_STR_DETAIL_VIEW_FULL, time);
            ui_text(card, 12, 158, 192, 20, text, &lanlan_font_16, UI_COLOR_WARN,
                    LV_TEXT_ALIGN_LEFT);
        }
    }

    /* Two view tabs; the active one is filled. Both are 24 px tall so they never
     * reach the hint line. */
    const char *tabs[2] = {LANLAN_STR_DETAIL_VIEW_COMPACT, LANLAN_STR_DETAIL_VIEW_FULL};
    for (int i = 0; i < 2; ++i) {
        bool active = (state->model->detail_full ? 1 : 0) == i;
        lv_obj_t *tab = ui_panel(screen, 12 + 110 * i, 258, 106, 26,
                                 active ? UI_COLOR_SELECTED : UI_COLOR_CARD, UI_ROW_CORNER);
        if (!tab) continue;
        ui_text(tab, 4, 4, 98, 18, tabs[i], &lanlan_font_16,
                active ? UI_COLOR_BODY : UI_COLOR_INK, LV_TEXT_ALIGN_CENTER);
    }
}

/* ------------------------------------------------------- companion page -- */

/* Copies one 96x96 big-endian RGB565 frame into the single reused canvas
 * buffer. The canvas keeps native-endian pixels; the display port performs the
 * wire-level byte swap. */
static void sprite_blit(uint32_t frame_index) {
    if (!s_sprite_buffer || frame_index >= LANLAN_SPRITE_COUNT) return;
    const lanlan_sprite_t *frame = &lanlan_sprites[frame_index];
    if (!frame->pixels || frame->width != LANLAN_SPRITE_WIDTH
        || frame->height != LANLAN_SPRITE_HEIGHT) {
        return;
    }
    const uint8_t *source = frame->pixels;
    for (size_t i = 0; i < (size_t)LANLAN_SPRITE_WIDTH * LANLAN_SPRITE_HEIGHT; ++i) {
        s_sprite_buffer[i] = (uint16_t)(((uint16_t)source[2 * i] << 8) | source[2 * i + 1]);
    }
    s_frame_shown = frame_index;
    if (s_canvas) lv_obj_invalidate(s_canvas);
}

static uint32_t animation_frame(void) {
    switch (s_animation) {
    case UI_ANIM_BLINK: return LANLAN_SPRITE_BLINK;
    case UI_ANIM_HAPPY: return LANLAN_SPRITE_HAPPY;
    case UI_ANIM_BARK: return LANLAN_SPRITE_BARK;
    default: return s_idle_frame ? LANLAN_SPRITE_IDLE_1 : LANLAN_SPRITE_IDLE_0;
    }
}

/* Label shown under the sprite; bark reads as the happy reaction. */
static lanlan_character_t animation_label(void) {
    switch (s_animation) {
    case UI_ANIM_BLINK: return LANLAN_CHARACTER_BLINK;
    case UI_ANIM_HAPPY:
    case UI_ANIM_BARK: return LANLAN_CHARACTER_HAPPY;
    default: return LANLAN_CHARACTER_IDLE;
    }
}

static void animation_tick(uint32_t now) {
    if (s_animation != UI_ANIM_IDLE && (int32_t)(now - s_animation_until) >= 0) {
        s_animation = UI_ANIM_IDLE;
        s_next_idle_swap = now + UI_IDLE_SWAP_MS;
        s_next_blink = now + 2000u + (random_next() % 4000u);
    }
    if (s_animation == UI_ANIM_IDLE) {
        if ((int32_t)(now - s_next_idle_swap) >= 0) {
            s_idle_frame = s_idle_frame ? 0u : 1u;
            s_next_idle_swap = now + UI_IDLE_SWAP_MS;
        }
        if (s_next_blink == 0) s_next_blink = now + 2000u;
        if ((int32_t)(now - s_next_blink) >= 0) {
            s_animation = UI_ANIM_BLINK;
            s_animation_until = now + UI_BLINK_HOLD_MS;
            s_next_blink = now + 2500u + (random_next() % 4500u);
        }
    }
    uint32_t frame = animation_frame();
    if (frame != s_frame_shown) sprite_blit(frame);
}

static void sprite_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!s_canvas) return;
    animation_tick(lv_tick_get());
}

static void render_companion(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    if (!lanlan_ui_pet_invariant_held(state->model)) {
        ESP_LOGE(TAG, "companion page reached with a pending record action");
    }
    lv_obj_t *card = ui_panel(screen, 60, 60, 120, 132, UI_COLOR_CARD, UI_CARD_CORNER);
    if (!card) return;
    s_canvas = lv_canvas_create(card);
    if (!s_canvas || !s_sprite_buffer) {
        s_canvas = NULL;
        return;
    }
    lv_canvas_set_buffer(s_canvas, s_sprite_buffer, LANLAN_SPRITE_WIDTH, LANLAN_SPRITE_HEIGHT,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 12, 18);
    lv_obj_set_size(s_canvas, LANLAN_SPRITE_WIDTH, LANLAN_SPRITE_HEIGHT);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    s_frame_shown = 0xFFFFFFFFu;
    sprite_blit(animation_frame());

    ui_text(screen, 12, 198, 216, 22, lanlan_view_character_state_label(animation_label()),
            &lanlan_font_24, UI_COLOR_INK, LV_TEXT_ALIGN_CENTER);
    ui_text(screen, 12, 226, 216, 20, LANLAN_STR_COMPANION_PET_HINT, &lanlan_font_16,
            UI_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
}

/* -------------------------------------------------------- settings page -- */

static void render_settings(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    const lanlan_records_view_t *view = state->view;
    char value[48];
    for (unsigned i = 0; i < LANLAN_SETTINGS_ROW_COUNT; ++i) {
        bool selected = state->model->settings_row == (lanlan_settings_row_t)i;
        lv_obj_t *row = ui_panel(screen, 12, UI_BODY_TOP + 32 * (int)i, 216, 30,
                                 selected ? UI_COLOR_SELECTED : UI_COLOR_CARD, UI_ROW_CORNER);
        if (!row) continue;
        ui_text(row, 12, 5, 130, 20, lanlan_view_settings_row_label((int)i), &lanlan_font_16,
                selected ? UI_COLOR_BODY : UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);

        value[0] = '\0';
        switch ((lanlan_settings_row_t)i) {
        case LANLAN_SETTINGS_ROW_MUTE:
            snprintf(value, sizeof(value), "%s",
                     state->model->globally_muted ? LANLAN_STR_SETTINGS_VALUE_ON
                                                  : LANLAN_STR_SETTINGS_VALUE_OFF);
            break;
        case LANLAN_SETTINGS_ROW_REMINDER_SOUND:
            snprintf(value, sizeof(value), "%s",
                     state->model->reminder_sound_enabled ? LANLAN_STR_SETTINGS_VALUE_ON
                                                          : LANLAN_STR_SETTINGS_VALUE_OFF);
            break;
        case LANLAN_SETTINGS_ROW_TIMEZONE: {
            char zone[10];
            format_offset(state->model->utc_offset_min, zone, sizeof(zone));
            snprintf(value, sizeof(value), "%s", zone);
            break;
        }
        case LANLAN_SETTINGS_ROW_SYNC:
            snprintf(value, sizeof(value), "%s", short_age_band(view));
            break;
        case LANLAN_SETTINGS_ROW_STORAGE:
            snprintf(value, sizeof(value), "%u %s",
                     (unsigned)(view ? view->cache.record_count : 0u),
                     LANLAN_STR_SYNC_RECORDS_UNIT);
            break;
        default: break;
        }
        if (value[0] != '\0') {
            ui_text(row, 142, 5, 66, 20, value, &lanlan_font_16,
                    selected ? UI_COLOR_BODY : UI_COLOR_MUTED, LV_TEXT_ALIGN_RIGHT);
        }
    }
    if (state->storage_limited || (view && !view->clock_trusted)) {
        ui_text(screen, 12, 282, 216, 16,
                state->storage_limited ? LANLAN_STR_ERRORS_STORAGE
                                       : LANLAN_STR_HOME_CLOCK_UNTRUSTED,
                &lanlan_font_16, UI_COLOR_WARN, LV_TEXT_ALIGN_LEFT);
    }
}

/* -------------------------------------------------------- reminders page -- */

static void render_reminders(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    lanlan_reminder_row_t rows[LANLAN_MODEL_LIST_ROWS];
    memset(rows, 0, sizeof(rows));
    if (!state->view || state->view->cache.reminder_count == 0) {
        ui_wrapped(screen, 12, UI_BODY_TOP + 40, 216, 60, LANLAN_STR_REMINDERS_EMPTY,
                   &lanlan_font_16, UI_COLOR_MUTED);
        return;
    }
    lanlan_view_reminder_rows(state->view, state->now, rows, LANLAN_MODEL_LIST_ROWS);
    for (unsigned i = 0; i < LANLAN_MODEL_LIST_ROWS; ++i) {
        int index = state->model->reminder_offset + (int)i;
        if (index >= (int)state->view->cache.reminder_count) break;
        if (rows[i].row[0] == '\0') continue;
        bool selected = index == state->model->reminder_index;
        lv_obj_t *row = ui_panel(screen, 12, UI_BODY_TOP + 40 * (int)i, 216, 36,
                                 selected ? UI_COLOR_SELECTED : UI_COLOR_CARD, UI_ROW_CORNER);
        if (!row) continue;
        ui_text(row, 10, 8, 148, 20, rows[i].row, &lanlan_font_16,
                selected ? UI_COLOR_BODY : UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);
        if (rows[i].due) {
            ui_text(row, 160, 8, 48, 20, LANLAN_STR_REMINDERS_DUE, &lanlan_font_16,
                    selected ? UI_COLOR_BODY : UI_COLOR_WARN, LV_TEXT_ALIGN_RIGHT);
        }
    }
    ui_wrapped(screen, 12, 256, 216, 32, LANLAN_STR_REMINDERS_DISMISS_NOTE, &lanlan_font_16,
               UI_COLOR_MUTED);
    if (!state->model->reminder_sound_enabled) {
        ui_text(screen, 12, 236, 216, 16, LANLAN_STR_REMINDERS_SOUND_OFF, &lanlan_font_16,
                UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    }
}

/* ----------------------------------------------------------- status page -- */

static void render_status(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    lv_obj_t *card = ui_panel(screen, 12, UI_BODY_TOP, 216, 150, UI_COLOR_CARD, UI_CARD_CORNER);
    if (!card) return;
    ui_dot(card, 14, 18, state->status_color);
    ui_text(card, 32, 12, 172, 22, state->status_text ? state->status_text : "", &lanlan_font_16,
            UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);
    if (state->cache_rebuilt) {
        ui_wrapped(card, 14, 46, 188, 40, LANLAN_STR_STATUS_CACHE_REBUILT, &lanlan_font_16,
                   UI_COLOR_WARN);
    } else if (state->storage_limited) {
        ui_wrapped(card, 14, 46, 188, 40, LANLAN_STR_ERRORS_STORAGE, &lanlan_font_16,
                   UI_COLOR_WARN);
    }
    if (state->status_detail) {
        ui_wrapped(card, 14, 92, 188, 44, state->status_detail, &lanlan_font_16, UI_COLOR_MUTED);
    }
    ui_text(screen, 12, 216, 216, 20, LANLAN_STR_STATUS_RETRY_HINT, &lanlan_font_16,
            UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    if (!state->secure_url) {
        ui_text(screen, 12, 240, 216, 20, "http:// (LAN dev)", &lanlan_font_16, UI_COLOR_WARN,
                LV_TEXT_ALIGN_LEFT);
    }
    char detail[128];
    lanlan_view_sync_detail_text(state->view, detail, sizeof(detail));
    ui_wrapped(screen, 12, 262, 216, 24, detail, &lanlan_font_16, UI_COLOR_MUTED);
}

/* ----------------------------------------------------------- entry points -- */

bool lanlan_ui_pet_invariant_held(const lanlan_model_t *model) {
    if (lanlan_model_pending_record_action(model)) {
        ESP_LOGE(TAG, "invariant violated: a pending record action exists");
        return false;
    }
    return true;
}

bool lanlan_ui_companion_visible(void) {
    return s_canvas != NULL;
}

void lanlan_ui_companion_react(lanlan_character_t state) {
    uint32_t now = lv_tick_get();
    switch (state) {
    case LANLAN_CHARACTER_BLINK: s_animation = UI_ANIM_BLINK; break;
    case LANLAN_CHARACTER_HAPPY: s_animation = UI_ANIM_HAPPY; break;
    default: s_animation = UI_ANIM_IDLE; break;
    }
    s_animation_until = now + UI_HAPPY_HOLD_MS;
    if (s_canvas) sprite_blit(animation_frame());
}

void lanlan_ui_companion_bark(void) {
    if (!s_canvas) return;
    s_animation = UI_ANIM_BARK;
    s_animation_until = lv_tick_get() + UI_BARK_HOLD_MS;
    sprite_blit(animation_frame());
}

static void ui_font_audit(void) {
    /* Proves the generated subsets cover every fixed string the UI binds, as
     * docs/development/engineering/lvgl-chinese-fonts.md requires. Logs one line
     * per missing code point instead of leaving a silent box storm. */
    const lv_font_t *const fonts[2] = {&lanlan_font_16, &lanlan_font_24};
    const char *const names[2] = {"16", "24"};
    size_t missing = 0;
    for (size_t f = 0; f < 2; ++f) {
      const lv_font_t *font = fonts[f];
      for (int id = 0; id < LANLAN_STR_COUNT; ++id) {
        const char *text = lanlan_strings[id];
        if (!text) continue;
        for (size_t i = 0; text[i] != '\0';) {
            unsigned char lead = (unsigned char)text[i];
            uint32_t codepoint = lead;
            size_t length = lanlan_utf8_char_len(lead);
            if (length == 1) {
                codepoint = lead;
            } else {
                codepoint = (uint32_t)(lead & (0xFFu >> (length + 1)));
                for (size_t k = 1; k < length && text[i + k] != '\0'; ++k) {
                    codepoint = (codepoint << 6) | (uint32_t)(text[i + k] & 0x3Fu);
                }
            }
            if (codepoint < 0x20) {
                i += length;
                continue;
            }
            lv_font_glyph_dsc_t dsc;
            memset(&dsc, 0, sizeof(dsc));
            if (!lv_font_get_glyph_dsc(font, &dsc, codepoint, 0) || dsc.is_placeholder) {
                ESP_LOGW(TAG, "font %s gap: U+%04X in string id %d", names[f],
                         (unsigned)codepoint, id);
                ++missing;
            }
            i += length;
        }
      }
    }
    ESP_LOGI(TAG, "font audit: %u missing glyphs over %d strings x 2 sizes", (unsigned)missing,
             (int)LANLAN_STR_COUNT);
}

esp_err_t lanlan_ui_init(lv_obj_t **screen_out) {
    if (!screen_out) return ESP_ERR_INVALID_ARG;
    if (s_screen) {
        *screen_out = s_screen;
        return ESP_OK;
    }
    s_sprite_buffer = heap_caps_malloc(LANLAN_SPRITE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_sprite_buffer) return ESP_ERR_NO_MEM;
    s_screen = lv_obj_create(NULL);
    if (!s_screen) {
        heap_caps_free(s_sprite_buffer);
        s_sprite_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_font(s_screen, &lanlan_font_16, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    s_random = (uint32_t)lv_tick_get() | 1u;
    s_next_blink = lv_tick_get() + 2000u;
    if (!s_timer) {
        s_timer = lv_timer_create(sprite_timer_cb, UI_SPRITE_TICK_MS, NULL);
    }
    ui_font_audit();
    *screen_out = s_screen;
    return ESP_OK;
}

void lanlan_ui_render(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    if (!screen || !state || !state->model) return;
    /* The canvas object is deleted with the other children, so clear the
     * pointer before lv_obj_clean(); the animation state survives while the
     * companion page stays visible. */
    s_canvas = NULL;
    s_frame_shown = 0xFFFFFFFFu;
    if (state->model->page != LANLAN_PAGE_COMPANION) s_animation = UI_ANIM_IDLE;
    lv_obj_clean(screen);
    render_status_bar(screen, state);
    switch (state->model->page) {
    case LANLAN_PAGE_HOME: render_home(screen, state); break;
    case LANLAN_PAGE_RECORDS: render_records(screen, state); break;
    case LANLAN_PAGE_DETAIL: render_detail(screen, state); break;
    case LANLAN_PAGE_COMPANION: render_companion(screen, state); break;
    case LANLAN_PAGE_SETTINGS: render_settings(screen, state); break;
    case LANLAN_PAGE_REMINDERS: render_reminders(screen, state); break;
    case LANLAN_PAGE_STATUS:
    default: render_status(screen, state); break;
    }
    render_hint(screen, state);
}

void lanlan_ui_deinit(void) {
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_canvas = NULL;
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    if (s_sprite_buffer) {
        heap_caps_free(s_sprite_buffer);
        s_sprite_buffer = NULL;
    }
}

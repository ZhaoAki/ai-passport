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

/* Height a wrapped label will occupy inside `width`, taken from LVGL's own text
 * metrics so the status card can be sized to its content instead of to a guess. */
static int ui_wrapped_height(const char *text, int width) {
    if (!text || text[0] == '\0') return 0;
    lv_point_t size;
    memset(&size, 0, sizeof(size));
    lv_text_get_size(&size, text, &lanlan_font_16, 0, 0, (int32_t)width, LV_TEXT_FLAG_NONE);
    return (int)size.y;
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

/* ------------------------------------------------------------ glyph check -- */

/* Advances one UTF-8 character; false at the end of the string. Malformed bytes
 * count as one character each, which is what the coverage check needs. */
static bool ui_next_codepoint(const char **cursor, uint32_t *codepoint) {
    const unsigned char *bytes = (const unsigned char *)*cursor;
    if (bytes[0] == '\0') return false;
    size_t length = lanlan_utf8_char_len(bytes[0]);
    uint32_t value = bytes[0];
    if (length > 1) {
        value = (uint32_t)(bytes[0] & (0xFFu >> (length + 1)));
        for (size_t k = 1; k < length; ++k) {
            if (bytes[k] == '\0') break;
            value = (value << 6) | (uint32_t)(bytes[k] & 0x3Fu);
        }
    }
    *codepoint = value;
    *cursor += (length == 0) ? 1 : length;
    return true;
}

/* The doc's coverage check: a code point counts as covered only when the font
 * answers and does not mark it as a placeholder. */
static bool ui_font_covers(const lv_font_t *font, const char *text) {
    if (!font || !text) return false;
    const char *cursor = text;
    uint32_t codepoint = 0;
    while (ui_next_codepoint(&cursor, &codepoint)) {
        if (codepoint < 0x20u) continue;
        lv_font_glyph_dsc_t desc;
        memset(&desc, 0, sizeof(desc));
        if (!lv_font_get_glyph_dsc(font, &desc, codepoint, 0) || desc.is_placeholder) {
            return false;
        }
    }
    return true;
}

/* A caregiver display name is service data, so it can contain a code point the
 * generated subsets do not cover. Drawing placeholder boxes for a person's name
 * would be worse than admitting the gap, so the neutral label from the string
 * table is used and the missing code point is logged once, never silently. */
static const char *ui_caregiver_label(const lanlan_ui_state_t *state, unsigned index) {
    const char *name = lanlan_caregiver_name_at(state->caregivers, index);
    if (ui_font_covers(&lanlan_font_16, name)) return name;
    static uint32_t reported[4];
    const char *cursor = name;
    uint32_t codepoint = 0;
    while (ui_next_codepoint(&cursor, &codepoint)) {
        bool covered = true;
        if (codepoint >= 0x20u) {
            lv_font_glyph_dsc_t desc;
            memset(&desc, 0, sizeof(desc));
            covered = lv_font_get_glyph_dsc(&lanlan_font_16, &desc, codepoint, 0)
                      && !desc.is_placeholder;
        }
        if (covered) continue;
        bool seen = false;
        for (size_t i = 0; i < 4; ++i) {
            if (reported[i] == codepoint) seen = true;
        }
        if (seen) continue;
        for (size_t i = 0; i < 4; ++i) {
            if (reported[i] == 0) {
                reported[i] = codepoint;
                break;
            }
        }
        ESP_LOGW(TAG, "caregiver name needs an uncovered glyph U+%04X; showing the neutral label",
                 (unsigned)codepoint);
    }
    return lanlan_caregiver_fallback_label();
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
    lv_obj_t *card = ui_panel(screen, 12, UI_BODY_TOP, 216, 202, UI_COLOR_CARD, UI_CARD_CORNER);
    if (!card) return;

    /* Large enough for the category, the date, the time and a full custom name
     * without the compiler having to assume a truncation. */
    char text[192];
    char date[32];
    char time[16];
    lanlan_record_format_local_date(record, date, sizeof(date));
    lanlan_record_format_local_time(record, time, sizeof(time));

    /* The category is the 24 px title, and the line under it is the local date and
     * time only: no screen prints the category twice. */
    snprintf(text, sizeof(text), "%s", lanlan_category_label(record->category));
    ui_text(card, 12, 2, 192, 30, text, &lanlan_font_24, UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);
    snprintf(text, sizeof(text), "%s %s", date, time);
    ui_text(card, 12, 34, 192, 20, text, &lanlan_font_16, UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);

    /* Both views resolve the caregiver through the learned directory, so a name
     * the service changed is picked up and an unknown one shows the neutral
     * label instead of a slot number. */
    if (!state->model->detail_full) {
        snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_PERFORMER,
                 ui_caregiver_label(state, record->performed_by));
        ui_text(card, 12, 150, 192, 20, text, &lanlan_font_16, UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    } else {
        /* Row order follows the design's field list: time, performer, quantity,
         * note, revision marker. */
        snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_PERFORMER,
                 ui_caregiver_label(state, record->performed_by));
        ui_text(card, 12, 60, 192, 20, text, &lanlan_font_16, UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);

        char amount[48];
        char duration[48];
        lanlan_record_format_amount(record, amount, sizeof(amount));
        lanlan_record_format_duration(record, duration, sizeof(duration));
        if (duration[0] != '\0') {
            snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_DURATION, duration);
        } else {
            snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_AMOUNT, amount);
        }
        ui_text(card, 12, 86, 192, 20, text, &lanlan_font_16, UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);

        snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_CREATOR,
                 ui_caregiver_label(state, record->created_by));
        ui_text(card, 12, 112, 192, 20, text, &lanlan_font_16, UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);

        /* The whole truncation marker must be visible. The preview is at most 48
         * bytes plus the 7-glyph marker: 536 px even when every byte is a
         * half-width character, which is well inside three 212 px lines, so a
         * 60 px box (three 20 px font lines) can never clip it. The note is the
         * only label that uses the card's full width, which keeps the marker from
         * being split off on its own line. */
        char note[80];
        lanlan_record_note_preview(record, note, sizeof(note));
        snprintf(text, sizeof(text), "%s %s", LANLAN_STR_DETAIL_NOTE, note);
        ui_wrapped(card, 2, 138, 212, 60, text, &lanlan_font_16, UI_COLOR_MUTED);
    }

    /* Two view tabs; the card ends above them and they never reach the hint bar. */
    const char *tabs[2] = {LANLAN_STR_DETAIL_VIEW_COMPACT, LANLAN_STR_DETAIL_VIEW_FULL};
    for (int i = 0; i < 2; ++i) {
        bool active = (state->model->detail_full ? 1 : 0) == i;
        lv_obj_t *tab = ui_panel(screen, 12 + 110 * i, 262, 106, 22,
                                 active ? UI_COLOR_SELECTED : UI_COLOR_CARD, UI_ROW_CORNER);
        if (!tab) continue;
        ui_text(tab, 4, 2, 98, 18, tabs[i], &lanlan_font_16,
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
        /* 32 px rows keep five of them plus the two footer lines inside the
         * 56..288 body band. */
        lv_obj_t *row = ui_panel(screen, 12, UI_BODY_TOP + 34 * (int)i, 216, 32,
                                 selected ? UI_COLOR_SELECTED : UI_COLOR_CARD, UI_ROW_CORNER);
        if (!row) continue;
        ui_text(row, 10, 6, 148, 20, rows[i].row, &lanlan_font_16,
                selected ? UI_COLOR_BODY : UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);
        if (rows[i].due) {
            ui_text(row, 160, 6, 48, 20, LANLAN_STR_REMINDERS_DUE, &lanlan_font_16,
                    selected ? UI_COLOR_BODY : UI_COLOR_WARN, LV_TEXT_ALIGN_RIGHT);
        }
    }
    if (!state->model->reminder_sound_enabled) {
        ui_text(screen, 12, 226, 216, 20, LANLAN_STR_REMINDERS_SOUND_OFF, &lanlan_font_16,
                UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    }
    /* The note is 20 characters, which is two 216 px lines, and its box ends
     * exactly at the body bottom (288): four pixels above the hint-bar band, so
     * the second line keeps its descenders and never touches the hint line. */
    ui_wrapped(screen, 12, 248, 216, 40, LANLAN_STR_REMINDERS_DISMISS_NOTE, &lanlan_font_16,
               UI_COLOR_MUTED);
}

/* ----------------------------------------------------------- status page -- */

static void render_status(lv_obj_t *screen, const lanlan_ui_state_t *state) {
    const lanlan_records_view_t *view = state->view;
    const char *notice = state->cache_rebuilt      ? LANLAN_STR_STATUS_CACHE_REBUILT
                         : state->storage_limited  ? LANLAN_STR_ERRORS_STORAGE
                                                   : NULL;
    const char *detail = state->status_detail;
    const char *host = (state->service_host && state->service_host[0] != '\0')
                           ? state->service_host
                           : NULL;
    const bool insecure = !state->secure_url;

    /* Two bounded, single-line facts. The service cursor is deliberately absent:
     * it is an internal number, so it stays in the `lanlan status` console output
     * rather than in the owner-facing text. */
    char sync_line[96];
    if (view && view->last_sync_epoch > 0) {
        lanlan_date_t date;
        int hour = 0;
        int minute = 0;
        if (lanlan_time_local_date(view->last_sync_epoch, view->utc_offset_min, &date)
                == LANLAN_TIME_OK
            && lanlan_time_local_hhmm(view->last_sync_epoch, view->utc_offset_min, &hour,
                                      &minute) == LANLAN_TIME_OK) {
            snprintf(sync_line, sizeof(sync_line), "%s %d%s%d%s %02d:%02d",
                     LANLAN_STR_SYNC_DETAIL, (int)date.month,
                     LANLAN_STR_QUANTITY_DATE_MONTH_UNIT, (int)date.day_in_month,
                     LANLAN_STR_QUANTITY_DATE_DAY_UNIT, hour, minute);
        } else {
            snprintf(sync_line, sizeof(sync_line), "%s %s", LANLAN_STR_SYNC_DETAIL,
                     LANLAN_STR_SYNC_NEVER);
        }
    } else {
        snprintf(sync_line, sizeof(sync_line), "%s %s", LANLAN_STR_SYNC_DETAIL,
                 LANLAN_STR_SYNC_NEVER);
    }
    char cache_line[64];
    snprintf(cache_line, sizeof(cache_line), "%s %u %s", LANLAN_STR_SYNC_CACHE_COUNT,
             (unsigned)(view ? view->cache.record_count : 0u), LANLAN_STR_SYNC_RECORDS_UNIT);

    /* Size the card to exactly the rows that will be drawn, so it never shows a
     * large empty body. Wrapped rows are measured with the font metrics. */
    const int notice_height = notice ? ui_wrapped_height(notice, 188) : 0;
    const int detail_height = detail ? ui_wrapped_height(detail, 188) : 0;
    int height = 8 + 24;
    if (notice) height += 6 + notice_height;
    if (detail) height += 6 + detail_height;
    height += 8 + (20 + 4) + (20 + 4);
    if (host) height += 20 + 4;
    if (insecure) height += 20 + 4;
    height += 8;
    const int max_height = UI_BODY_BOTTOM - UI_BODY_TOP;
    if (height > max_height) height = max_height;

    lv_obj_t *card = ui_panel(screen, 12, UI_BODY_TOP, 216, height, UI_COLOR_CARD,
                              UI_CARD_CORNER);
    if (!card) return;
    int y = 8;
    ui_dot(card, 14, y + 3, state->status_color);
    /* Wide enough for the longest state line the application can pass (the
     * credential-rejected instruction), so it is not cut with DOTS. */
    ui_text(card, 32, y, 180, 22, state->status_text ? state->status_text : "", &lanlan_font_16,
            UI_COLOR_INK, LV_TEXT_ALIGN_LEFT);
    y += 24;
    if (notice) {
        y += 6;
        ui_wrapped(card, 14, y, 188, notice_height, notice, &lanlan_font_16, UI_COLOR_WARN);
        y += notice_height;
    }
    if (detail) {
        y += 6;
        ui_wrapped(card, 14, y, 188, detail_height, detail, &lanlan_font_16, UI_COLOR_MUTED);
        y += detail_height;
    }
    y += 8;
    ui_text(card, 12, y, 204, 20, sync_line, &lanlan_font_16, UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    y += 24;
    ui_text(card, 12, y, 204, 20, cache_line, &lanlan_font_16, UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    y += 24;
    if (host) {
        ui_text(card, 12, y, 204, 20, host, &lanlan_font_16, UI_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
        y += 24;
    }
    if (insecure) {
        ui_text(card, 12, y, 204, 20, "http:// (LAN dev)", &lanlan_font_16, UI_COLOR_WARN,
                LV_TEXT_ALIGN_LEFT);
    }
    /* The retry instruction belongs to the bottom hint line only, which this page
     * already shows, so it is deliberately not repeated here. */
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
            const char *cursor = text;
            uint32_t codepoint = 0;
            while (ui_next_codepoint(&cursor, &codepoint)) {
                if (codepoint < 0x20u) continue;
                lv_font_glyph_dsc_t desc;
                memset(&desc, 0, sizeof(desc));
                if (!lv_font_get_glyph_dsc(font, &desc, codepoint, 0) || desc.is_placeholder) {
                    ESP_LOGW(TAG, "font %s gap: U+%04X in string id %d", names[f],
                             (unsigned)codepoint, id);
                    ++missing;
                }
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

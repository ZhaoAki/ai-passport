/* Host render harness for the REAL Cyber Lanlan application UI.
 *
 * This program compiles main/lanlan_ui.c unchanged against the pinned managed
 * LVGL component plus the ESP-IDF stub headers in tests/lanlan_ui/stubs/, feeds
 * it a realistic synthetic cache and writes one PPM per screen. It is the
 * rendering evidence behind the "240x320 rendering" acceptance item: nothing
 * here reimplements the UI, so a screen that looks wrong on the host looks
 * wrong on the panel too.
 *
 * Besides the captures it audits glyph coverage:
 *   - the frozen inventory (main/lanlan_strings.h via lanlan_strings[] plus the
 *     fixed record labels, which include the note truncation marker) must be
 *     covered by BOTH lanlan_font_16 and lanlan_font_24;
 *   - every code point of every string the view model produces and every code
 *     point of every label the screen actually renders is checked against the
 *     font the widget uses;
 *   - a code point outside the inventory (an emoji) must resolve to the LVGL
 *     placeholder, never to a real glyph.
 * A code point that is in the frozen inventory and has no glyph is a hard
 * failure (non-zero exit). A code point outside it is caller-supplied free
 * text, which the design renders with the placeholder on purpose; it is
 * reported as FREE-FORM and does not fail the run. */
#include "lanlan_ui.h"

#include "lanlan_cache.h"
#include "lanlan_caregiver.h"
#include "lanlan_model.h"
#include "lanlan_record.h"
#include "lanlan_reminder.h"
#include "lanlan_strings.h"
#include "lanlan_time.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lanlan_font_16);
LV_FONT_DECLARE(lanlan_font_24);

#define UI_W 240
#define UI_H 320
#define UI_TZ_OFFSET_MIN ((int16_t)480) /* UTC+8, the design's default family zone */
#define UI_REQUIRED_CAPACITY 1024

static uint16_t s_frame[UI_W * UI_H];
static uint16_t s_buffer[UI_W * 40];
static lv_display_t *s_display;
static lv_obj_t *s_screen;
static const char *s_output_dir;

static unsigned s_captures;
static unsigned s_renders;
static unsigned s_labels;
static unsigned s_missing;
static unsigned s_freeform;
static unsigned s_layout_warnings;

static int64_t s_today; /* local day index of the synthetic "today" */

/* ------------------------------------------------------------ display I/O -- */

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const uint16_t *data = (const uint16_t *)pixels;
    unsigned stride = (unsigned)(area->x2 - area->x1 + 1);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) continue;
            s_frame[(size_t)y * UI_W + (size_t)x] =
                data[(size_t)(y - area->y1) * stride + (size_t)(x - area->x1)];
        }
    }
    lv_display_flush_ready(display);
}

static void write_ppm(const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fprintf(file, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (size_t i = 0; i < (size_t)UI_W * UI_H; ++i) {
        uint16_t value = s_frame[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(((value >> 11) & 0x1Fu) * 255u / 31u);
        rgb[1] = (unsigned char)(((value >> 5) & 0x3Fu) * 255u / 63u);
        rgb[2] = (unsigned char)((value & 0x1Fu) * 255u / 31u);
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

/* -------------------------------------------------------------- UTF-8 walk -- */

static uint32_t next_codepoint(const unsigned char **cursor) {
    uint32_t cp = *(*cursor)++;
    if (cp < 0x80) return cp;
    unsigned continuation = cp < 0xE0 ? 1u : cp < 0xF0 ? 2u : 3u;
    cp &= (continuation == 1u) ? 0x1Fu : (continuation == 2u) ? 0x0Fu : 0x07u;
    while (continuation--) {
        if ((**cursor & 0xC0u) != 0x80u) return 0xFFFDu;
        cp = (cp << 6) | (uint32_t)(*(*cursor)++ & 0x3Fu);
    }
    return cp;
}

/* ------------------------------------------------------- required inventory -- */

static uint32_t s_required[UI_REQUIRED_CAPACITY];
static size_t s_required_count;

static void required_add(uint32_t cp) {
    if (cp < 0x20u || (cp >= 0x7Fu && cp < 0xA0u)) return;
    for (size_t i = 0; i < s_required_count; ++i) {
        if (s_required[i] == cp) return;
    }
    if (s_required_count >= UI_REQUIRED_CAPACITY) {
        fprintf(stderr, "required inventory overflow\n");
        exit(1);
    }
    s_required[s_required_count++] = cp;
}

static bool required_has(uint32_t cp) {
    for (size_t i = 0; i < s_required_count; ++i) {
        if (s_required[i] == cp) return true;
    }
    return false;
}

static void required_add_text(const char *text) {
    if (!text) return;
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) required_add(next_codepoint(&cursor));
}

static void required_collect(void) {
    for (int id = 0; id < LANLAN_STR_COUNT; ++id) required_add_text(lanlan_strings[id]);
    for (size_t i = 0; i < sizeof(LANLAN_RECORD_FIXED_LABELS) / sizeof(LANLAN_RECORD_FIXED_LABELS[0]);
         ++i) {
        required_add_text(LANLAN_RECORD_FIXED_LABELS[i]);
    }
}

/* ------------------------------------------------------------ glyph audit -- */

static bool glyph_covered(const lv_font_t *font, uint32_t cp) {
    lv_font_glyph_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    return lv_font_get_glyph_dsc(font, &dsc, cp, 0) && !dsc.is_placeholder;
}

static void audit_text(const lv_font_t *font, const char *font_name, const char *text,
                       const char *context) {
    if (!text) return;
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        uint32_t cp = next_codepoint(&cursor);
        if (cp < 0x20u || cp == 0x7Fu) continue;
        if (glyph_covered(font, cp)) continue;
        if (required_has(cp)) {
            printf("MISSING U+%04X in font %s (%s)\n", (unsigned)cp, font_name, context);
            ++s_missing;
        } else {
            printf("FREE-FORM U+%04X has no glyph in font %s; placeholder drawn (%s)\n",
                   (unsigned)cp, font_name, context);
            ++s_freeform;
        }
    }
}

static void audit_both(const char *text, const char *context) {
    audit_text(&lanlan_font_16, "lanlan_font_16", text, context);
    audit_text(&lanlan_font_24, "lanlan_font_24", text, context);
}

static void audit_string_table(void) {
    for (int id = 0; id < LANLAN_STR_COUNT; ++id) {
        char context[128];
        snprintf(context, sizeof(context), "lanlan_strings[%d] key '%s'", id,
                 lanlan_string_keys[id]);
        audit_both(lanlan_strings[id], context);
    }
    for (size_t i = 0; i < sizeof(LANLAN_RECORD_FIXED_LABELS) / sizeof(LANLAN_RECORD_FIXED_LABELS[0]);
         ++i) {
        char context[64];
        snprintf(context, sizeof(context), "LANLAN_RECORD_FIXED_LABELS[%u]", (unsigned)i);
        audit_both(LANLAN_RECORD_FIXED_LABELS[i], context);
    }
    printf("glyph audit: %u required code points from the frozen inventory\n",
           (unsigned)s_required_count);
}

static void audit_view_strings(const lanlan_records_view_t *view, const lanlan_local_now_t *now,
                               const lanlan_caregiver_table_t *caregivers,
                               const lanlan_caregiver_table_t *fallback_caregivers) {
    char context[128];
    char text[256];

    for (int page = LANLAN_PAGE_HOME; page <= LANLAN_PAGE_STATUS; ++page) {
        snprintf(context, sizeof(context), "page title %d", page);
        audit_both(lanlan_view_page_title((lanlan_page_t)page), context);
    }
    for (int row = 0; row < LANLAN_SETTINGS_ROW_COUNT; ++row) {
        snprintf(context, sizeof(context), "settings row label %d", row);
        audit_both(lanlan_view_settings_row_label(row), context);
    }
    for (int entry = 0; entry < LANLAN_HOME_ENTRY_COUNT; ++entry) {
        snprintf(context, sizeof(context), "home entry label %d", entry);
        audit_both(lanlan_view_home_entry_label(entry), context);
    }
    for (int character = 0; character <= LANLAN_CHARACTER_COUNT; ++character) {
        snprintf(context, sizeof(context), "companion state label %d", character);
        audit_both(lanlan_view_character_state_label((lanlan_character_t)character), context);
    }
    for (uint32_t index = 0; index < view->cache.record_count; ++index) {
        lanlan_view_record_row(view, (int)index, text, sizeof(text));
        snprintf(context, sizeof(context), "records row %u", (unsigned)index);
        audit_both(text, context);
    }
    lanlan_reminder_row_t rows[LANLAN_MODEL_LIST_ROWS];
    memset(rows, 0, sizeof(rows));
    lanlan_view_reminder_rows(view, now, rows, LANLAN_MODEL_LIST_ROWS);
    for (size_t i = 0; i < LANLAN_MODEL_LIST_ROWS; ++i) {
        snprintf(context, sizeof(context), "reminder row %u", (unsigned)i);
        audit_both(rows[i].row, context);
    }
    lanlan_view_sync_status_text(view, text, sizeof(text));
    audit_both(text, "sync status text");
    lanlan_view_sync_detail_text(view, text, sizeof(text));
    audit_both(text, "sync detail text");
    const int batteries[] = {-1, 0, 7, 55, 78, 100};
    for (size_t i = 0; i < sizeof(batteries) / sizeof(batteries[0]); ++i) {
        lanlan_view_battery_text(batteries[i], text, sizeof(text));
        snprintf(context, sizeof(context), "battery text %d", batteries[i]);
        audit_both(text, context);
    }
    for (uint32_t index = 0; index < view->cache.record_count; ++index) {
        const lanlan_record_t *record = &view->cache.records[index];
        char subitem[LANLAN_RECORD_CUSTOM_BYTES * 4];
        lanlan_record_subitem_label(record, subitem, sizeof(subitem));
        snprintf(context, sizeof(context), "record %u subitem", (unsigned)index);
        audit_both(subitem, context);
        lanlan_record_format_amount(record, text, sizeof(text));
        snprintf(context, sizeof(context), "record %u amount", (unsigned)index);
        audit_both(text, context);
        lanlan_record_format_duration(record, text, sizeof(text));
        snprintf(context, sizeof(context), "record %u duration", (unsigned)index);
        audit_both(text, context);
        lanlan_record_format_local_time(record, text, sizeof(text));
        snprintf(context, sizeof(context), "record %u local time", (unsigned)index);
        audit_both(text, context);
        lanlan_record_format_local_date(record, text, sizeof(text));
        snprintf(context, sizeof(context), "record %u local date", (unsigned)index);
        audit_both(text, context);
        lanlan_record_note_preview(record, text, sizeof(text));
        snprintf(context, sizeof(context), "record %u note preview", (unsigned)index);
        audit_both(text, context);
    }
    /* Caregiver display names are service data. The UI replaces an uncovered
     * name with the neutral label, so the raw directory entries are audited
     * here and an out-of-inventory name is reported as free-form, not missing. */
    const lanlan_caregiver_table_t *tables[2] = {caregivers, fallback_caregivers};
    const char *const table_names[2] = {"caregiver directory", "fallback caregiver directory"};
    for (size_t t = 0; t < 2; ++t) {
        if (!tables[t]) continue;
        for (unsigned index = 0; index < LANLAN_CAREGIVER_SLOTS; ++index) {
            snprintf(context, sizeof(context), "%s slot %u", table_names[t], index);
            audit_both(lanlan_caregiver_name_at(tables[t], index), context);
        }
        snprintf(context, sizeof(context), "%s unknown id", table_names[t]);
        audit_both(lanlan_caregiver_name_for_id(tables[t], "no-such-member"), context);
    }
    audit_both(lanlan_caregiver_fallback_label(), "neutral caregiver label");
}

static void inspect(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        ++s_labels;
        const char *text = lv_label_get_text(obj);
        const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        const char *font_name = (font == &lanlan_font_24)   ? "lanlan_font_24"
                                : (font == &lanlan_font_16) ? "lanlan_font_16"
                                                            : "unexpected-font";
        audit_text(font, font_name, text, "rendered label");
        lv_area_t area;
        lv_obj_get_coords(obj, &area);
        if (area.x1 < 0 || area.x2 >= UI_W || area.y1 < 0 || area.y2 >= UI_H) {
            printf("LAYOUT label outside screen: '%s' (%d,%d)-(%d,%d)\n", text, (int)area.x1,
                   (int)area.y1, (int)area.x2, (int)area.y2);
            ++s_layout_warnings;
        }
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if (parent) {
            lv_area_t bounds;
            lv_obj_get_coords(parent, &bounds);
            if (area.x1 < bounds.x1 || area.x2 > bounds.x2 || area.y1 < bounds.y1
                || area.y2 > bounds.y2) {
                printf("LAYOUT label outside parent: '%s' (%d,%d)-(%d,%d) not inside "
                       "(%d,%d)-(%d,%d)\n",
                       text, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2,
                       (int)bounds.x1, (int)bounds.y1, (int)bounds.x2, (int)bounds.y2);
                ++s_layout_warnings;
            }
        }
    }
    uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; ++i) inspect(lv_obj_get_child(obj, i));
}

/* --------------------------------------------------------------- capturing -- */

static void capture(const char *name, const lanlan_ui_state_t *state) {
    lanlan_ui_render(s_screen, state);
    lv_obj_update_layout(s_screen);
    inspect(s_screen);
    lv_refr_now(s_display);
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    if (memory.free_size < 4096) {
        fprintf(stderr, "LVGL heap almost exhausted after '%s': %u bytes free\n", name,
                (unsigned)memory.free_size);
        exit(1);
    }
    if (!name) {
        ++s_renders;
        return;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", s_output_dir, name);
    write_ppm(path);
    printf("capture %s (%dx%d)\n", name, UI_W, UI_H);
    ++s_captures;
    ++s_renders;
}

/* ----------------------------------------------------------- synthetic data -- */

static int64_t at_local(int64_t day, int hour, int minute) {
    int64_t epoch = 0;
    assert(lanlan_time_from_local(day, hour, minute, UI_TZ_OFFSET_MIN, &epoch) == LANLAN_TIME_OK);
    return epoch;
}

static void add_record(lanlan_cache_t *cache, unsigned seed, int64_t epoch, lanlan_category_t category,
                       lanlan_subitem_t subitem, lanlan_unit_t unit, bool amount_known,
                       double amount, uint16_t duration_minutes, uint8_t created_by,
                       uint8_t performed_by, const char *custom_name, const char *note,
                       uint32_t version, lanlan_time_confidence_t confidence,
                       lanlan_record_status_t status) {
    assert(cache->record_count < LANLAN_CACHE_MAX_RECORDS);
    lanlan_record_t *record = &cache->records[cache->record_count++];
    memset(record, 0, sizeof(*record));
    record->occurred_epoch = epoch;
    record->created_epoch = epoch + 120;
    record->id[0] = (uint8_t)seed;
    record->id[1] = (uint8_t)(seed ^ 0x33u);
    record->id[2] = (uint8_t)(seed * 7u);
    record->id[15] = 0xA5u;
    record->seq = 1000u + seed;
    record->version = version;
    record->occurred_tz_offset_min = UI_TZ_OFFSET_MIN;
    record->category = category;
    record->subitem = subitem;
    record->duration_minutes = duration_minutes;
    record->created_by = created_by;
    record->performed_by = performed_by;
    record->time_confidence = confidence;
    record->status = status;
    if (custom_name) lanlan_record_set_custom_name(record, custom_name);
    if (note) lanlan_record_set_note(record, note);
    if (amount_known) {
        lanlan_record_set_amount(record, amount, unit);
    } else {
        lanlan_record_set_amount_unknown(record);
    }
}

static int find_by_seed(const lanlan_cache_t *cache, unsigned seed) {
    for (uint32_t i = 0; i < cache->record_count; ++i) {
        if (cache->records[i].id[0] == (uint8_t)seed
            && cache->records[i].id[1] == (uint8_t)(seed ^ 0x33u)) {
            return (int)i;
        }
    }
    return -1;
}

/* The cache models a revocation as "remove the record, keep its id in the
 * tombstone ring", so a revoked record can never appear in a consistent cache
 * and the UI has no revoked screen to render. These three probes pin that rule
 * with the real cache code instead of pretending a tombstone has a view. */
static void audit_tombstones(void) {
    lanlan_cache_t probe;
    lanlan_cache_clear(&probe);
    add_record(&probe, 200, at_local(s_today, 9, 0), LANLAN_CAT_MEAL, LANLAN_SUB_NONE,
               LANLAN_UNIT_G, true, 60.0, 0, 0, 0, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    if (!lanlan_cache_is_consistent(&probe)) {
        printf("TOMBSTONE CHECK FAILED: the synthetic probe record is not cache-consistent\n");
        ++s_missing;
        return;
    }
    printf("tombstone check: one live record is a consistent cache\n");

    uint8_t id[LANLAN_ID_BYTES];
    memcpy(id, probe.records[0].id, LANLAN_ID_BYTES);

    lanlan_cache_t both = probe;
    lanlan_cache_add_tombstone(&both, id);
    if (lanlan_cache_is_consistent(&both)) {
        printf("TOMBSTONE CHECK FAILED: a live record whose id is also tombstoned was accepted\n");
        ++s_missing;
    } else {
        printf("tombstone check: a live record whose id is also tombstoned is rejected\n");
    }

    lanlan_cache_t removed;
    lanlan_cache_clear(&removed);
    lanlan_cache_add_tombstone(&removed, id);
    if (!lanlan_cache_is_consistent(&removed)) {
        printf("TOMBSTONE CHECK FAILED: the post-revocation cache shape was rejected\n");
        ++s_missing;
    } else {
        printf("tombstone check: the post-revocation shape (tombstone kept, record gone) is a "
               "consistent cache\n");
    }
}

#define NOTE_TRUNCATED \
    "今天喂食喝水洗澡梳毛刷牙剪指甲清猫砂遛狗擦脚垫掏耳朵今天喂食喝水洗澡梳毛刷牙剪指甲清猫砂"

static void build_cache(lanlan_cache_t *cache) {
    lanlan_cache_clear(cache);
    int64_t today = s_today;
    int64_t yesterday = today - 1;

    /* Newest first after the sort below. The first record is the one the detail
     * screens open, so it carries a known amount, a revised version and the
     * note that must trip the 48-byte preview truncation. */
    add_record(cache, 1, at_local(today, 19, 40), LANLAN_CAT_MEAL, LANLAN_SUB_NONE, LANLAN_UNIT_G,
               true, 120.0, 0, 0, 0, NULL, NOTE_TRUNCATED, 3, LANLAN_TIME_CONFIDENCE_TRUSTED,
               LANLAN_STATUS_ACTIVE);
    add_record(cache, 2, at_local(today, 18, 5), LANLAN_CAT_MEAL, LANLAN_SUB_NONE,
               LANLAN_UNIT_NONE, false, 0.0, 0, 1, 1, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 3, at_local(today, 16, 20), LANLAN_CAT_WATER, LANLAN_SUB_NONE, LANLAN_UNIT_ML,
               true, 150.0, 0, 0, 0, NULL, "喝完了整碗", 1, LANLAN_TIME_CONFIDENCE_TRUSTED,
               LANLAN_STATUS_ACTIVE);
    add_record(cache, 4, at_local(today, 15, 0), LANLAN_CAT_WATER, LANLAN_SUB_NONE,
               LANLAN_UNIT_NONE, false, 0.0, 0, 1, 0, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 5, at_local(today, 12, 30), LANLAN_CAT_CARE, LANLAN_SUB_BATH,
               LANLAN_UNIT_NONE, false, 0.0, 0, 1, 1, NULL, "洗完澡了", 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 6, at_local(today, 11, 0), LANLAN_CAT_CARE, LANLAN_SUB_GROOMING,
               LANLAN_UNIT_NONE, false, 0.0, 0, 0, 1, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 7, at_local(yesterday, 21, 10), LANLAN_CAT_CARE, LANLAN_SUB_TEETH,
               LANLAN_UNIT_NONE, false, 0.0, 0, 0, 0, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_ESTIMATED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 8, at_local(yesterday, 20, 0), LANLAN_CAT_CARE, LANLAN_SUB_COMB,
               LANLAN_UNIT_NONE, false, 0.0, 0, 1, 1, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 9, at_local(yesterday, 17, 45), LANLAN_CAT_CARE, LANLAN_SUB_CARE_OTHER,
               LANLAN_UNIT_NONE, false, 0.0, 0, 0, 0, "擦脚", NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 10, at_local(yesterday, 9, 15), LANLAN_CAT_CLEANING, LANLAN_SUB_EAR,
               LANLAN_UNIT_NONE, false, 0.0, 0, 0, 0, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 11, at_local(yesterday, 8, 50), LANLAN_CAT_CLEANING, LANLAN_SUB_PAW,
               LANLAN_UNIT_NONE, false, 0.0, 0, 1, 0, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 12, at_local(today - 2, 19, 0), LANLAN_CAT_CLEANING, LANLAN_SUB_PAD,
               LANLAN_UNIT_NONE, false, 0.0, 0, 0, 0, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 13, at_local(today - 2, 18, 30), LANLAN_CAT_CLEANING, LANLAN_SUB_LITTER,
               LANLAN_UNIT_NONE, false, 0.0, 0, 1, 1, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 14, at_local(today - 2, 10, 0), LANLAN_CAT_CLEANING,
               LANLAN_SUB_CLEANING_OTHER, LANLAN_UNIT_NONE, false, 0.0, 0, 0, 0, "洗垫", NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 25, at_local(today - 3, 17, 20), LANLAN_CAT_WALK, LANLAN_SUB_NONE,
               LANLAN_UNIT_NONE, false, 0.0, 25, 0, 1, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 15, at_local(today - 3, 8, 40), LANLAN_CAT_WALK, LANLAN_SUB_NONE,
               LANLAN_UNIT_NONE, false, 0.0, 0, 1, 1, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 99, at_local(today - 4, 14, 0), LANLAN_CAT_OTHER, LANLAN_SUB_NONE,
               LANLAN_UNIT_NONE, false, 0.0, 0, 0, 0, "遛猫", "今天洗澡\U0001F600", 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);
    add_record(cache, 16, at_local(today - 5, 7, 30), LANLAN_CAT_MEAL, LANLAN_SUB_NONE,
               LANLAN_UNIT_G, true, 60.0, 0, 1, 0, NULL, NULL, 1,
               LANLAN_TIME_CONFIDENCE_TRUSTED, LANLAN_STATUS_ACTIVE);

    cache->cursor = 4711;

    /* Two revocation tombstones. The design removes a revoked record from the
     * cache and keeps only its id, so a tombstone has no visible view of its
     * own; carrying two proves the cache shape the UI renders against is the
     * post-revocation one. Their ids deliberately match no live record. */
    cache->tombstone_count = 2;
    memset(cache->tombstones, 0, sizeof(cache->tombstones));
    cache->tombstones[0][0] = 0xE0u;
    cache->tombstones[0][15] = 0x01u;
    cache->tombstones[1][0] = 0xE1u;
    cache->tombstones[1][15] = 0x02u;

    lanlan_cache_sort_records(cache, NULL);
}

static void add_reminder(lanlan_cache_t *cache, unsigned seed, lanlan_category_t category,
                         lanlan_subitem_t subitem, const char *custom_name,
                         const char *time_local, bool enabled, int64_t last_rung_day) {
    assert(cache->reminder_count < LANLAN_CACHE_MAX_REMINDERS);
    lanlan_reminder_t *reminder = &cache->reminders[cache->reminder_count++];
    memset(reminder, 0, sizeof(*reminder));
    reminder->id[0] = (uint8_t)(0x80u + seed);
    reminder->id[1] = (uint8_t)(seed ^ 0x5Cu);
    reminder->id[15] = 0x5Au;
    reminder->version = 1;
    reminder->category = category;
    reminder->subitem = subitem;
    reminder->enabled = enabled;
    reminder->last_rung_day = last_rung_day;
    if (custom_name) {
        lanlan_record_t wrapper;
        memset(&wrapper, 0, sizeof(wrapper));
        lanlan_record_set_custom_name(&wrapper, custom_name);
        memcpy(reminder->custom_name, wrapper.custom_name, sizeof(reminder->custom_name));
    }
    if (time_local) memcpy(reminder->time_local, time_local, strlen(time_local) + 1);
}

/* Seven reminders: three due, one already rung today, one without a schedule,
 * one disabled and one whose time is still ahead. */
static void build_reminders(lanlan_cache_t *cache) {
    add_reminder(cache, 1, LANLAN_CAT_MEAL, LANLAN_SUB_NONE, NULL, "08:00", true,
                 LANLAN_REMINDER_NO_RUNG);
    add_reminder(cache, 2, LANLAN_CAT_WATER, LANLAN_SUB_NONE, NULL, "12:00", true,
                 LANLAN_REMINDER_NO_RUNG);
    add_reminder(cache, 3, LANLAN_CAT_CARE, LANLAN_SUB_TEETH, NULL, "21:00", true,
                 LANLAN_REMINDER_NO_RUNG);
    add_reminder(cache, 4, LANLAN_CAT_CLEANING, LANLAN_SUB_LITTER, NULL, "18:00", true, s_today);
    add_reminder(cache, 5, LANLAN_CAT_CARE, LANLAN_SUB_COMB, NULL, "", true,
                 LANLAN_REMINDER_NO_RUNG);
    add_reminder(cache, 6, LANLAN_CAT_WALK, LANLAN_SUB_NONE, NULL, "07:30", false,
                 LANLAN_REMINDER_NO_RUNG);
    add_reminder(cache, 7, LANLAN_CAT_OTHER, LANLAN_SUB_NONE, "遛猫", "09:00", true,
                 LANLAN_REMINDER_NO_RUNG);
}

/* The two real family members the service reports, learned by id. */
static void build_caregivers(lanlan_caregiver_table_t *table) {
    lanlan_caregiver_clear(table);
    assert(lanlan_caregiver_learn(table, "member-hehe-0001", LANLAN_STR_CAREGIVERS_HEHE) == 0);
    assert(lanlan_caregiver_learn(table, "member-yangyang-0002", LANLAN_STR_CAREGIVERS_YANGYANG) == 1);
}

/* A directory whose only name contains a code point outside the generated
 * subset: the UI must render the neutral label, never a placeholder box. Slot 1
 * stays unused, which also renders the neutral label. */
static void build_fallback_caregivers(lanlan_caregiver_table_t *table) {
    lanlan_caregiver_clear(table);
    assert(lanlan_caregiver_learn(table, "member-xiaoming-0003", "小明") == 0);
}

static void build_view(lanlan_records_view_t *view, const lanlan_cache_t *cache, bool clock_trusted,
                       int battery_percent) {
    memset(view, 0, sizeof(*view));
    view->cache = *cache;
    view->clock_trusted = clock_trusted;
    view->utc_offset_min = UI_TZ_OFFSET_MIN;
    view->last_sync_epoch = clock_trusted ? at_local(s_today, 19, 55) : 0;
    view->now_epoch = clock_trusted ? at_local(s_today, 20, 30) : 0;
    view->cursor = cache->cursor;
    view->battery_percent = battery_percent;
}

/* -------------------------------------------------------------------- main -- */

int main(int argc, char **argv) {
    assert(argc == 2);
    s_output_dir = argv[1];
    s_today = lanlan_time_days_from_civil(2026, 10, 4);

    required_collect();
    audit_string_table();

    /* Negative control: a code point outside the frozen inventory must resolve
     * to the placeholder, never to a real glyph. */
    const uint32_t outside[] = {0x1F600u, 0x20ACu};
    const lv_font_t *const fonts[2] = {&lanlan_font_16, &lanlan_font_24};
    const char *const font_names[2] = {"lanlan_font_16", "lanlan_font_24"};
    for (size_t f = 0; f < 2; ++f) {
        for (size_t i = 0; i < sizeof(outside) / sizeof(outside[0]); ++i) {
            if (required_has(outside[i])) continue;
            if (glyph_covered(fonts[f], outside[i])) {
                printf("PLACEHOLDER CHECK FAILED: U+%04X is not in the inventory but font %s "
                       "claims a real glyph\n",
                       (unsigned)outside[i], font_names[f]);
                ++s_missing;
            } else {
                printf("placeholder check: U+%04X outside the inventory -> placeholder in %s\n",
                       (unsigned)outside[i], font_names[f]);
            }
        }
    }

    lv_init();
    s_display = lv_display_create(UI_W, UI_H);
    assert(s_display);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_buffer, NULL, sizeof(s_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush);

    lv_obj_t *screen = NULL;
    assert(lanlan_ui_init(&screen) == ESP_OK);
    assert(screen != NULL);
    s_screen = screen;
    lv_screen_load(s_screen);

    lanlan_cache_t cache;
    build_cache(&cache);
    build_reminders(&cache);
    assert(cache.record_count == 18);
    assert(cache.reminder_count == 7);
    audit_tombstones();

    int walk_index = find_by_seed(&cache, 25);
    int fallback_index = find_by_seed(&cache, 99);
    assert(walk_index >= 0);
    assert(fallback_index >= 0);

    lanlan_local_now_t now;
    now.local_day = s_today;
    now.hour = 20;
    now.minute = 30;

    lanlan_records_view_t view;
    build_view(&view, &cache, true, 78);

    lanlan_caregiver_table_t caregivers;
    lanlan_caregiver_table_t fallback_caregivers;
    build_caregivers(&caregivers);
    build_fallback_caregivers(&fallback_caregivers);

    audit_view_strings(&view, &now, &caregivers, &fallback_caregivers);

    lanlan_model_t model;
    lanlan_model_init(&model);
    lanlan_model_load_cache(&model, &cache);
    lanlan_model_set_clock(&model, &now, true);
    model.home_entry = LANLAN_HOME_ENTRY_TODAY;
    model.settings_row = LANLAN_SETTINGS_ROW_REFRESH;
    model.record_index = 0;
    model.list_offset = 0;
    model.detail_full = 0;
    model.reminder_index = 0;
    model.reminder_offset = 0;
    model.character = LANLAN_CHARACTER_IDLE;
    model.globally_muted = false;
    model.reminder_sound_enabled = true;
    model.utc_offset_min = UI_TZ_OFFSET_MIN;

    lanlan_ui_state_t state;
    memset(&state, 0, sizeof(state));
    state.model = &model;
    state.view = &view;
    state.now = &now;
    state.caregivers = &caregivers;
    state.battery_percent = 78;
    state.status_text = LANLAN_STR_STATUS_OK;
    state.status_color = 0x3AA76Du;
    state.status_detail = NULL;
    state.cache_rebuilt = false;
    state.storage_limited = false;
    state.secure_url = true;
    state.banner_text = NULL;

    /* The seven screens, in the design's table order, with the two detail
     * views as separate captures. */
    model.page = LANLAN_PAGE_HOME;
    capture("01-home", &state);

    model.page = LANLAN_PAGE_RECORDS;
    capture("02-records", &state);

    model.page = LANLAN_PAGE_DETAIL;
    model.detail_full = 0;
    capture("03-detail-compact", &state);

    model.detail_full = 1;
    capture("04-detail-full", &state);

    model.page = LANLAN_PAGE_COMPANION;
    capture("05-companion", &state);

    model.page = LANLAN_PAGE_SETTINGS;
    model.settings_row = LANLAN_SETTINGS_ROW_SYNC;
    capture("06-settings", &state);

    model.page = LANLAN_PAGE_REMINDERS;
    capture("07-reminders", &state);

    model.page = LANLAN_PAGE_STATUS;
    state.status_detail = NULL;
    state.battery_percent = -1; /* unavailable reading: hidden, not drawn as 0% */
    capture("08-status", &state);

    /* Extra evidence the seven-screen list does not cover on its own. */
    state.status_detail = NULL;
    state.battery_percent = 78;

    model.page = LANLAN_PAGE_DETAIL;
    model.detail_full = 1;
    model.record_index = walk_index;
    capture("09-detail-full-walk", &state);

    model.record_index = fallback_index;
    capture("10-detail-note-fallback", &state);

    /* A caregiver display name the generated subsets do not cover, plus an
     * unused slot: both must show the neutral label. */
    state.caregivers = &fallback_caregivers;
    model.record_index = 0;
    capture("14-detail-caregiver-fallback", &state);
    state.caregivers = &caregivers;

    model.record_index = 0;
    model.page = LANLAN_PAGE_HOME;
    model.home_entry = LANLAN_HOME_ENTRY_RECORDS;
    view.clock_trusted = false;
    view.last_sync_epoch = 0;
    view.now_epoch = 0;
    lanlan_model_set_clock(&model, &now, false);
    capture("11-home-clock-untrusted", &state);

    /* Empty cache: the untrusted-clock empty list, then the plain empty home. */
    lanlan_cache_t empty_cache;
    lanlan_cache_clear(&empty_cache);
    lanlan_records_view_t empty_view;
    build_view(&empty_view, &empty_cache, false, -1);
    lanlan_model_t empty_model;
    lanlan_model_init(&empty_model);
    lanlan_model_load_cache(&empty_model, &empty_cache);
    lanlan_model_set_clock(&empty_model, &now, false);
    lanlan_ui_state_t empty_state = state;
    empty_state.model = &empty_model;
    empty_state.view = &empty_view;
    empty_state.battery_percent = -1;
    empty_model.page = LANLAN_PAGE_RECORDS;
    capture("12-records-empty-untrusted", &empty_state);
    empty_model.page = LANLAN_PAGE_HOME;
    capture("13-home-empty", &empty_state);

    /* Status page error variants. */
    model.page = LANLAN_PAGE_STATUS;
    state.status_text = LANLAN_STR_STATUS_OFFLINE;
    state.status_color = 0xC4762Bu;
    state.secure_url = false; /* the LAN development URL warning line */
    capture("15-status-offline-http", &state);

    state.status_text = LANLAN_STR_STATUS_CREDENTIAL_REJECTED;
    state.status_color = 0xB54A3Cu;
    state.secure_url = true;
    state.status_detail = LANLAN_STR_ERRORS_SERVER;
    state.cache_rebuilt = true;
    capture("16-status-cache-rebuilt", &state);

    state.status_text = LANLAN_STR_STATUS_OK;
    state.status_color = 0x3AA76Du;
    state.status_detail = NULL;
    state.secure_url = true;
    state.cache_rebuilt = false;
    state.battery_percent = 78;

    /* The partial-refresh path: several full rebuilds in a row on the same
     * screen prove the in-place rebuild does not leak LVGL heap. */
    for (unsigned i = 0; i < 60; ++i) {
        model.page = (lanlan_page_t)(i % 7);
        capture(NULL, &state);
    }

    printf("lanlan_ui_preview: %u captures, %u renders, %u labels checked, %u required-glyph "
           "misses, %u free-form placeholders, %u layout warnings\n",
           s_captures, s_renders, s_labels, s_missing, s_freeform, s_layout_warnings);
    printf("fonts: lanlan_font_16=%s lanlan_font_24=%s (LVGL %d.%d.%d)\n",
           lanlan_font_16.get_glyph_dsc ? "ok" : "missing",
           lanlan_font_24.get_glyph_dsc ? "ok" : "missing", LVGL_VERSION_MAJOR,
           LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    if (s_freeform == 0) {
        printf("note: every checked code point was covered by the generated subsets\n");
    } else {
        printf("note: %u checked code point(s) fell outside the frozen inventory and used the "
               "documented placeholder/neutral-label path\n",
               s_freeform);
    }

    lanlan_ui_deinit();
    lv_deinit();
    return s_missing == 0 ? 0 : 1;
}

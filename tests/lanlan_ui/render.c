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
 * reported as FREE-FORM and does not fail the run.
 *
 * Usage:
 *
 *   lanlan_ui_preview <output-dir>            # 16 PPM screen captures (default)
 *   lanlan_ui_preview <output-dir> --stress   # captures + the A12 stress run
 *
 * The stress run is the host-only half of acceptance item A12 ("stability with
 * the network, audio and animation active"): it drives the seven screens and the
 * real lanlan_model_handle_key() state machine for >= 500 page switches and
 * >= 1000 key events with LVGL rendering on, samples the LVGL pool, the process
 * heap and the live object count before/during/after, and exits non-zero when
 * the free pool, the largest free block or the live object count does not return
 * to the pre-run baseline. The animation path is kept live by calling
 * lanlan_ui_companion_react()/lanlan_ui_companion_bark() and lv_timer_handler()
 * while the companion page is shown. Every number it prints is a HOST
 * measurement of the ESP-IDF-independent UI and model code, not a device
 * measurement. */
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
#include <time.h>

#include <sys/resource.h>

#if defined(__APPLE__)
#include <malloc/malloc.h> /* malloc_zone_statistics: the mallinfo equivalent here */
#elif defined(__GLIBC__)
#include <malloc.h> /* mallinfo2 / mallinfo */
#endif

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
static lanlan_records_view_t *s_view; /* mutable: the stress run keeps it in step with the model */
static const char *s_output_dir;

static unsigned s_captures;
static unsigned s_renders;
static unsigned s_labels;
static unsigned s_missing;
static unsigned s_freeform;
static unsigned s_layout_warnings;
static unsigned s_layout_printed;
static unsigned s_flushes; /* display flush callbacks: proves pixels were rendered */
static bool s_audit_labels = true; /* glyph audit on every inspect() */

static int64_t s_today; /* local day index of the synthetic "today" */

/* ------------------------------------------------------------ host clock -- */

/* The firmware feeds lv_tick_inc() from a periodic timer. On the host nothing
 * advances LVGL's internal counter, so the sprite timer would never fire and the
 * companion animation would stay frozen. A monotonic millisecond callback makes
 * lv_tick_get() behave like the device. It does not change the captures: the
 * screens are rendered synchronously and no screen starts an LVGL animation. */
static uint32_t host_tick_cb(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
}

/* ------------------------------------------------------------ display I/O -- */

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const uint16_t *data = (const uint16_t *)pixels;
    unsigned stride = (unsigned)(area->x2 - area->x1 + 1);
    ++s_flushes;
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
                               const lanlan_caregiver_table_t *fallback_caregivers,
                               const lanlan_model_t *model) {
    char context[128];
    char text[256];

    for (int page = LANLAN_PAGE_HOME; page <= LANLAN_PAGE_STATUS; ++page) {
        snprintf(context, sizeof(context), "page title %d", page);
        audit_both(lanlan_view_page_title((lanlan_page_t)page), context);
    }
    for (int row = 0; row < LANLAN_SETTINGS_ROW_COUNT; ++row) {
        snprintf(context, sizeof(context), "settings row label %d", row);
        audit_both(lanlan_view_settings_row_label(row), context);
        snprintf(context, sizeof(context), "settings row value %d", row);
        audit_both(lanlan_view_settings_row_value_text(model, row), context);
    }
    for (int i = 0; i < LANLAN_DIM_STEP_COUNT; ++i) {
        snprintf(context, sizeof(context), "dim step %d", LANLAN_DIM_STEPS_SECONDS[i]);
        audit_both(lanlan_view_dim_text(LANLAN_DIM_STEPS_SECONDS[i]), context);
        audit_both(lanlan_view_timeout_text(LANLAN_DIM_STEPS_SECONDS[i]), context);
    }
    for (int i = 0; i < LANLAN_SCREEN_OFF_STEP_COUNT; ++i) {
        snprintf(context, sizeof(context), "screen-off step %d",
                 LANLAN_SCREEN_OFF_STEPS_SECONDS[i]);
        audit_both(lanlan_view_screen_off_text(LANLAN_SCREEN_OFF_STEPS_SECONDS[i]), context);
        audit_both(lanlan_view_timeout_text(LANLAN_SCREEN_OFF_STEPS_SECONDS[i]), context);
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
        if (s_audit_labels) audit_text(font, font_name, text, "rendered label");
        lv_area_t area;
        lv_obj_get_coords(obj, &area);
        if (area.x1 < 0 || area.x2 >= UI_W || area.y1 < 0 || area.y2 >= UI_H) {
            ++s_layout_warnings;
            if (s_layout_printed++ < 20) {
                printf("LAYOUT label outside screen: '%s' (%d,%d)-(%d,%d)\n", text, (int)area.x1,
                       (int)area.y1, (int)area.x2, (int)area.y2);
            }
        }
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if (parent) {
            lv_area_t bounds;
            lv_obj_get_coords(parent, &bounds);
            if (area.x1 < bounds.x1 || area.x2 > bounds.x2 || area.y1 < bounds.y1
                || area.y2 > bounds.y2) {
                ++s_layout_warnings;
                if (s_layout_printed++ < 20) {
                    printf("LAYOUT label outside parent: '%s' (%d,%d)-(%d,%d) not inside "
                           "(%d,%d)-(%d,%d)\n",
                           text, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2,
                           (int)bounds.x1, (int)bounds.y1, (int)bounds.x2, (int)bounds.y2);
                }
            }
        }
    }
    uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; ++i) inspect(lv_obj_get_child(obj, i));
}

/* --------------------------------------------------------------- capturing -- */

/* One full UI rebuild + software render without writing a file. The stress run
 * uses this so "rendering on" means exactly what the capture path does. */
static void render_only(const lanlan_ui_state_t *state) {
    lanlan_ui_render(s_screen, state);
    lv_obj_update_layout(s_screen);
    lv_refr_now(s_display);
    ++s_renders;
}

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

/* ================================================================== stress ==
 *
 * Host stability evidence for acceptance item A12. Everything below drives the
 * real UI/model code; nothing reimplements it. All figures are host numbers.
 *
 * Workload
 *   - page switching: 80 cycles over the eight-step sequence
 *       HOME, RECORDS, DETAIL(compact), DETAIL(full), COMPANION, SETTINGS,
 *       REMINDERS, STATUS
 *     which is 560 page switches (>= 500), enters and leaves both the record
 *     detail and the reminder list, and switches compact/full twice per cycle.
 *   - key events: 80 cycles of 20 key slots through lanlan_model_handle_key(),
 *     including up/down clicks, OK click, OK double click, OK long press and
 *     screen-off wake sequences (power off, the waking gesture, then the same
 *     gesture again), which is 2160 delivered events (>= 1000).
 *   - the wake contract is asserted, not assumed: with the display off EVERY
 *     key must return WAKE_ONLY and change nothing at all (the model is compared
 *     byte for byte against a pre-wake copy), the next gesture must behave
 *     exactly like the same gesture on a model that was never turned off, and a
 *     gesture delivered while the display is on (including dimmed, which keeps
 *     the model active) must never be consumed.
 *   - animation: while the companion page is shown the harness calls
 *     lanlan_ui_companion_react()/bark() and runs lv_timer_handler() so the
 *     sprite timer path is exercised.
 *   - every render is a full lanlan_ui_render() + layout + lv_refr_now(), with
 *     the model's list windows copied into the view the way the application
 *     does before each render.
 *
 * Stability assertions (exit non-zero on failure, with the exact numbers):
 *   - LVGL free pool after the run must not be below the pre-run baseline,
 *   - the largest free block must not be below the pre-run baseline,
 *   - the live object count must not exceed the pre-run baseline.
 * The tolerance is deliberately zero bytes / zero objects: the before and after
 * samples are taken after rebuilding the identical baseline screen, so the
 * allocator is deterministic. A relaxation here would hide exactly the leak
 * this run exists to find; investigating a difference is the intended path. */

#define STRESS_TOLERANCE_FREE_BYTES 0
#define STRESS_TOLERANCE_LARGEST_BYTES 0
#define STRESS_TOLERANCE_OBJECTS 0

#define STRESS_CYCLES 80
#define STRESS_KEYS_PER_CYCLE 20
#define STRESS_PAGE_STEPS 8

typedef struct {
    size_t lvgl_total;
    size_t lvgl_free;
    size_t lvgl_used;
    size_t lvgl_max_used;
    size_t lvgl_largest_free;
    unsigned lvgl_free_blocks;
    unsigned lvgl_used_blocks;
    unsigned lvgl_frag_pct;
    unsigned lvgl_used_pct;
    unsigned objects;
    size_t heap_in_use;
    size_t heap_allocated;
    unsigned heap_blocks;
    long peak_rss_bytes;
} usage_sample_t;

typedef struct {
    usage_sample_t before;
    usage_sample_t after;
    usage_sample_t worst; /* min free / min largest / max used / max frag / max objects */
    unsigned switches;
    unsigned keys;
    unsigned renders;
    unsigned wake_sequences;
    unsigned double_clicks;
    unsigned page_visits[LANLAN_PAGE_STATUS + 1];
    unsigned page_objects_max[LANLAN_PAGE_STATUS + 1]; /* per screen, for leak attribution */
    unsigned detail_compact;
    unsigned detail_full;
    unsigned flushes;
    unsigned min_free_render;   /* where the free pool was lowest */
    lanlan_page_t min_free_page;
    unsigned max_objects_render; /* where the live object count peaked */
    lanlan_page_t max_objects_page;
    lanlan_page_t last_page;
    bool invariants_ok;
} stress_report_t;

/* Every object reachable from the application screen, the active screen and the
 * three display layers. The UI creates all of its widgets under the application
 * screen, so a widget leak anywhere in the UI shows up as growth. */
static unsigned count_subtree(lv_obj_t *obj) {
    unsigned total = 1;
    uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; ++i) total += count_subtree(lv_obj_get_child(obj, i));
    return total;
}

static unsigned count_objects(void) {
    lv_obj_t *roots[5];
    unsigned root_count = 0;
    roots[root_count++] = s_screen;
    roots[root_count++] = lv_display_get_screen_active(s_display);
    roots[root_count++] = lv_display_get_layer_top(s_display);
    roots[root_count++] = lv_display_get_layer_sys(s_display);
    roots[root_count++] = lv_display_get_layer_bottom(s_display);
    unsigned total = 0;
    for (unsigned i = 0; i < root_count; ++i) {
        if (!roots[i]) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < i; ++j) {
            if (roots[j] == roots[i]) duplicate = true;
        }
        if (duplicate) continue;
        total += count_subtree(roots[i]);
    }
    return total;
}

static void sample_usage(usage_sample_t *out, bool with_process_heap) {
    memset(out, 0, sizeof(*out));
    lv_mem_monitor_t monitor;
    memset(&monitor, 0, sizeof(monitor));
    lv_mem_monitor(&monitor);
    out->lvgl_total = monitor.total_size;
    out->lvgl_free = monitor.free_size;
    out->lvgl_used = monitor.total_size - monitor.free_size;
    out->lvgl_max_used = monitor.max_used;
    out->lvgl_largest_free = monitor.free_biggest_size;
    out->lvgl_free_blocks = (unsigned)monitor.free_cnt;
    out->lvgl_used_blocks = (unsigned)monitor.used_cnt;
    out->lvgl_frag_pct = monitor.frag_pct;
    out->lvgl_used_pct = monitor.used_pct;
    out->objects = count_objects();

    if (with_process_heap) {
#if defined(__APPLE__)
        malloc_statistics_t stats;
        memset(&stats, 0, sizeof(stats));
        malloc_zone_statistics(malloc_default_zone(), &stats);
        out->heap_in_use = stats.size_in_use;
        out->heap_allocated = stats.size_allocated;
        out->heap_blocks = stats.blocks_in_use;
#elif defined(__GLIBC__)
#if defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
        struct mallinfo2 info = mallinfo2();
        out->heap_in_use = info.uordblks;
        out->heap_allocated = info.arena;
        out->heap_blocks = (unsigned)info.hblks;
#else
        struct mallinfo info = mallinfo();
        out->heap_in_use = (size_t)info.uordblks;
        out->heap_allocated = (size_t)info.arena;
        out->heap_blocks = (unsigned)info.hblks;
#endif
#endif
#endif
        struct rusage usage;
        memset(&usage, 0, sizeof(usage));
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
            out->peak_rss_bytes = usage.ru_maxrss; /* macOS reports bytes */
#else
            out->peak_rss_bytes = usage.ru_maxrss * 1024; /* Linux reports KiB */
#endif
        }
    }
}

static void stress_track(stress_report_t *report, const usage_sample_t *sample) {
    /* The worst sample keeps the minimum of every "must not shrink" figure and
     * the maximum of every "must not grow" figure, together with the render
     * index and screen it happened on so a failure can name the iteration. */
    if (sample->lvgl_free < report->worst.lvgl_free) {
        report->worst.lvgl_free = sample->lvgl_free;
        report->min_free_render = report->renders;
        report->min_free_page = report->last_page;
    }
    if (sample->lvgl_largest_free < report->worst.lvgl_largest_free) {
        report->worst.lvgl_largest_free = sample->lvgl_largest_free;
    }
    if (sample->lvgl_used > report->worst.lvgl_used) report->worst.lvgl_used = sample->lvgl_used;
    if (sample->lvgl_max_used > report->worst.lvgl_max_used) {
        report->worst.lvgl_max_used = sample->lvgl_max_used;
    }
    if (sample->lvgl_frag_pct > report->worst.lvgl_frag_pct) {
        report->worst.lvgl_frag_pct = sample->lvgl_frag_pct;
    }
    if (sample->objects > report->worst.objects) {
        report->worst.objects = sample->objects;
        report->max_objects_render = report->renders;
        report->max_objects_page = report->last_page;
    }
    if (sample->heap_in_use > report->worst.heap_in_use) {
        report->worst.heap_in_use = sample->heap_in_use;
    }
    if (sample->peak_rss_bytes > report->worst.peak_rss_bytes) {
        report->worst.peak_rss_bytes = sample->peak_rss_bytes;
    }
}

/* Samples the pool and the object count after one stress render. The full
 * process-heap sample (a syscall plus the allocator's statistics) runs every
 * eighth render so the per-iteration cost stays sane while still catching a
 * leak early enough to name the iteration. */
static usage_sample_t stress_sample(stress_report_t *report) {
    usage_sample_t sample;
    sample_usage(&sample, report->renders % 8 == 0);
    stress_track(report, &sample);
    if (sample.lvgl_free < 2048) {
        fprintf(stderr, "STRESS FAILURE: LVGL pool nearly exhausted on render %u (%u bytes free)\n",
                report->renders, (unsigned)sample.lvgl_free);
        exit(1);
    }
    return sample;
}

static void stress_after_render(lanlan_model_t *model, stress_report_t *report) {
    ++report->renders;
    if (model->page >= 0 && model->page <= LANLAN_PAGE_STATUS) ++report->page_visits[model->page];
    if (model->page != report->last_page) {
        ++report->switches;
        report->last_page = model->page;
    }
    if (report->renders % 16 == 0) {
        lv_timer_handler(); /* let the companion sprite timer fire */
    }
    if (report->renders % 64 == 0) {
        /* Keep some geometry coverage in the stress path without paying the
         * glyph audit (already done by the captures) on every iteration. */
        bool keep = s_audit_labels;
        s_audit_labels = false;
        inspect(s_screen);
        s_audit_labels = keep;
    }
    usage_sample_t sample = stress_sample(report);
    if (model->page >= 0 && model->page <= LANLAN_PAGE_STATUS
        && sample.objects > report->page_objects_max[model->page]) {
        report->page_objects_max[model->page] = sample.objects;
    }
}

/* The application copies the model's list windows into the view before every
 * render (main/main.c:509-510), because lanlan_view_reminder_rows() fills the
 * window that starts at view->reminder_offset while the renderer draws the rows
 * at model->reminder_offset. Mirror that so the drawn rows and their text always
 * describe the same window. */
static void stress_sync_view_offsets(const lanlan_model_t *model) {
    if (!s_view) return;
    s_view->list_offset = model->list_offset;
    s_view->reminder_offset = model->reminder_offset;
}

/* One stress render: keep the view windows aligned, draw, then sample. */
static void stress_render(lanlan_model_t *model, lanlan_ui_state_t *state,
                          stress_report_t *report) {
    stress_sync_view_offsets(model);
    render_only(state);
    stress_after_render(model, report);
}

static void stress_select_content(lanlan_model_t *model, lanlan_ui_state_t *state, unsigned cycle,
                                  unsigned step) {
    int records = (int)model->cache.record_count;
    int reminders = (int)model->cache.reminder_count;
    model->home_entry = (lanlan_home_entry_t)(cycle % LANLAN_HOME_ENTRY_COUNT);
    model->settings_row = (lanlan_settings_row_t)((cycle + step) % LANLAN_SETTINGS_ROW_COUNT);
    model->record_index = records > 0 ? (int)((cycle * 3u + step) % (unsigned)records) : 0;
    model->list_offset = lanlan_view_clamp_offset(records, model->record_index - 1,
                                                  LANLAN_MODEL_LIST_ROWS);
    model->reminder_index = reminders > 0 ? (int)((cycle + step) % (unsigned)reminders) : 0;
    model->reminder_offset = lanlan_view_clamp_offset(reminders, model->reminder_index - 1,
                                                      LANLAN_MODEL_LIST_ROWS);
    model->character = (lanlan_character_t)(cycle % LANLAN_CHARACTER_COUNT);

    /* Exercise the two writable timeout rows and the toggles through the real
     * setters, so the settings page renders every value it can show. */
    int dim = LANLAN_DIM_STEPS_SECONDS[cycle % LANLAN_DIM_STEP_COUNT];
    int off = LANLAN_SCREEN_OFF_STEPS_SECONDS[(cycle + step) % LANLAN_SCREEN_OFF_STEP_COUNT];
    if (!lanlan_model_set_timeouts(model, dim, off)) {
        (void)lanlan_model_set_timeouts(
            model, model->dim_seconds,
            LANLAN_SCREEN_OFF_STEPS_SECONDS[LANLAN_SCREEN_OFF_STEP_COUNT - 1]);
    }
    model->globally_muted = (cycle % 3u) == 0u;
    model->reminder_sound_enabled = (cycle % 5u) != 0u;

    static const int k_batteries[] = {78, -1, 30, 100, 0};
    static const char *const k_status[] = {LANLAN_STR_STATUS_OK, LANLAN_STR_STATUS_OFFLINE,
                                           LANLAN_STR_STATUS_CREDENTIAL_REJECTED};
    state->battery_percent = k_batteries[cycle % (sizeof(k_batteries) / sizeof(k_batteries[0]))];
    state->status_text = k_status[cycle % (sizeof(k_status) / sizeof(k_status[0]))];
    state->status_color = state->battery_percent < 0 ? 0xC4762Bu : 0x3AA76Du;
    state->secure_url = (cycle % 4u) != 0u;
    state->cache_rebuilt = (cycle % 7u) == 0u;
    state->storage_limited = (cycle % 11u) == 0u;
    state->status_detail = (cycle % 5u) == 0u ? LANLAN_STR_ERRORS_NETWORK : NULL;
    state->banner_text = (cycle % 13u) == 0u ? LANLAN_STR_REMINDERS_DUE : NULL;
}

/* Rebuilds the exact screen state the pre-run sample was taken on, so the
 * before/after object counts and pool figures are directly comparable. */
static void stress_baseline(lanlan_model_t *model, lanlan_ui_state_t *state,
                            const lanlan_caregiver_table_t *caregivers) {
    model->page = LANLAN_PAGE_HOME;
    model->home_entry = LANLAN_HOME_ENTRY_TODAY;
    model->record_index = 0;
    model->list_offset = 0;
    model->detail_full = 0;
    model->settings_row = LANLAN_SETTINGS_ROW_REFRESH;
    model->reminder_index = 0;
    model->reminder_offset = 0;
    model->character = LANLAN_CHARACTER_IDLE;
    model->companion_mood = 0;
    model->active = true;
    model->globally_muted = false;
    model->reminder_sound_enabled = true;
    model->utc_offset_min = UI_TZ_OFFSET_MIN;
    (void)lanlan_model_set_timeouts(model, LANLAN_TIMEOUT_DIM_DEFAULT_SECONDS,
                                    LANLAN_TIMEOUT_SCREEN_OFF_DEFAULT_SECONDS);
    state->battery_percent = 78;
    state->status_text = LANLAN_STR_STATUS_OK;
    state->status_color = 0x3AA76Du;
    state->status_detail = NULL;
    state->cache_rebuilt = false;
    state->storage_limited = false;
    state->secure_url = true;
    state->banner_text = NULL;
    state->caregivers = caregivers;
    stress_sync_view_offsets(model);
    render_only(state);
}

/* True when the two model states are byte-identical. Both are copies of the
 * same original, so the padding matches and memcmp is exact; this is the same
 * comparison tests/test_lanlan_model.c (test_wake_only_first_gesture) uses. */
static bool stress_model_equal(const lanlan_model_t *want, const lanlan_model_t *got) {
    return memcmp(want, got, sizeof(*want)) == 0;
}

/* Names every differing field, so an invariant failure points at the field that
 * moved instead of only saying "different". */
static void stress_model_diff(const lanlan_model_t *want, const lanlan_model_t *got) {
    printf("STRESS INVARIANT: model state differs from the reference:");
#define STRESS_DIFF(field) \
    if (want->field != got->field) printf(" " #field "=%d->%d", (int)want->field, (int)got->field)
    STRESS_DIFF(page);
    STRESS_DIFF(home_entry);
    STRESS_DIFF(record_index);
    STRESS_DIFF(list_offset);
    STRESS_DIFF(detail_full);
    STRESS_DIFF(character);
    STRESS_DIFF(companion_mood);
    STRESS_DIFF(settings_row);
    STRESS_DIFF(reminder_index);
    STRESS_DIFF(reminder_offset);
    STRESS_DIFF(globally_muted);
    STRESS_DIFF(reminder_sound_enabled);
    STRESS_DIFF(dim_seconds);
    STRESS_DIFF(screen_off_seconds);
    STRESS_DIFF(utc_offset_min);
    STRESS_DIFF(active);
    STRESS_DIFF(clock_trusted);
#undef STRESS_DIFF
    if (memcmp(&want->now, &got->now, sizeof(want->now)) != 0) printf(" now");
    if (memcmp(&want->cache, &got->cache, sizeof(want->cache)) != 0) printf(" cache");
    printf("\n");
}

/* One key event delivered with the display ON. "On" includes the dimmed state:
 * dimming only lowers the backlight, the model stays active, and the wake rule
 * must therefore never consume the gesture. `woke_screen` is informational, so
 * the harness alternates it to prove the model cannot be told to swallow a
 * gesture by a caller's flag. Returns the model's action. */
static lanlan_action_t stress_deliver_active_key(lanlan_model_t *model,
                                                 lanlan_ui_state_t *state, lanlan_key_t key,
                                                 stress_report_t *report) {
    if (!model->active) {
        printf("STRESS INVARIANT: the harness delivered key %d as an active-display key while "
               "the model was off\n",
               (int)key);
        report->invariants_ok = false;
    }
    lanlan_key_event_t event;
    event.key = key;
    event.woke_screen = (report->keys & 1u) != 0u; /* informational only */
    lanlan_action_t action = lanlan_model_handle_key(model, &event);
    ++report->keys;
    if (action == LANLAN_ACTION_WAKE_ONLY) {
        printf("STRESS INVARIANT: key %d on page %d was consumed as a wake gesture while the "
               "display was on (a dimmed display is still active)\n",
               (int)key, (int)model->page);
        report->invariants_ok = false;
    }
    if (lanlan_action_is_record_write(action)) {
        printf("STRESS INVARIANT: key %d on page %d produced a forbidden record write (%d)\n",
               (int)key, (int)model->page, (int)action);
        report->invariants_ok = false;
    }
    if (lanlan_model_pending_record_action(model)) {
        printf("STRESS INVARIANT: a pending record action survived key %d on page %d\n", (int)key,
               (int)model->page);
        report->invariants_ok = false;
    }
    if (action == LANLAN_ACTION_COMPANION_PET) {
        lanlan_ui_companion_react(LANLAN_CHARACTER_HAPPY);
    }
    stress_render(model, state, report);
    return action;
}

/* Screen-off wake sequence: the display goes off, the waking gesture is
 * delivered, then the same gesture is delivered again with the display on.
 *
 * Contract (main/lanlan_model.c, tests/test_lanlan_model.c:316+): while the
 * display is off EVERY key and event is consumed and only turns the display on,
 * so page, selection, settings and companion state are all unchanged. The rule
 * belongs to the model, so `woke_screen` cannot influence it. */
static void stress_wake_sequence(lanlan_model_t *model, lanlan_ui_state_t *state, lanlan_key_t key,
                                 stress_report_t *report) {
    /* The state the model must come back to: it is active here, and the waking
     * gesture may change nothing except that flag. */
    lanlan_model_t reference = *model;
    lanlan_model_set_active(model, false);

    lanlan_key_event_t event;
    event.key = key;
    event.woke_screen = true; /* informational; the model owns the rule */
    lanlan_action_t wake_action = lanlan_model_handle_key(model, &event);
    ++report->keys;
    ++report->wake_sequences;
    if (wake_action != LANLAN_ACTION_WAKE_ONLY) {
        printf("STRESS INVARIANT: the waking gesture (key %d on page %d) returned action %d, "
               "expected WAKE_ONLY\n",
               (int)key, (int)reference.page, (int)wake_action);
        report->invariants_ok = false;
    }
    if (!model->active) {
        printf("STRESS INVARIANT: the waking gesture (key %d) left the display off\n", (int)key);
        report->invariants_ok = false;
    }
    if (!stress_model_equal(&reference, model)) {
        printf("STRESS INVARIANT: the waking gesture (key %d) changed model state\n", (int)key);
        stress_model_diff(&reference, model);
        report->invariants_ok = false;
    }
    stress_render(model, state, report);

    /* "The next gesture behaves normally" is checked against an oracle: the
     * same key sent to a copy of the model that was never turned off must give
     * the same action and the same resulting state. */
    lanlan_key_event_t follow;
    follow.key = key;
    follow.woke_screen = false;
    lanlan_action_t expected = lanlan_model_handle_key(&reference, &follow);
    lanlan_action_t actual = stress_deliver_active_key(model, state, key, report);
    if (actual != expected) {
        printf("STRESS INVARIANT: the gesture after the wake (key %d) returned action %d, the "
               "never-off reference returned %d\n",
               (int)key, (int)actual, (int)expected);
        report->invariants_ok = false;
    }
    if (!stress_model_equal(&reference, model)) {
        printf("STRESS INVARIANT: the gesture after the wake (key %d) left a different state than "
               "the never-off reference\n",
               (int)key);
        stress_model_diff(&reference, model);
        report->invariants_ok = false;
    }
}

/* 20 key slots per cycle: up/down, OK click, OK double click, OK long press and
 * screen-off wake sequences (waking gesture then the same gesture again). */
static void stress_key_cycle(lanlan_model_t *model, lanlan_ui_state_t *state, unsigned cycle,
                             stress_report_t *report) {
    static const lanlan_key_t k_keys[STRESS_KEYS_PER_CYCLE] = {
        LANLAN_KEY_UP,       LANLAN_KEY_DOWN,     LANLAN_KEY_OK_CLICK, LANLAN_KEY_OK_CLICK,
        LANLAN_KEY_OK_LONG,  LANLAN_KEY_DOWN,     LANLAN_KEY_UP,       LANLAN_KEY_OK_CLICK,
        LANLAN_KEY_OK_LONG,  LANLAN_KEY_DOWN,     LANLAN_KEY_DOWN,     LANLAN_KEY_OK_CLICK,
        LANLAN_KEY_UP,       LANLAN_KEY_OK_LONG,  LANLAN_KEY_OK_CLICK, LANLAN_KEY_DOWN,
        LANLAN_KEY_UP,       LANLAN_KEY_OK_CLICK, LANLAN_KEY_OK_LONG,  LANLAN_KEY_DOWN,
    };
    for (unsigned index = 0; index < STRESS_KEYS_PER_CYCLE; ++index) {
        lanlan_key_t key = k_keys[(index + cycle) % STRESS_KEYS_PER_CYCLE];
        if ((index % 5u) == 4u) {
            stress_wake_sequence(model, state, key, report);
        } else if ((index % 7u) == 3u) {
            stress_deliver_active_key(model, state, LANLAN_KEY_OK_CLICK, report);
            stress_deliver_active_key(model, state, LANLAN_KEY_OK_CLICK, report);
            ++report->double_clicks;
        } else {
            stress_deliver_active_key(model, state, key, report);
        }
    }
}

static int stress_run(lanlan_model_t *model, lanlan_ui_state_t *state,
                      const lanlan_caregiver_table_t *caregivers) {
    static const lanlan_page_t k_pages[STRESS_PAGE_STEPS] = {
        LANLAN_PAGE_HOME,     LANLAN_PAGE_RECORDS,  LANLAN_PAGE_DETAIL, LANLAN_PAGE_DETAIL,
        LANLAN_PAGE_COMPANION, LANLAN_PAGE_SETTINGS, LANLAN_PAGE_REMINDERS, LANLAN_PAGE_STATUS,
    };
    stress_report_t report;
    memset(&report, 0, sizeof(report));
    report.invariants_ok = true;
    report.last_page = LANLAN_PAGE_HOME;

    /* Baseline: identical render before and after, so any difference is a leak. */
    stress_baseline(model, state, caregivers);
    sample_usage(&report.before, true);
    report.worst = report.before;

    printf("stress: starting %u page-switch cycles (%u steps) and %u key cycles (%u slots)\n",
           (unsigned)STRESS_CYCLES, (unsigned)STRESS_PAGE_STEPS, (unsigned)STRESS_CYCLES,
           (unsigned)STRESS_KEYS_PER_CYCLE);

    bool keep_audit = s_audit_labels;
    s_audit_labels = false; /* the captures already audited every glyph */

    for (unsigned cycle = 0; cycle < STRESS_CYCLES; ++cycle) {
        for (unsigned step = 0; step < STRESS_PAGE_STEPS; ++step) {
            lanlan_page_t page = k_pages[step];
            if (step == 2u) {
                model->detail_full = 0; /* enter the compact detail */
                ++report.detail_compact;
            }
            if (step == 3u) {
                model->detail_full = 1; /* switch to the full detail */
                ++report.detail_full;
            }
            stress_select_content(model, state, cycle, step);
            model->page = page;
            stress_render(model, state, &report);
            if (page == LANLAN_PAGE_COMPANION) {
                /* Keep the animation path live while the companion is shown. */
                lanlan_ui_companion_react((cycle % 2u) ? LANLAN_CHARACTER_HAPPY
                                                       : LANLAN_CHARACTER_BLINK);
                if ((cycle % 3u) == 0u) lanlan_ui_companion_bark();
                lv_timer_handler();
                stress_render(model, state, &report);
            }
        }
    }

    for (unsigned cycle = 0; cycle < STRESS_CYCLES; ++cycle) {
        stress_key_cycle(model, state, cycle, &report);
    }

    s_audit_labels = keep_audit;
    report.flushes = s_flushes;

    /* Return to the exact pre-run screen and sample again. */
    stress_baseline(model, state, caregivers);
    sample_usage(&report.after, true);

    printf("STRESS SUMMARY mode=host-lvgl switches=%u keys=%u renders=%u flushes=%u "
           "wake_sequences=%u double_clicks=%u detail_compact=%u detail_full=%u\n",
           report.switches, report.keys, report.renders, report.flushes, report.wake_sequences,
           report.double_clicks, report.detail_compact, report.detail_full);
    printf("STRESS PAGES home=%u records=%u detail=%u companion=%u settings=%u reminders=%u "
           "status=%u\n",
           report.page_visits[LANLAN_PAGE_HOME], report.page_visits[LANLAN_PAGE_RECORDS],
           report.page_visits[LANLAN_PAGE_DETAIL], report.page_visits[LANLAN_PAGE_COMPANION],
           report.page_visits[LANLAN_PAGE_SETTINGS], report.page_visits[LANLAN_PAGE_REMINDERS],
           report.page_visits[LANLAN_PAGE_STATUS]);
    printf("STRESS PAGE OBJECTS objects_home=%u objects_records=%u objects_detail=%u "
           "objects_companion=%u objects_settings=%u objects_reminders=%u objects_status=%u\n",
           report.page_objects_max[LANLAN_PAGE_HOME], report.page_objects_max[LANLAN_PAGE_RECORDS],
           report.page_objects_max[LANLAN_PAGE_DETAIL],
           report.page_objects_max[LANLAN_PAGE_COMPANION],
           report.page_objects_max[LANLAN_PAGE_SETTINGS],
           report.page_objects_max[LANLAN_PAGE_REMINDERS],
           report.page_objects_max[LANLAN_PAGE_STATUS]);
    printf("STRESS LVGL total=%u free_before=%u free_after=%u free_min=%u used_before=%u "
           "used_after=%u used_max=%u used_pct_after=%u max_used_before=%u max_used_after=%u "
           "frag_before=%u frag_after=%u frag_max=%u "
           "largest_before=%u largest_after=%u largest_min=%u free_blocks_after=%u "
           "used_blocks_after=%u\n",
           (unsigned)report.before.lvgl_total, (unsigned)report.before.lvgl_free,
           (unsigned)report.after.lvgl_free, (unsigned)report.worst.lvgl_free,
           (unsigned)report.before.lvgl_used, (unsigned)report.after.lvgl_used,
           (unsigned)report.worst.lvgl_used, (unsigned)report.after.lvgl_used_pct,
           (unsigned)report.before.lvgl_max_used, (unsigned)report.after.lvgl_max_used,
           report.before.lvgl_frag_pct, report.after.lvgl_frag_pct, report.worst.lvgl_frag_pct,
           (unsigned)report.before.lvgl_largest_free, (unsigned)report.after.lvgl_largest_free,
           (unsigned)report.worst.lvgl_largest_free, report.after.lvgl_free_blocks,
           report.after.lvgl_used_blocks);
    printf("STRESS OBJECTS objects_before=%u objects_after=%u objects_max=%u "
           "objects_max_render=%u objects_max_page=%d free_min_render=%u free_min_page=%d\n",
           report.before.objects, report.after.objects, report.worst.objects,
           report.max_objects_render, (int)report.max_objects_page, report.min_free_render,
           (int)report.min_free_page);
    printf("STRESS PROCESS heap_in_use_before=%u heap_in_use_after=%u heap_in_use_max=%u "
           "heap_allocated_before=%u heap_allocated_after=%u blocks_in_use_after=%u "
           "peak_rss_before=%ld peak_rss_after=%ld\n",
           (unsigned)report.before.heap_in_use, (unsigned)report.after.heap_in_use,
           (unsigned)report.worst.heap_in_use, (unsigned)report.before.heap_allocated,
           (unsigned)report.after.heap_allocated, report.after.heap_blocks,
           report.before.peak_rss_bytes, report.after.peak_rss_bytes);

    int failures = 0;
    if (report.switches < 500u) {
        printf("STRESS FAILURE: only %u page switches, need at least 500\n", report.switches);
        ++failures;
    }
    if (report.keys < 1000u) {
        printf("STRESS FAILURE: only %u key events, need at least 1000\n", report.keys);
        ++failures;
    }
    if (!report.invariants_ok) {
        printf("STRESS FAILURE: a model invariant check failed during the run\n");
        ++failures;
    }
    if (report.after.lvgl_free + STRESS_TOLERANCE_FREE_BYTES < report.before.lvgl_free) {
        printf("STRESS FAILURE: LVGL free pool shrank: before=%u after=%u tolerance=%u bytes; "
               "lowest free figure was %u at render %u on page %d\n",
               (unsigned)report.before.lvgl_free, (unsigned)report.after.lvgl_free,
               (unsigned)STRESS_TOLERANCE_FREE_BYTES, (unsigned)report.worst.lvgl_free,
               report.min_free_render, (int)report.min_free_page);
        ++failures;
    }
    if (report.after.lvgl_largest_free + STRESS_TOLERANCE_LARGEST_BYTES
        < report.before.lvgl_largest_free) {
        printf("STRESS FAILURE: largest free block shrank: before=%u after=%u tolerance=%u bytes\n",
               (unsigned)report.before.lvgl_largest_free, (unsigned)report.after.lvgl_largest_free,
               (unsigned)STRESS_TOLERANCE_LARGEST_BYTES);
        ++failures;
    }
    if (report.after.objects > report.before.objects + STRESS_TOLERANCE_OBJECTS) {
        printf("STRESS FAILURE: live LVGL object count grew: before=%u after=%u tolerance=%u; "
               "peak was %u at render %u on page %d\n",
               report.before.objects, report.after.objects, (unsigned)STRESS_TOLERANCE_OBJECTS,
               report.worst.objects, report.max_objects_render, (int)report.max_objects_page);
        ++failures;
    }
    if (failures == 0) {
        printf("STRESS RESULT PASS\n");
        return 0;
    }
    printf("STRESS RESULT FAIL (%d check%s)\n", failures, failures == 1 ? "" : "s");
    return 1;
}

static void usage(void) {
    fprintf(stderr,
            "usage: lanlan_ui_preview <output-dir> [--stress]\n"
            "\n"
            "  <output-dir>  directory for the 16 screen PPM captures\n"
            "  --stress      after the captures, run the A12 stability workload:\n"
            "                >=500 page switches across the seven screens with the\n"
            "                record-detail compact/full toggle, >=1000 synthetic key\n"
            "                events through lanlan_model_handle_key (up/down, OK\n"
            "                click, OK double click, OK long press and screen-off\n"
            "                wake sequences) with LVGL rendering on, then print the\n"
            "                LVGL pool, process heap and live-object numbers and\n"
            "                fail when the pre-run baseline does not return.\n"
            "\n"
            "Examples:\n"
            "  ./lanlan_ui_preview build/lanlan-preview/screens\n"
            "  ./lanlan_ui_preview build/lanlan-preview/screens --stress\n"
            "  python3 tools/preview_lanlan.py                      # 16 captures\n"
            "  python3 tools/preview_lanlan.py --mode stress        # captures + stress\n");
}

/* -------------------------------------------------------------------- main -- */

int main(int argc, char **argv) {
    const char *output_dir = NULL;
    bool stress = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--stress") == 0) {
            stress = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            return 2;
        } else if (output_dir == NULL) {
            output_dir = argv[i];
        } else {
            fprintf(stderr, "unexpected argument '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (output_dir == NULL) {
        usage();
        return 2;
    }
    s_output_dir = output_dir;
    /* Keep stdout line-buffered so the driver sees the LAYOUT / STRESS lines in
     * the order they were produced even when it pipes them. */
    setvbuf(stdout, NULL, _IOLBF, 0);
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
    lv_tick_set_cb(host_tick_cb); /* the host equivalent of the periodic lv_tick_inc */
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
    model.companion_mood = 0;
    model.globally_muted = false;
    model.reminder_sound_enabled = true;
    model.utc_offset_min = UI_TZ_OFFSET_MIN;
    /* The application loads both backlight timeouts from NVS before the first
     * render; lanlan_model_init() leaves them at 0, which is not a selectable
     * step, so install the documented defaults. */
    assert(lanlan_model_set_timeouts(&model, LANLAN_TIMEOUT_DIM_DEFAULT_SECONDS,
                                     LANLAN_TIMEOUT_SCREEN_OFF_DEFAULT_SECONDS));

    s_view = &view;
    audit_view_strings(&view, &now, &caregivers, &fallback_caregivers, &model);

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

    int exit_code = s_missing == 0 ? 0 : 1;
    if (stress) {
        if (stress_run(&model, &state, &caregivers) != 0) exit_code = 1;
    }

    lanlan_ui_deinit();
    lv_deinit();
    return exit_code;
}

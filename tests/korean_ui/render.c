#include "korean_ui.h"
#include "bsp_display_rounding.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Host-only display geometry is checked against the firmware's bsp_pins.h by the runner.
#define W 240
#define H 320
#define R 30
static uint16_t s_frame[W * H];
static uint16_t s_buffer[W * 20];
static lv_display_t *s_display;
static lv_obj_t *s_screen;
static unsigned s_cases;
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const uint16_t *data = (const uint16_t *)pixels;
    unsigned stride = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < W && y >= 0 && y < H);
            s_frame[y * W + x] = bsp_display_pixel_outside_rounded_rect(x, y, W, H, R)
                ? 0 : data[(y - area->y1) * stride + x - area->x1];
        }
    lv_display_flush_ready(display);
}
static uint32_t next_codepoint(const unsigned char **p) {
    uint32_t cp = *(*p)++;
    if (cp < 0x80) return cp;
    unsigned count = cp < 0xE0 ? 1 : cp < 0xF0 ? 2 : 3;
    cp &= count == 1 ? 0x1F : count == 2 ? 0x0F : 0x07;
    while (count--) { assert((**p & 0xC0) == 0x80); cp = (cp << 6) | (*(*p)++ & 0x3F); }
    return cp;
}
static void check_glyphs(const lv_font_t *font, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = next_codepoint(&p);
        if (cp < 32) continue;
        lv_font_glyph_dsc_t d = {0};
        if (!lv_font_get_glyph_dsc(font, &d, cp, 0) || d.is_placeholder) {
            fprintf(stderr, "Missing glyph U+%04X in %s\n", (unsigned)cp, text);
            abort();
        }
    }
}
static void inspect(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *str = lv_label_get_text(obj);
        check_glyphs(lv_obj_get_style_text_font(obj, LV_PART_MAIN), str);
        lv_area_t area; lv_obj_get_coords(obj, &area);
        if (area.x1 < 0 || area.x2 >= W || area.y1 < 0 || area.y2 >= H) {
            fprintf(stderr, "Label outside screen: %s (%d,%d)-(%d,%d)\n", str,
                    (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2);
            abort();
        }
        lv_obj_t *parent = lv_obj_get_parent(obj);
        lv_area_t p; lv_obj_get_coords(parent, &p);
        if (area.x1 < p.x1 || area.x2 > p.x2 || area.y1 < p.y1 || area.y2 > p.y2) {
            fprintf(stderr, "Label outside parent: %s\n", str); abort();
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) inspect(lv_obj_get_child(obj, i));
}
static void capture(const char *path, const ko_model_t *model, bool healthy) {
    ko_ui_render(s_screen, model, healthy ? 82 : -1, healthy, healthy, healthy);
    lv_obj_update_layout(s_screen);
    inspect(s_screen);
    lv_refr_now(s_display);
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    assert(memory.free_size > 3000);
    if (path) {
        FILE *f = fopen(path, "wb"); assert(f);
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (unsigned i = 0; i < W * H; ++i) {
            uint16_t v = s_frame[i];
            unsigned char rgb[] = {(unsigned char)(((v >> 11) & 31) * 255 / 31),
                (unsigned char)(((v >> 5) & 63) * 255 / 63), (unsigned char)((v & 31) * 255 / 31)};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    ++s_cases;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    lv_init();
    s_display = lv_display_create(W, H); assert(s_display);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_buffer, NULL, sizeof(s_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush);
    s_screen = lv_obj_create(NULL);
    lv_screen_load(s_screen);
    lv_font_glyph_dsc_t negative = {0};
    assert(!lv_font_get_glyph_dsc(&korean_font_16, &negative, 0x1F600, 0) || negative.is_placeholder);
    ko_model_t m; ko_init(&m, 123);
    char file[1024];
    snprintf(file, sizeof(file), "%s/home.ppm", argv[1]); capture(file, &m, true);
    for (unsigned i = 0; i < KO_COUNT; ++i) {
        m.page = KO_CARDS; m.menu = i < KO_LETTERS ? 0 : 1; m.card = i;
        for (unsigned flipped = 0; flipped < 2; ++flipped) {
            m.flipped = flipped;
            check_glyphs(&korean_font_28, ko_items[i].ko);
            check_glyphs(&korean_font_16, ko_items[i].zh);
            check_glyphs(&korean_font_16, ko_items[i].note);
            capture(NULL, &m, true);
        }
    }
    m.page = KO_CARDS; m.menu = 1; m.card = 24; m.flipped = true;
    snprintf(file, sizeof(file), "%s/card.ppm", argv[1]); capture(file, &m, true);
    m.card = 54;
    snprintf(file, sizeof(file), "%s/long-word.ppm", argv[1]); capture(file, &m, true);
    ko_handle(&m, KO_BACK); m.menu = 2; ko_handle(&m, KO_OK);
    m.questions[0] = 24; m.choices[0] = 25; m.choices[1] = 24; m.choices[2] = 26; m.selection = 1;
    snprintf(file, sizeof(file), "%s/question.ppm", argv[1]); capture(file, &m, true);
    ko_handle(&m, KO_OK);
    snprintf(file, sizeof(file), "%s/correct.ppm", argv[1]); capture(file, &m, true);
    m.last_correct = false; m.selection = 0;
    snprintf(file, sizeof(file), "%s/wrong.ppm", argv[1]); capture(file, &m, true);
    m.listening = true; m.feedback = false;
    snprintf(file, sizeof(file), "%s/listening.ppm", argv[1]); capture(file, &m, true);
    m.page = KO_RESULT; m.round_count = 5; m.round_correct = 4; m.progress.answered = 5; m.progress.correct = 4;
    snprintf(file, sizeof(file), "%s/result.ppm", argv[1]); capture(file, &m, true);
    m.page = KO_EMPTY;
    snprintf(file, sizeof(file), "%s/empty.ppm", argv[1]); capture(file, &m, true);
    m.page = KO_AUDIO_ERROR; capture(NULL, &m, true);
    // Recreate every screen repeatedly under the same 24 KB LVGL allocator.
    for (unsigned i = 0; i < 200; ++i) {
        m.page = i % 6; capture(NULL, &m, true); capture(NULL, &m, false);
    }
    printf("LVGL UI: PASS (%u renders, 60 cards both sides, glyph and geometry checks, 24 KB pool)\n", s_cases);
    lv_deinit();
    return 0;
}

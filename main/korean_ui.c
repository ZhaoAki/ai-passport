#include "korean_ui.h"
#include <stdio.h>
#define INK 0x25364A
#define MUTED 0x66768A
#define GREEN 0x167D66
#define BG 0xF4F7F4

static lv_obj_t *text(lv_obj_t *parent, int x, int y, int width,
                      const char *value, bool large, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, large ? &korean_font_28 : &korean_font_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, value);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}
static void center_text(lv_obj_t *parent, int y, const char *value, bool large, uint32_t color) {
    lv_obj_t *label = text(parent, 18, y, 204, value, large, color);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}
static void row(lv_obj_t *screen, int y, const char *value, bool selected, uint32_t color) {
    lv_obj_t *box = lv_obj_create(screen);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 18, y);
    lv_obj_set_size(box, 204, 30);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(selected ? color : 0xFFFFFF), 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    text(box, 10, 3, 184, value, false, selected ? 0xFFFFFF : INK);
}
void ko_ui_render(lv_obj_t *screen, const ko_model_t *m, int battery,
                  bool audio_ok, bool storage_ok, bool buttons_ok) {
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_text_font(screen, &korean_font_16, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    text(screen, 18, 13, 156, "韩语口袋课", false, GREEN);
    char buf[80];
    if (battery < 0) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d%%", battery);
    text(screen, 182, 13, 42, buf, false, MUTED);
    if (m->page == KO_HOME) {
        text(screen, 18, 43, 204, "一点一点，学会韩语", false, INK);
        snprintf(buf, sizeof(buf), "已看 %u/60  ·  错题 %u", ko_seen_count(&m->progress),
                 ko_mistake_count(&m->progress));
        text(screen, 18, 72, 204, buf, false, MUTED);
        const char *entries[] = {"字母入门", "单词卡片", "看词选义", "听音选词", "错题复习"};
        for (unsigned i = 0; i < 5; ++i) row(screen, 103 + 31 * i, entries[i], m->menu == i, GREEN);
        text(screen, 18, 285, 204, "上下选择  确认进入", false, MUTED);
    } else if (m->page == KO_CARDS) {
        const ko_item_t *item = &ko_items[m->card];
        unsigned index = m->card < KO_LETTERS ? m->card + 1 : m->card - KO_LETTERS + 1;
        snprintf(buf, sizeof(buf), "%s  %u/%u", m->menu == 0 ? "基础字母" : "日常单词",
                 index, m->menu == 0 ? KO_LETTERS : KO_COUNT - KO_LETTERS);
        text(screen, 18, 48, 204, buf, false, MUTED);
        center_text(screen, 91, item->ko, true, INK);
        center_text(screen, 137, item->roman, false, MUTED);
        center_text(screen, 178, m->flipped ? item->zh : "确认键查看含义", false, GREEN);
        if (m->flipped) center_text(screen, 214, item->note, false, MUTED);
        text(screen, 18, 274, 204, "上下换卡  确认翻面", false, MUTED);
        text(screen, 18, 292, 204, "长按：下键听音，OK返回", false, MUTED);
    } else if (m->page == KO_QUIZ) {
        const ko_item_t *item = &ko_items[m->questions[m->round_pos]];
        snprintf(buf, sizeof(buf), "%s  %u/%u", m->review ? "错题复习" : (m->listening ? "听音选词" : "看词选义"),
                 m->round_pos + 1, m->round_count);
        text(screen, 18, 48, 204, buf, false, MUTED);
        center_text(screen, 83, m->listening && !m->feedback ? "听一听" : item->ko, true, INK);
        center_text(screen, 124, m->feedback ? item->zh : (m->listening ? "长按下键重听" : "选择中文意思"), false, MUTED);
        for (unsigned i = 0; i < 3; ++i) {
            bool is_correct = m->choices[i] == m->questions[m->round_pos];
            bool highlight = m->feedback ? (is_correct || i == m->selection) : i == m->selection;
            uint32_t color = m->feedback && !is_correct ? 0xB94A48 : GREEN;
            row(screen, 156 + 33 * i, ko_items[m->choices[i]].zh, highlight, color);
        }
        text(screen, 18, 258, 204, "长按：OK返回，下键听音", false, MUTED);
        text(screen, 18, 285, 204, m->feedback ? (m->last_correct ? "答对了！确认继续" : "记住答案，确认继续")
                                                           : "上下选择  确认作答", false, GREEN);
    } else if (m->page == KO_RESULT) {
        center_text(screen, 61, "完成一轮", true, GREEN);
        snprintf(buf, sizeof(buf), "%u / %u", m->round_correct, m->round_count);
        center_text(screen, 119, buf, true, INK);
        center_text(screen, 166, "每轮最多五题", false, MUTED);
        center_text(screen, 194, "连对两次，移出错题", false, MUTED);
        snprintf(buf, sizeof(buf), "累计答对 %lu 题", (unsigned long)m->progress.correct);
        center_text(screen, 236, buf, false, MUTED);
        center_text(screen, 285, "确认回首页", false, GREEN);
    } else if (m->page == KO_AUDIO_ERROR) {
        center_text(screen, 82, "声音不可用", false, GREEN);
        center_text(screen, 134, "可先练习看词选义", false, INK);
        center_text(screen, 285, "确认回首页", false, MUTED);
    } else {
        center_text(screen, 82, "暂时没有错题", false, GREEN);
        center_text(screen, 134, "先做一轮练习吧", false, INK);
        center_text(screen, 285, "确认回首页", false, MUTED);
    }
    const char *status = !buttons_ok ? "按键不可用，请重启" : !storage_ok ? "记录未保存" : !audio_ok ? "声音不可用，可继续看词" : NULL;
    if (status) {
        lv_obj_t *strip = lv_obj_create(screen);
        lv_obj_remove_style_all(strip);
        lv_obj_set_pos(strip, 18, 271);
        lv_obj_set_size(strip, 204, 45);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(strip, lv_color_hex(BG), 0);
        lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
        text(strip, 0, 0, 204, status, false, 0xB94A48);
        text(strip, 0, 22, 204, "长按确认返回", false, MUTED);
    }
}

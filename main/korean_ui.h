#pragma once
#include "korean_model.h"
#include "lvgl.h"
LV_FONT_DECLARE(korean_font_16);
LV_FONT_DECLARE(korean_font_28);
void ko_ui_render(lv_obj_t *screen, const ko_model_t *m, int battery,
                  bool audio_ok, bool storage_ok, bool buttons_ok);

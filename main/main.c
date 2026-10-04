// Offline Korean application. One owner serializes model, NVS, audio and rendering.
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "korean_model.h"
#include "korean_ui.h"
#include "korean_audio_data.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

extern const uint8_t audio_start[] asm("_binary_korean_mms_16k_pcm_start");
extern const uint8_t audio_end[] asm("_binary_korean_mms_16k_pcm_end");
static const char *TAG = "korean";
static ko_model_t s_model;
static QueueHandle_t s_queue;
static lv_obj_t *s_screen;
static nvs_handle_t s_nvs;
static bool s_storage_open, s_storage_ok, s_audio_ok, s_buttons_ok;
static bool s_audio_awake;
static atomic_bool s_ready;
static uint32_t s_pcm_offset, s_pcm_remaining;
static int s_battery = -1;
static int64_t s_last_audio_write;
typedef struct { bsp_btn_t key; bsp_btn_ev_t event; } input_t;

static void on_key(bsp_btn_t key, bsp_btn_ev_t event, void *user) {
    (void)user;
    if (!atomic_load(&s_ready)) return;
    if (event != BSP_BTN_CLICK && event != BSP_BTN_LONG) return;
    const input_t input = {key, event};
    (void)xQueueSend(s_queue, &input, 0);
}
static void render(void) {
    if (!bsp_lvgl_lock(1000)) return;
    ko_ui_render(s_screen, &s_model, s_battery, s_audio_ok, s_storage_ok, s_buttons_ok);
    bsp_lvgl_unlock();
}
static void load_progress(void) {
    // Never erase unrelated NVS when initialization or decoding fails.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) err = nvs_open("korean_course", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Storage unavailable: %s", esp_err_to_name(err)); return; }
    s_storage_open = true;
    s_storage_ok = true;
    uint8_t data[KO_SAVE_BYTES];
    size_t size = sizeof(data);
    err = nvs_get_blob(s_nvs, "progress_v1", data, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK || !ko_decode(&s_model.progress, data, size)) {
        // Retain invalid/unknown records for inspection instead of overwriting them.
        s_storage_ok = false;
        s_storage_open = false;
        nvs_close(s_nvs);
        ESP_LOGE(TAG, "Progress cannot be read; session will use RAM only");
    }
}
static void save_progress(void) {
    if (!s_storage_open) return;
    uint8_t data[KO_SAVE_BYTES];
    ko_encode(&s_model.progress, data);
    esp_err_t err = nvs_set_blob(s_nvs, "progress_v1", data, sizeof(data));
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    s_storage_ok = err == ESP_OK;
    if (err != ESP_OK) ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
}
static void stop_audio(void) { s_pcm_remaining = 0; }
static void audio_failed(void) {
    stop_audio();
    s_audio_ok = false;
    if (s_model.page == KO_QUIZ && s_model.listening && !s_model.feedback)
        s_model.page = KO_AUDIO_ERROR;
}
static void play(unsigned id) {
    if (!s_audio_ok || id >= KO_COUNT) return;
    const ko_audio_clip_t *clip = &ko_audio_clips[id];
    size_t size = (size_t)(audio_end - audio_start);
    if (clip->offset > size || clip->bytes > size - clip->offset || clip->bytes % 2) {
        audio_failed();
        ESP_LOGE(TAG, "Invalid audio index %u", id);
        return;
    }
    esp_err_t err = s_audio_awake ? ESP_OK : bsp_audio_wake();
    if (err == ESP_OK) err = bsp_audio_set_format(16000, 16, 1);
    if (err != ESP_OK) { audio_failed(); ESP_LOGE(TAG, "Audio start: %s", esp_err_to_name(err)); return; }
    s_audio_awake = true;
    s_pcm_offset = clip->offset;
    s_pcm_remaining = clip->bytes;
}
static void feed_audio(void) {
    // Only 512 bytes of aligned PCM on the task stack; no whole-clip heap allocation.
    int16_t pcm[256];
    size_t bytes = s_pcm_remaining < sizeof(pcm) ? s_pcm_remaining : sizeof(pcm);
    memcpy(pcm, audio_start + s_pcm_offset, bytes);
    esp_err_t err = bsp_audio_write(pcm, bytes);
    if (err != ESP_OK) {
        audio_failed();
        ESP_LOGE(TAG, "Audio write: %s", esp_err_to_name(err));
        render();
        return;
    }
    s_last_audio_write = esp_timer_get_time();
    s_pcm_offset += bytes;
    s_pcm_remaining -= bytes;
    // Some driver writes complete immediately while DMA has room; yield to LVGL/buttons.
    vTaskDelay(pdMS_TO_TICKS(1));
}
static void input_task(void *arg) {
    (void)arg;
    int64_t last_input = esp_timer_get_time(), last_battery = last_input;
    bool dimmed = false, dark = false;
    for (;;) {
        input_t input;
        TickType_t wait = s_pcm_remaining ? 0 : pdMS_TO_TICKS(100);
        if (xQueueReceive(s_queue, &input, wait) == pdTRUE) {
            last_input = esp_timer_get_time();
            if (dimmed || dark) bsp_display_backlight(65);
            dimmed = false;
            if (dark) { dark = false; continue; } // First key only wakes the screen.
            if (input.event == BSP_BTN_LONG && input.key == BSP_BTN_DOWN) {
                if (s_model.page == KO_CARDS) play(s_model.card);
                if (s_model.page == KO_QUIZ) play(s_model.questions[s_model.round_pos]);
                render();
                continue;
            }
            if (input.event == BSP_BTN_LONG && input.key != BSP_BTN_OK) continue;
            ko_key_t key = input.event == BSP_BTN_LONG ? KO_BACK :
                input.key == BSP_BTN_UP ? KO_UP : input.key == BSP_BTN_DOWN ? KO_DOWN : KO_OK;
            if (!s_audio_ok && s_model.page == KO_HOME && s_model.menu == 3 && key == KO_OK) {
                s_model.page = KO_AUDIO_ERROR;
                render();
                continue;
            }
            stop_audio();
            ko_page_t old_page = s_model.page;
            unsigned old_pos = s_model.round_pos;
            bool dirty = ko_handle(&s_model, key);
            // No PCM writes run concurrently with NVS commits or codec suspend.
            if (dirty) {
                if (s_audio_awake) {
                    esp_err_t err = bsp_audio_sleep();
                    s_audio_awake = false;
                    if (err != ESP_OK) s_audio_ok = false;
                }
                save_progress();
            }
            if (s_model.page == KO_QUIZ && s_model.listening && !s_model.feedback &&
                (old_page != KO_QUIZ || old_pos != s_model.round_pos))
                play(s_model.questions[s_model.round_pos]);
            render();
        }
        if (s_pcm_remaining) feed_audio();
        int64_t now = esp_timer_get_time();
        if (!s_pcm_remaining && s_audio_awake && now - last_input > 2000000 && now - s_last_audio_write > 200000) {
            // DMA is finished long before this idle boundary.
            esp_err_t err = bsp_audio_sleep();
            s_audio_awake = false;
            if (err != ESP_OK) { s_audio_ok = false; ESP_LOGE(TAG, "Audio suspend: %s", esp_err_to_name(err)); render(); }
        }
        if (!dimmed && now - last_input > 30000000) { bsp_display_backlight(15); dimmed = true; }
        if (!dark && now - last_input > 90000000) { bsp_display_backlight(0); dark = true; }
        if (!s_pcm_remaining && now - last_battery > 30000000) {
            last_battery = now;
            s_battery = bsp_battery_soc();
            if (!dark) render();
        }
    }
}
void app_main(void) {
    ko_init(&s_model, esp_random());
    load_progress();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    s_audio_ok = bsp_audio_init() == ESP_OK;
    s_audio_awake = s_audio_ok;
    if (s_audio_ok) bsp_audio_set_volume(65);
    if (bsp_battery_init() == ESP_OK) s_battery = bsp_battery_soc();
    s_queue = xQueueCreate(12, sizeof(input_t));
    if (!s_queue) { ESP_LOGE(TAG, "Input queue allocation failed"); return; }
    s_buttons_ok = bsp_button_init(on_key, NULL) == ESP_OK;
    if (!bsp_lvgl_lock(1000)) return;
    s_screen = lv_obj_create(NULL);
    ko_ui_render(s_screen, &s_model, s_battery, s_audio_ok, s_storage_ok, s_buttons_ok);
    lv_screen_load(s_screen);
    bsp_lvgl_unlock();
    bsp_display_backlight(65);
    if (xTaskCreate(input_task, "korean_app", 6144, NULL, 5, NULL) != pdPASS) {
        s_buttons_ok = false;
        render();
        ESP_LOGE(TAG, "Input task allocation failed");
        return;
    }
    atomic_store(&s_ready, true);
    ESP_LOGI(TAG, "Korean course ready; free heap=%u largest block=%u",
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

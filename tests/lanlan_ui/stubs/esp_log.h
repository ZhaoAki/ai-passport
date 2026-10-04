/* Host stub for ESP-IDF's esp_log.h.
 *
 * The real main/lanlan_ui.c logs through ESP_LOGx. On the host these go to
 * stderr so the preview harness's own stdout stays machine-readable. The
 * format string of main/lanlan_ui.c is a plain printf format, so fprintf is a
 * faithful stand-in. */
#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, format, ...) fprintf(stderr, "E %s: " format "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) fprintf(stderr, "W %s: " format "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) fprintf(stderr, "I %s: " format "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) fprintf(stderr, "D %s: " format "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) fprintf(stderr, "V %s: " format "\n", (tag), ##__VA_ARGS__)

#define ESP_LOG_LEVEL_LOCAL(level, tag, format, ...) \
    fprintf(stderr, "%s: " format "\n", (tag), ##__VA_ARGS__)

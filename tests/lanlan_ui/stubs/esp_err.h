/* Host stub for ESP-IDF's esp_err.h.
 *
 * tests/lanlan_ui compiles the REAL main/lanlan_ui.c against host LVGL, so the
 * ESP-IDF headers it includes must resolve here. This stub provides only the
 * declarations that translation unit actually needs; it is never linked into
 * firmware. */
#pragma once

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107

const char *esp_err_to_name(esp_err_t code);

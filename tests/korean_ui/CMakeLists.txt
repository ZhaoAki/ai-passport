cmake_minimum_required(VERSION 3.16)
project(korean_ui_preview C)
set(CMAKE_C_STANDARD 11)
get_filename_component(APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${APP_ROOT}/managed_components/lvgl__lvgl" lvgl-build)
add_executable(korean_ui_preview render.c
    "${APP_ROOT}/main/korean_ui.c" "${APP_ROOT}/main/korean_model.c"
    "${APP_ROOT}/main/korean_course.c"
    "${APP_ROOT}/assets/fonts/korean_font_16.c" "${APP_ROOT}/assets/fonts/korean_font_28.c"
    "${APP_ROOT}/components/bsp/src/bsp_display_rounding.c")
target_include_directories(korean_ui_preview PRIVATE "${APP_ROOT}/main" "${APP_ROOT}/components/bsp/src")
target_link_libraries(korean_ui_preview PRIVATE lvgl m)

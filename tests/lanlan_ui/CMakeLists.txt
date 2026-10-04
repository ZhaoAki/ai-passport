cmake_minimum_required(VERSION 3.16)
project(lanlan_ui_preview C)
set(CMAKE_C_STANDARD 11)

# Host render harness for the REAL Cyber Lanlan application UI. It compiles
# main/lanlan_ui.c unchanged against the pinned managed LVGL component and a
# handful of ESP-IDF stub headers under tests/lanlan_ui/stubs/, so the screens
# produced here are the screens the firmware draws. See render.c.
get_filename_component(APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if(NOT EXISTS "${APP_ROOT}/managed_components/lvgl__lvgl/lvgl.h")
    message(FATAL_ERROR
        "Pinned host LVGL is missing at managed_components/lvgl__lvgl. "
        "Run ./tools/validate.sh --firmware once so the component manager "
        "downloads component version 9.5.0, then re-run the preview driver.")
endif()

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${APP_ROOT}/managed_components/lvgl__lvgl" lvgl-build)

set(LANLAN_SOURCES
    render.c
    "${APP_ROOT}/main/lanlan_ui.c"
    "${APP_ROOT}/main/lanlan_model.c"
    "${APP_ROOT}/main/lanlan_cache.c"
    "${APP_ROOT}/main/lanlan_record.c"
    "${APP_ROOT}/main/lanlan_reminder.c"
    "${APP_ROOT}/main/lanlan_time.c"
    "${APP_ROOT}/main/lanlan_strings.c"
    "${APP_ROOT}/assets/fonts/lanlan_font_16.c"
    "${APP_ROOT}/assets/fonts/lanlan_font_24.c"
    "${APP_ROOT}/assets/images/lanlan_sprites.c")

# A companion change moved the caregiver display names into their own
# translation unit (service user id -> slot -> display name). Compile it when it
# is present so the harness always links against the checked-out layout without
# editing anything under main/.
if(EXISTS "${APP_ROOT}/main/lanlan_caregiver.c")
    list(APPEND LANLAN_SOURCES "${APP_ROOT}/main/lanlan_caregiver.c")
endif()

add_executable(lanlan_ui_preview ${LANLAN_SOURCES})

target_include_directories(lanlan_ui_preview PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/stubs"
    "${APP_ROOT}/main"
    "${APP_ROOT}/assets/images"
    "${APP_ROOT}/assets/fonts")

target_link_libraries(lanlan_ui_preview PRIVATE lvgl m)

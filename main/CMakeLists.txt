# Cyber Lanlan application component.
#
# The Korean learning application sources and its font/PCM links are removed from
# this branch's build; the Korean files themselves are owned by another change.
idf_component_register(
    SRCS
        "main.c"
        "lanlan_cache.c"
        "lanlan_caregiver.c"
        "lanlan_json.c"
        "lanlan_model.c"
        "lanlan_record.c"
        "lanlan_reminder.c"
        "lanlan_sfx_data.c"
        "lanlan_strings.c"
        "lanlan_sync.c"
        "lanlan_sync_parse.c"
        "lanlan_time.c"
        "lanlan_ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        bsp
        esp_timer
        esp_hw_support
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_client
        esp-tls
        mbedtls
        console
        lwip
        lvgl
)

# Generated assets: the 16 px / 24 px Chinese subsets, the placeholder sprite
# frames and the 16 kHz PCM clip pack. `lanlan_sprites.h` is consumed by the UI,
# so the asset directory joins the private include path.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/images"
)
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/lanlan_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/lanlan_font_24.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/images/lanlan_sprites.c"
)
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../assets/music/lanlan_sfx_16k.pcm" BINARY)

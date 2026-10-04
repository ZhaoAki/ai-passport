idf_component_register(
    SRCS "main.c" "korean_model.c" "korean_course.c" "korean_ui.c" "korean_audio_data.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer esp_hw_support nvs_flash
)
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/korean_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/korean_font_28.c"
)
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../assets/music/korean_mms_16k.pcm" BINARY)

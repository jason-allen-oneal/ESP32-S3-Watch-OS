idf_component_register(
    SRCS "src/shell.cpp" "src/revenant_grid_v2.c"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core nightglass_services lvgl esp_timer
    EMBED_FILES "assets/revenant_grid_v2.rgb565")

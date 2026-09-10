idf_component_register(
    SRCS "src/shell.cpp" "src/revenant_grid_v2.c" "src/revenant_shell_v1.c" "src/openclaw_mark_v1.c"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core nightglass_services lvgl esp_timer esp_lvgl_port
    EMBED_FILES "assets/revenant_grid_v2.rgb565" "assets/revenant_shell_v1.rgb565" "assets/openclaw_mark_v1.a8")

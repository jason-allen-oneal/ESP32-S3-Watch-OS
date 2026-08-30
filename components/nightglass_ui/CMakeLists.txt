idf_component_register(
    SRCS "src/shell.cpp" "src/revenant_skull.c"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core nightglass_services lvgl esp_timer)

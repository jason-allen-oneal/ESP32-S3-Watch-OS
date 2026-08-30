idf_component_register(
    SRCS "src/shell.cpp"
    INCLUDE_DIRS "include"
    REQUIRES morrow_core morrow_services lvgl esp_timer)

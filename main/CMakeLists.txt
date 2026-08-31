idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash nightglass_core nightglass_bsp nightglass_services nightglass_ui
             nightglass_update)

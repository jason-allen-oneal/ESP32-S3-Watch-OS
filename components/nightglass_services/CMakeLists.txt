idf_component_register(
    SRCS "src/hardware.cpp" "src/power.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core nightglass_bsp esp_driver_i2c esp_driver_gpio esp_timer)

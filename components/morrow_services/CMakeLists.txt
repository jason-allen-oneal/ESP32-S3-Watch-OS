idf_component_register(
    SRCS "src/hardware.cpp"
    INCLUDE_DIRS "include"
    REQUIRES morrow_core esp_driver_i2c esp_driver_gpio esp_timer)

idf_component_register(
    SRCS "src/board.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core esp_driver_gpio esp_driver_i2c esp32_s3_touch_amoled_2_06)

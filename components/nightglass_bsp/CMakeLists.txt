idf_component_register(
    SRCS "src/board.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core esp_driver_gpio esp_driver_i2c esp_timer esp_lvgl_port
             esp_lcd espressif__esp_lcd_touch espressif__esp_lcd_touch_ft5x06
             esp32_s3_touch_amoled_2_06)

idf_component_register(
    SRCS "src/gyro_processor.cpp" "src/hardware.cpp" "src/power.cpp"
         "src/time_math.cpp" "src/clock.cpp" "src/watchface.cpp"
         "src/watchface_schema.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core nightglass_bsp esp_driver_i2c esp_driver_gpio esp_timer
             esp_hw_support nvs_flash)

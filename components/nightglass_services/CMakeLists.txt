idf_component_register(
    SRCS "src/activity.cpp" "src/activity_day.cpp" "src/activity_processor.cpp"
         "src/gyro_processor.cpp" "src/hardware.cpp" "src/power.cpp"
         "src/time_math.cpp" "src/clock.cpp" "src/watchface.cpp"
         "src/watchface_schema.cpp" "src/connectivity_protocol.cpp" "src/connectivity.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core nightglass_bsp esp_driver_i2c esp_driver_gpio esp_timer
             esp_driver_usb_serial_jtag esp_hw_support nvs_flash bt)

idf_component_register(
    SRCS "src/activity.cpp" "src/activity_day.cpp" "src/activity_processor.cpp"
         "src/activity_units.cpp"
         "src/gyro_processor.cpp" "src/hardware.cpp" "src/power.cpp"
         "src/time_math.cpp" "src/clock.cpp" "src/watchface.cpp"
         "src/watchface_schema.cpp" "src/connectivity_protocol.cpp" "src/connectivity.cpp"
         "src/weather_logic.cpp" "src/network_weather.cpp"
         "src/audio.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core nightglass_bsp esp_driver_i2c esp_driver_gpio esp_timer
             esp_driver_usb_serial_jtag esp_hw_support nvs_flash bt esp_wifi esp_event
             esp_netif esp_http_client mbedtls esp_codec_dev esp_driver_i2s esp_pm)

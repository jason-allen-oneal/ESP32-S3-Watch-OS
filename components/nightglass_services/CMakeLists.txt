set(NIGHTGLASS_SOUND_FILES)
foreach(sound_name notification call calendar email alarm timer success warning)
    set(sound_file "assets/${sound_name}.wav")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${sound_file}")
        list(APPEND NIGHTGLASS_SOUND_FILES "${sound_file}")
        string(TOUPPER "${sound_name}" sound_define)
        list(APPEND NIGHTGLASS_SOUND_DEFINES "NIGHTGLASS_SOUND_${sound_define}_EMBEDDED=1")
    endif()
endforeach()

set(NIGHTGLASS_SOUND_EMBED_ARGS)
if(NIGHTGLASS_SOUND_FILES)
    list(APPEND NIGHTGLASS_SOUND_EMBED_ARGS EMBED_FILES ${NIGHTGLASS_SOUND_FILES})
endif()

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
             esp_netif esp_http_client mbedtls esp_codec_dev esp_driver_i2s esp_pm
    ${NIGHTGLASS_SOUND_EMBED_ARGS})

if(NIGHTGLASS_SOUND_DEFINES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${NIGHTGLASS_SOUND_DEFINES})
endif()

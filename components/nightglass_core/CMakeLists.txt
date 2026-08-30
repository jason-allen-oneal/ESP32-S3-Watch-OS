idf_component_register(
    SRCS "src/health.cpp" "src/navigation.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer freertos)

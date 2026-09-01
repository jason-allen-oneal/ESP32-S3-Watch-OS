idf_component_register(
    SRCS "src/manifest.cpp" "src/policy.cpp" "src/service.cpp" "src/verifier.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nightglass_core app_update esp_partition esp_driver_gpio nvs_flash mbedtls)

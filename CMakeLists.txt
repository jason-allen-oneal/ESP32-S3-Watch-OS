cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nightglass)

# sdkconfig is intentionally local and ignored, so sdkconfig.defaults alone is
# not a release guarantee. Fail CMake configuration when the resolved config
# weakens a reviewed safety-critical setting.
execute_process(
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/scripts/verify-release-config.py
            ${CMAKE_CURRENT_LIST_DIR}/sdkconfig
    RESULT_VARIABLE NIGHTGLASS_RELEASE_CONFIG_RESULT
)
if(NOT NIGHTGLASS_RELEASE_CONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR "Nightglass resolved release configuration is unsafe")
endif()

# ESP-IDF contributes generic erase/full-flash/component-flash targets. Keep
# them visible for transparency but make their execution fail before esptool.
# Nightglass release writes require the separately reviewed update/recovery
# path; ordinary builds do not depend on this blocker.
add_custom_target(nightglass_block_generic_flash
    COMMAND ${CMAKE_CURRENT_LIST_DIR}/scripts/block-generic-flash.sh
    VERBATIM
)
set(NIGHTGLASS_BLOCKED_FLASH_TARGETS
    flash encrypted-flash erase_flash dfu-flash
    app-flash encrypted-app-flash
    bootloader-flash encrypted-bootloader-flash
    partition-table-flash partition_table-flash encrypted-partition-table-flash
    otadata-flash encrypted-otadata-flash
    assets-flash encrypted-assets-flash
)

# The runtime partition is compiled separately from the application. Reject
# unsupported paths and malformed audio before generating its image.
execute_process(
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/scripts/validate-runtime-assets.py
            ${CMAKE_CURRENT_LIST_DIR}/runtime_assets
    RESULT_VARIABLE NIGHTGLASS_ASSET_VALIDATION_RESULT
)
if(NOT NIGHTGLASS_ASSET_VALIDATION_RESULT EQUAL 0)
    message(FATAL_ERROR "Nightglass runtime asset validation failed")
endif()

# Generate the dedicated runtime asset image, but do not include it in the
# default flash target. Application-only updates must never overwrite user
# assets. Provision this partition explicitly after validating its hash.
littlefs_create_partition_image(assets runtime_assets)

# The LittleFS helper creates its flash targets above, after project() creates
# ESP-IDF's targets. Attach the blocker only once every generated target exists.
foreach(target_name IN LISTS NIGHTGLASS_BLOCKED_FLASH_TARGETS)
    if(TARGET ${target_name})
        add_dependencies(${target_name} nightglass_block_generic_flash)
    endif()
endforeach()

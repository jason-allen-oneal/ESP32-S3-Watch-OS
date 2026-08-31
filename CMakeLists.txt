cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nightglass)

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

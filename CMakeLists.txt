cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nightglass)

# Generate the dedicated runtime asset image, but do not include it in the
# default flash target. Application-only updates must never overwrite user
# assets. Provision this partition explicitly after validating its hash.
littlefs_create_partition_image(assets runtime_assets)

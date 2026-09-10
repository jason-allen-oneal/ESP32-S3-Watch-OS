#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

// Static OpenClaw mascot coverage, tinted by the existing voice-health state.
// Native-size A8 reads embedded rodata directly, without a runtime pixel buffer.
extern const uint8_t openclaw_mark_v1_a8_start[]
    asm("_binary_openclaw_mark_v1_a8_start");

const lv_image_dsc_t openclaw_mark_v1 = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_A8,
        .flags = 0,
        .w = 54,
        .h = 54,
        .stride = 54,
        .reserved_2 = 0,
    },
    .data_size = 54U * 54U,
    .data = openclaw_mark_v1_a8_start,
    .reserved = NULL,
};

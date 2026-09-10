#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

// Dedicated secondary-screen chrome. The full watch face remains separate.
// Fixed RGB565 pixels are read directly from embedded rodata, with no decode
// or full-frame runtime allocation.
extern const uint8_t revenant_shell_v1_rgb565_start[]
    asm("_binary_revenant_shell_v1_rgb565_start");

const lv_image_dsc_t revenant_shell_v1_background = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = 410,
        .h = 502,
        .stride = 820,
        .reserved_2 = 0,
    },
    .data_size = 410U * 502U * 2U,
    .data = revenant_shell_v1_rgb565_start,
    .reserved = NULL,
};

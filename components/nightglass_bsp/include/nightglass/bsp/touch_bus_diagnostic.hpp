#pragma once

#include <cstdint>

#include "esp_err.h"

namespace nightglass::bsp {

// Only the normal diagnostic's confirmed address NACK can request a quiet
// window. Requests are coalesced and separated by at least 20 monotonic seconds.
bool take_touch_bus_quiet_request();

struct TouchReadEvidence {
    std::uint32_t sequence{0};
    esp_err_t result{ESP_ERR_INVALID_STATE};
    std::int64_t completed_us{0};
};

// Completed genuine reads only, including their normal diagnostics. This
// snapshot is not a health update; the ordinary input callback owns health.
TouchReadEvidence touch_read_evidence();

// Post the existing LVGL touch event; never read or render on the caller task.
void request_touch_read();

// Try the LVGL lock for 20 ms, reread documented A5, and write ACTIVE (0) only
// if that read observes MONITOR (1). ACTIVE is an unwritten success; all other
// modes/read errors fail. No reset, delay, or health credit occurs here.
esp_err_t activate_touch_from_monitor();

// Real GPIO callback count, modulo 2^32; not contact or health evidence.
std::uint32_t touch_irq_sequence();

struct TouchBusQuietSample {
    esp_err_t count_result{ESP_ERR_INVALID_STATE};
    esp_err_t mode_result{ESP_ERR_INVALID_STATE};
    esp_err_t probe_result{ESP_ERR_INVALID_STATE};
    std::uint8_t point_count{0xff};
    std::uint8_t power_mode{0xff};
    std::uint32_t irq_sequence{0};
    int reset_level{-1};
    int irq_level{-1};
    bool locked{false};
    std::int64_t sampled_at_us{0};
};

// Try the LVGL lock for at most 20 ms, then make bounded raw I2C observations.
// No delay loops, writes, resets, driver-buffer changes, or touch-health credit.
// A missing handle leaves read/probe results INVALID_STATE; a skipped probe
// also remains INVALID_STATE, never a manufactured ACK. The caller may pause
// its own polling, but this helper does not promise exclusive/all-bus quiet.
TouchBusQuietSample sample_touch_bus_quiet_diagnostic();

}  // namespace nightglass::bsp

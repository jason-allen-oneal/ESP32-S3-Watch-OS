#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#if __has_include("driver/i2c_master.h")
#include "driver/i2c_master.h"
#else
// Host tests do not need an ESP-IDF bus; keep the service interface visible.
using i2c_master_bus_handle_t = void *;
#endif
#include "nightglass/core/status.hpp"

namespace nightglass::services {

struct AudioSnapshot {
    bool enabled{false};
    bool input_ready{false};
    bool output_ready{false};
    bool stereo_requested{true};
    std::uint32_t sample_rate_hz{16000};
    std::uint32_t frames_captured{0};
    std::uint32_t frames_played{0};
    std::uint32_t read_errors{0};
    std::uint32_t write_errors{0};
    std::uint32_t dropped_bytes{0};
    std::int64_t last_io_us{0};
};

// Pure helpers are intentionally small so protocol/audio policy can be tested
// on the host without ESP-IDF or a codec attached.
std::size_t bounded_audio_bytes(std::size_t requested) noexcept;
std::uint32_t pcm16_rms(const std::int16_t *samples, std::size_t count) noexcept;

class AudioService {
public:
    nightglass::core::Status start(i2c_master_bus_handle_t bus_handle);
    nightglass::core::Status capture(std::span<std::uint8_t> destination);
    nightglass::core::Status play(std::span<const std::uint8_t> samples);
    [[nodiscard]] AudioSnapshot snapshot() const;
};

AudioService &audio_service();

}  // namespace nightglass::services

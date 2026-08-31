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

enum class AudioOperation : std::uint8_t {
    none,
    playback,
    capture,
};

struct AudioSnapshot {
    std::uint32_t sequence{0};
    // `enabled` means the diagnostic surface is compiled and armed. It does
    // not mean a codec, microphone, or amplifier is powered at boot.
    bool enabled{false};
    // Set after the first explicit test begins; retained so the UI can
    // distinguish an armed service from a real failed test.
    bool hardware_initialized{false};
    bool hardware_failed{false};
    bool hardware_active{false};
    bool input_ready{false};
    bool output_ready{false};
    // These describe the Nightglass-owned PA GPIO46 physical gate, not the
    // codec mute register (whose vendor return path is not trustworthy).
    bool amplifier_disabled{true};
    bool amplifier_disabled_verified{false};
    bool operation_pending{false};
    bool last_operation_ok{false};
    AudioOperation operation{AudioOperation::none};
    bool stereo_requested{true};
    std::uint32_t sample_rate_hz{16000};
    std::uint8_t output_volume{0};
    std::uint32_t frames_captured{0};
    std::uint32_t frames_played{0};
    std::uint32_t read_errors{0};
    std::uint32_t write_errors{0};
    std::uint32_t dropped_bytes{0};
    std::uint32_t last_transfer_bytes{0};
    std::uint32_t last_capture_rms{0};
    std::uint8_t last_operation_status{0};
    std::int64_t last_io_us{0};
};

// Pure helpers are intentionally small so protocol/audio policy can be tested
// on the host without ESP-IDF or a codec attached.
std::size_t bounded_audio_bytes(std::size_t requested) noexcept;
std::uint32_t pcm16_rms(const std::int16_t *samples, std::size_t count) noexcept;

class AudioService {
public:
    nightglass::core::Status start(i2c_master_bus_handle_t bus_handle);
    // Queue one bounded diagnostic operation. The request returns before
    // codec/I2S I/O begins, keeping LVGL responsive. No samples are retained
    // or transmitted after the worker exits.
    nightglass::core::Status request_microphone_sample();
    nightglass::core::Status request_test_tone();
    [[nodiscard]] AudioSnapshot snapshot() const;
};

AudioService &audio_service();

}  // namespace nightglass::services

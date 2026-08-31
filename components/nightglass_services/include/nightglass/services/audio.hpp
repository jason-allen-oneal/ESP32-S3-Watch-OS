#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#if __has_include("driver/i2c_master.h")
#include "driver/i2c_master.h"
#else
// Host tests do not need an ESP-IDF bus; keep the service interface visible.
using i2c_master_bus_handle_t = void *;
#endif
#include "nightglass/core/status.hpp"

namespace nightglass::services {

inline constexpr std::uint32_t kAudioSampleRateHz = 16000;
inline constexpr std::uint32_t kTestToneDurationMs = 2000;
inline constexpr std::uint32_t kTestToneTransferBytes =
    kAudioSampleRateHz * kTestToneDurationMs / 1000U * sizeof(std::int16_t);

enum class SoundCue : std::uint8_t {
    test = 0,
    notification,
    call,
    calendar,
    email,
    alarm,
    timer,
    success,
    warning,
};

constexpr std::uint32_t sound_cue_duration_ms(SoundCue cue) noexcept {
    switch (cue) {
        case SoundCue::notification: return 420;
        case SoundCue::call: return 2000;
        case SoundCue::calendar: return 700;
        case SoundCue::email: return 620;
        case SoundCue::alarm: return 2000;
        case SoundCue::timer: return 1200;
        case SoundCue::success: return 500;
        case SoundCue::warning: return 800;
        case SoundCue::test: return kTestToneDurationMs;
    }
    return kTestToneDurationMs;
}

constexpr std::uint32_t sound_cue_transfer_bytes(SoundCue cue) noexcept {
    return kAudioSampleRateHz * sound_cue_duration_ms(cue) / 1000U *
           sizeof(std::int16_t);
}

struct Pcm16WavView {
    std::size_t data_offset{0};
    std::size_t data_size{0};
    bool valid{false};
};

enum class AudioOperation : std::uint8_t {
    none,
    playback,
    capture,
};

struct AudioSettings {
    std::uint8_t volume_percent{100};
    bool muted{false};
    bool do_not_disturb{false};
};

static_assert(std::is_trivially_copyable_v<AudioSettings>);

constexpr bool valid_audio_settings(const AudioSettings &settings) noexcept {
    return settings.volume_percent <= 100;
}

constexpr bool audio_cue_allowed(SoundCue cue, const AudioSettings &settings) noexcept {
    if (settings.muted || settings.volume_percent == 0) return false;
    if (!settings.do_not_disturb) return true;
    return cue == SoundCue::alarm || cue == SoundCue::timer || cue == SoundCue::test;
}

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
    SoundCue cue{SoundCue::test};
    bool stereo_requested{false};
    std::uint32_t sample_rate_hz{kAudioSampleRateHz};
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
    AudioSettings settings{};
    // Transient schedule-owned suppression; persisted quiet-hour settings live
    // with the clock because they depend on local civil time.
    bool scheduled_dnd{false};
};

// Pure helpers are intentionally small so protocol/audio policy can be tested
// on the host without ESP-IDF or a codec attached.
std::size_t bounded_audio_bytes(std::size_t requested) noexcept;
std::uint32_t pcm16_rms(const std::int16_t *samples, std::size_t count) noexcept;
Pcm16WavView parse_pcm16_mono_wav(std::span<const std::uint8_t> sound) noexcept;

class AudioService {
public:
    nightglass::core::Status start(i2c_master_bus_handle_t bus_handle);
    // Queue one bounded diagnostic operation. The request returns before
    // codec/I2S I/O begins, keeping LVGL responsive. No samples are retained
    // or transmitted after the worker exits.
    nightglass::core::Status request_microphone_sample();
    nightglass::core::Status request_test_tone();
    nightglass::core::Status request_sound(SoundCue cue);
    nightglass::core::Status update_settings(const AudioSettings &settings);
    void set_scheduled_dnd(bool active);
    [[nodiscard]] AudioSnapshot snapshot() const;
};

AudioService &audio_service();

}  // namespace nightglass::services

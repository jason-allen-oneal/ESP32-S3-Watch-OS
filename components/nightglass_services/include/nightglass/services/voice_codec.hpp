#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace nightglass::services {

inline constexpr std::uint32_t kVoiceCaptureSampleRateHz = 16000;
inline constexpr std::uint32_t kVoiceTransportSampleRateHz = 8000;
inline constexpr std::uint32_t kVoiceDefaultDurationSeconds = 60;
inline constexpr std::uint32_t kVoiceMaximumDurationSeconds = 300;
inline constexpr std::size_t kVoiceMaximumEncodedBytes =
    kVoiceTransportSampleRateHz * kVoiceMaximumDurationSeconds;

constexpr bool valid_voice_duration(std::uint32_t seconds) noexcept {
    return seconds == 30 || seconds == 60 || seconds == 120 || seconds == 300;
}

constexpr std::size_t voice_encoded_capacity(std::uint32_t seconds) noexcept {
    return valid_voice_duration(seconds)
               ? static_cast<std::size_t>(kVoiceTransportSampleRateHz) * seconds
               : 0;
}

constexpr std::uint32_t next_voice_duration(std::uint32_t seconds) noexcept {
    switch (seconds) {
        case 30: return 60;
        case 60: return 120;
        case 120: return 300;
        case 300: return 30;
        default: return kVoiceDefaultDurationSeconds;
    }
}

[[nodiscard]] std::uint8_t pcm16_to_mulaw(std::int16_t sample) noexcept;
[[nodiscard]] std::int16_t mulaw_to_pcm16(std::uint8_t sample) noexcept;

// Downsample 16 kHz mono PCM16 to 8 kHz with a bounded two-sample box filter,
// then encode each output sample as G.711 mu-law. Returns zero unless the
// input count is even and the output can hold the complete result.
[[nodiscard]] std::size_t encode_voice_mulaw_8khz(
    std::span<const std::int16_t> input,
    std::span<std::uint8_t> output) noexcept;

}  // namespace nightglass::services

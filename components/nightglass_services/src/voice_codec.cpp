#include "nightglass/services/voice_codec.hpp"

#include <algorithm>
#include <cstdint>

namespace nightglass::services {

std::uint8_t pcm16_to_mulaw(std::int16_t sample) noexcept {
    constexpr std::int32_t kBias = 0x84;
    constexpr std::int32_t kClip = 32635;
    std::int32_t magnitude = sample;
    const std::uint8_t sign = magnitude < 0 ? 0x80U : 0x00U;
    if (magnitude < 0) magnitude = -magnitude;
    magnitude = std::min(magnitude, kClip) + kBias;

    std::uint8_t exponent = 7;
    for (std::int32_t mask = 0x4000; exponent > 0 && (magnitude & mask) == 0;
         mask >>= 1, --exponent) {}
    const auto mantissa = static_cast<std::uint8_t>(
        (magnitude >> (static_cast<unsigned>(exponent) + 3U)) & 0x0f);
    return static_cast<std::uint8_t>(~(sign | (exponent << 4U) | mantissa));
}

std::int16_t mulaw_to_pcm16(std::uint8_t sample) noexcept {
    const auto encoded = static_cast<std::uint8_t>(~sample);
    const auto sign = encoded & 0x80U;
    const auto exponent = static_cast<unsigned>((encoded >> 4U) & 0x07U);
    const auto mantissa = static_cast<std::int32_t>(encoded & 0x0fU);
    const std::int32_t magnitude =
        (((mantissa << 3U) + 0x84) << exponent) - 0x84;
    const auto signed_sample = sign != 0 ? -magnitude : magnitude;
    return static_cast<std::int16_t>(std::clamp(
        signed_sample, std::int32_t{-32768}, std::int32_t{32767}));
}

std::size_t encode_voice_mulaw_8khz(std::span<const std::int16_t> input,
                                    std::span<std::uint8_t> output) noexcept {
    if ((input.size() & 1U) != 0 || output.size() < input.size() / 2U) return 0;
    const auto encoded_count = input.size() / 2U;
    for (std::size_t index = 0; index < encoded_count; ++index) {
        const auto left = static_cast<std::int32_t>(input[index * 2U]);
        const auto right = static_cast<std::int32_t>(input[index * 2U + 1U]);
        output[index] = pcm16_to_mulaw(
            static_cast<std::int16_t>((left + right) / 2));
    }
    return encoded_count;
}

}  // namespace nightglass::services

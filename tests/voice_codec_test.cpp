#include <array>
#include <cassert>
#include <cstdint>

#include "nightglass/services/voice_codec.hpp"

int main() {
    using namespace nightglass::services;
    static_assert(kVoiceDefaultDurationSeconds == 60);
    static_assert(kVoiceMaximumDurationSeconds == 300);
    static_assert(kVoiceMaximumEncodedBytes == 2'400'000);
    static_assert(voice_encoded_capacity(30) == 240'000);
    static_assert(voice_encoded_capacity(60) == 480'000);
    static_assert(voice_encoded_capacity(120) == 960'000);
    static_assert(voice_encoded_capacity(300) == 2'400'000);
    static_assert(voice_encoded_capacity(8) == 0);
    static_assert(next_voice_duration(30) == 60);
    static_assert(next_voice_duration(60) == 120);
    static_assert(next_voice_duration(120) == 300);
    static_assert(next_voice_duration(300) == 30);
    assert(pcm16_to_mulaw(0) == 0xff);
    assert(pcm16_to_mulaw(32124) == 0x80);
    assert(pcm16_to_mulaw(-32124) == 0x00);
    assert(mulaw_to_pcm16(0xff) == 0);
    assert(mulaw_to_pcm16(0x80) == 32124);
    assert(mulaw_to_pcm16(0x00) == -32124);

    const std::array<std::int16_t, 6> pcm{0, 0, 1000, 1000, -1000, -1000};
    std::array<std::uint8_t, 3> encoded{};
    assert(encode_voice_mulaw_8khz(pcm, encoded) == encoded.size());
    assert(encoded[0] == pcm16_to_mulaw(0));
    assert(encoded[1] == pcm16_to_mulaw(1000));
    assert(encoded[2] == pcm16_to_mulaw(-1000));
    std::array<std::uint8_t, 2> too_small{};
    assert(encode_voice_mulaw_8khz(pcm, too_small) == 0);
    assert(encode_voice_mulaw_8khz(
               std::span<const std::int16_t>(pcm.data(), 5), encoded) == 0);
}

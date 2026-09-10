#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "nightglass/services/voice_codec.hpp"

namespace nightglass::services {

inline constexpr std::uint8_t kVoiceProtocolVersion = 1;
inline constexpr std::size_t kVoiceMaximumFrameBytes = 244;
inline constexpr std::size_t kVoiceMaximumResponseBytes = 2048;
// Spoken replies are synthesized on the phone and transported as bounded
// 8 kHz G.711 mu-law. Keep the optional audio leg short enough that BLE can
// finish it without turning the watch into a recorder or retaining a large
// unbounded queue.
inline constexpr std::size_t kVoiceMaximumSpokenReplyBytes = 96'000;
inline constexpr std::size_t kVoiceDataHeaderBytes = 12;
inline constexpr std::size_t kVoiceAudioDataHeaderBytes = 14;
inline constexpr std::size_t kVoiceMaximumDataPayloadBytes =
    kVoiceMaximumFrameBytes - kVoiceDataHeaderBytes;
inline constexpr std::uint8_t kVoiceMaximumCredits = 8;
// Request-begin keeps the original 16-byte layout. The codec byte reserves
// its two high bits for per-turn destinations while codec 1 remains unchanged
// for older companions.
inline constexpr std::uint8_t kVoiceRequestFlagSpokenReplies = 0x80;
inline constexpr std::uint8_t kVoiceRequestFlagDiscordReply = 0x40;
inline constexpr std::uint8_t kVoiceRequestCodecMask = 0x3f;

enum class VoiceFrameKind : std::uint8_t {
    invalid = 0,
    request_begin = 0x40,
    request_data = 0x41,
    request_end = 0x42,
    request_cancel = 0x43,
    request_ack = 0x44,
    response_begin = 0x45,
    response_data = 0x46,
    response_end = 0x47,
    response_status = 0x48,
    health = 0x49,
    response_audio_begin = 0x4a,
    response_audio_data = 0x4b,
    response_audio_end = 0x4c,
};

enum class VoiceHealthState : std::uint8_t {
    unavailable = 0,
    degraded = 1,
    healthy = 2,
};

enum class VoiceStatus : std::uint8_t {
    ok = 0,
    cancelled = 1,
    busy = 2,
    invalid = 3,
    disconnected = 4,
    timeout = 5,
    gateway_unavailable = 6,
    scope_rejected = 7,
    processing_failed = 8,
};

struct VoiceFrame {
    VoiceFrameKind kind{VoiceFrameKind::invalid};
    std::uint32_t session_id{0};
    std::uint32_t response_id{0};
    std::uint32_t offset{0};
    std::uint32_t total_bytes{0};
    std::uint32_t crc32{0};
    std::uint16_t sequence{0};
    std::uint8_t credits{0};
    VoiceStatus status{VoiceStatus::invalid};
    VoiceHealthState health{VoiceHealthState::unavailable};
    std::uint8_t codec{0};
    std::uint8_t sample_rate_khz{0};
    std::span<const std::uint8_t> payload{};
};

inline constexpr std::int64_t kVoiceHealthStaleUs = 150'000'000;

constexpr bool voice_health_sequence_is_newer(std::uint32_t candidate,
                                              std::uint32_t previous) noexcept {
    if (candidate == 0 || candidate == previous) return false;
    if (previous == 0) return true;
    return static_cast<std::uint32_t>(candidate - previous) < 0x8000'0000U;
}

constexpr bool voice_health_is_stale(std::int64_t now_us,
                                     std::int64_t updated_us) noexcept {
    return updated_us <= 0 || now_us < updated_us ||
           now_us - updated_us > kVoiceHealthStaleUs;
}

struct EncodedVoiceFrame {
    std::array<std::uint8_t, kVoiceMaximumFrameBytes> bytes{};
    std::size_t size{0};
};

[[nodiscard]] std::uint32_t voice_crc32(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] bool parse_voice_frame(std::span<const std::uint8_t> frame,
                                     VoiceFrame &message) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_begin(std::uint32_t session_id,
                                                    std::uint32_t total_bytes,
                                                    std::uint32_t crc32,
                                                    bool spoken_replies,
                                                    bool discord_reply) noexcept;
[[nodiscard]] inline EncodedVoiceFrame encode_voice_begin(
    std::uint32_t session_id, std::uint32_t total_bytes,
    std::uint32_t crc32, bool spoken_replies) noexcept {
    return encode_voice_begin(session_id, total_bytes, crc32,
                              spoken_replies, false);
}
[[nodiscard]] inline EncodedVoiceFrame encode_voice_begin(
    std::uint32_t session_id, std::uint32_t total_bytes,
    std::uint32_t crc32) noexcept {
    return encode_voice_begin(session_id, total_bytes, crc32, false, false);
}
[[nodiscard]] EncodedVoiceFrame encode_voice_data(
    std::uint32_t session_id, std::uint16_t sequence, std::uint32_t offset,
    std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_end(std::uint32_t session_id,
                                                  std::uint32_t total_bytes,
                                                  std::uint32_t crc32) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_cancel(std::uint32_t session_id,
                                                     VoiceStatus reason) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_audio_begin(
    std::uint32_t session_id, std::uint32_t response_id, std::uint32_t total_bytes,
    std::uint32_t crc32) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_audio_data(
    std::uint32_t session_id, std::uint32_t response_id, std::uint32_t offset,
    std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_audio_end(
    std::uint32_t session_id, std::uint32_t response_id, std::uint32_t total_bytes,
    std::uint32_t crc32) noexcept;

}  // namespace nightglass::services

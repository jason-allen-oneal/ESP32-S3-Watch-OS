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
inline constexpr std::size_t kVoiceDataHeaderBytes = 12;
inline constexpr std::size_t kVoiceMaximumDataPayloadBytes =
    kVoiceMaximumFrameBytes - kVoiceDataHeaderBytes;
inline constexpr std::uint8_t kVoiceMaximumCredits = 8;

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
    std::span<const std::uint8_t> payload{};
};

struct EncodedVoiceFrame {
    std::array<std::uint8_t, kVoiceMaximumFrameBytes> bytes{};
    std::size_t size{0};
};

[[nodiscard]] std::uint32_t voice_crc32(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] bool parse_voice_frame(std::span<const std::uint8_t> frame,
                                     VoiceFrame &message) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_begin(std::uint32_t session_id,
                                                    std::uint32_t total_bytes,
                                                    std::uint32_t crc32) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_data(
    std::uint32_t session_id, std::uint16_t sequence, std::uint32_t offset,
    std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_end(std::uint32_t session_id,
                                                  std::uint32_t total_bytes,
                                                  std::uint32_t crc32) noexcept;
[[nodiscard]] EncodedVoiceFrame encode_voice_cancel(std::uint32_t session_id,
                                                     VoiceStatus reason) noexcept;

}  // namespace nightglass::services

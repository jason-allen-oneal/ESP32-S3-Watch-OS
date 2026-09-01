#include "nightglass/services/voice_protocol.hpp"

#include <algorithm>

namespace nightglass::services {
namespace {

void write_u16(std::uint8_t *out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8U);
}

void write_u32(std::uint8_t *out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8U);
    out[2] = static_cast<std::uint8_t>(value >> 16U);
    out[3] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint16_t read_u16(const std::uint8_t *in) {
    return static_cast<std::uint16_t>(in[0]) |
           static_cast<std::uint16_t>(in[1] << 8U);
}

std::uint32_t read_u32(const std::uint8_t *in) {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8U) |
           (static_cast<std::uint32_t>(in[2]) << 16U) |
           (static_cast<std::uint32_t>(in[3]) << 24U);
}

bool valid_common(std::span<const std::uint8_t> frame, VoiceFrame &message,
                  std::size_t minimum) {
    if (frame.size() < minimum || frame[0] != kVoiceProtocolVersion) return false;
    message.kind = static_cast<VoiceFrameKind>(frame[1]);
    message.session_id = read_u32(frame.data() + 2);
    return message.session_id != 0;
}

EncodedVoiceFrame fixed_frame(VoiceFrameKind kind, std::uint32_t session_id,
                              std::size_t size) {
    EncodedVoiceFrame frame{};
    if (session_id == 0 || size > frame.bytes.size()) return frame;
    frame.bytes[0] = kVoiceProtocolVersion;
    frame.bytes[1] = static_cast<std::uint8_t>(kind);
    write_u32(frame.bytes.data() + 2, session_id);
    frame.size = size;
    return frame;
}

}  // namespace

std::uint32_t voice_crc32(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : data) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

bool parse_voice_frame(std::span<const std::uint8_t> frame,
                       VoiceFrame &message) noexcept {
    message = {};
    if (frame.size() < 2 || frame[0] != kVoiceProtocolVersion) return false;
    const auto kind = static_cast<VoiceFrameKind>(frame[1]);
    if (kind == VoiceFrameKind::request_ack) {
        if (!valid_common(frame, message, 12) || frame.size() != 12) return false;
        message.offset = read_u32(frame.data() + 6);
        message.credits = frame[10];
        message.status = static_cast<VoiceStatus>(frame[11]);
        return message.offset <= kVoiceMaximumEncodedBytes &&
               message.credits <= kVoiceMaximumCredits &&
               static_cast<std::uint8_t>(message.status) <=
                   static_cast<std::uint8_t>(VoiceStatus::processing_failed);
    }
    if (kind == VoiceFrameKind::response_begin) {
        if (!valid_common(frame, message, 16) || frame.size() != 16) return false;
        message.response_id = read_u32(frame.data() + 6);
        message.total_bytes = read_u16(frame.data() + 10);
        message.crc32 = read_u32(frame.data() + 12);
        return message.response_id != 0 && message.total_bytes != 0 &&
               message.total_bytes <= kVoiceMaximumResponseBytes;
    }
    if (kind == VoiceFrameKind::response_data) {
        if (!valid_common(frame, message, kVoiceDataHeaderBytes) ||
            frame.size() > kVoiceMaximumFrameBytes) return false;
        message.response_id = read_u32(frame.data() + 6);
        message.offset = read_u16(frame.data() + 10);
        message.payload = frame.subspan(kVoiceDataHeaderBytes);
        return message.response_id != 0 && !message.payload.empty() &&
               message.offset <= kVoiceMaximumResponseBytes &&
               message.payload.size() <= kVoiceMaximumResponseBytes - message.offset;
    }
    if (kind == VoiceFrameKind::response_end) {
        if (!valid_common(frame, message, 16) || frame.size() != 16) return false;
        message.response_id = read_u32(frame.data() + 6);
        message.total_bytes = read_u16(frame.data() + 10);
        message.crc32 = read_u32(frame.data() + 12);
        return message.response_id != 0 && message.total_bytes != 0 &&
               message.total_bytes <= kVoiceMaximumResponseBytes;
    }
    if (kind == VoiceFrameKind::response_status) {
        if (!valid_common(frame, message, 7) || frame.size() != 7) return false;
        message.status = static_cast<VoiceStatus>(frame[6]);
        return message.status != VoiceStatus::ok &&
               static_cast<std::uint8_t>(message.status) <=
               static_cast<std::uint8_t>(VoiceStatus::processing_failed);
    }
    if (kind == VoiceFrameKind::health) {
        if (!valid_common(frame, message, 7) || frame.size() != 7) return false;
        message.health = static_cast<VoiceHealthState>(frame[6]);
        return message.health >= VoiceHealthState::unavailable &&
               message.health <= VoiceHealthState::healthy;
    }
    return false;
}

EncodedVoiceFrame encode_voice_begin(std::uint32_t session_id,
                                     std::uint32_t total_bytes,
                                     std::uint32_t crc32) noexcept {
    auto frame = fixed_frame(VoiceFrameKind::request_begin, session_id, 16);
    if (frame.size == 0 || total_bytes == 0 || total_bytes > kVoiceMaximumEncodedBytes) {
        return {};
    }
    write_u32(frame.bytes.data() + 6, total_bytes);
    write_u32(frame.bytes.data() + 10, crc32);
    frame.bytes[14] = 1;  // G.711 mu-law.
    frame.bytes[15] = 8;  // kHz.
    return frame;
}

EncodedVoiceFrame encode_voice_data(std::uint32_t session_id,
                                    std::uint16_t sequence,
                                    std::uint32_t offset,
                                    std::span<const std::uint8_t> payload) noexcept {
    auto frame = fixed_frame(VoiceFrameKind::request_data, session_id,
                             kVoiceDataHeaderBytes + payload.size());
    if (frame.size == 0 || sequence == 0 || payload.empty() ||
        payload.size() > kVoiceMaximumDataPayloadBytes ||
        offset > kVoiceMaximumEncodedBytes ||
        payload.size() > kVoiceMaximumEncodedBytes - offset) return {};
    write_u16(frame.bytes.data() + 6, sequence);
    write_u32(frame.bytes.data() + 8, offset);
    std::copy(payload.begin(), payload.end(), frame.bytes.begin() + kVoiceDataHeaderBytes);
    return frame;
}

EncodedVoiceFrame encode_voice_end(std::uint32_t session_id,
                                   std::uint32_t total_bytes,
                                   std::uint32_t crc32) noexcept {
    auto frame = fixed_frame(VoiceFrameKind::request_end, session_id, 14);
    if (frame.size == 0 || total_bytes == 0 || total_bytes > kVoiceMaximumEncodedBytes) {
        return {};
    }
    write_u32(frame.bytes.data() + 6, total_bytes);
    write_u32(frame.bytes.data() + 10, crc32);
    return frame;
}

EncodedVoiceFrame encode_voice_cancel(std::uint32_t session_id,
                                      VoiceStatus reason) noexcept {
    auto frame = fixed_frame(VoiceFrameKind::request_cancel, session_id, 7);
    if (frame.size == 0 || reason == VoiceStatus::ok ||
        static_cast<std::uint8_t>(reason) >
            static_cast<std::uint8_t>(VoiceStatus::processing_failed)) return {};
    frame.bytes[6] = static_cast<std::uint8_t>(reason);
    return frame;
}

}  // namespace nightglass::services

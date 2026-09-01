#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "nightglass/services/voice_protocol.hpp"

int main() {
    using namespace nightglass::services;
    const std::array<std::uint8_t, 9> body{'N','i','g','h','t','g','l','a','s'};
    assert(voice_crc32(body) == 0xcbcf4966U);
    const auto begin = encode_voice_begin(7, body.size(), voice_crc32(body));
    assert(begin.size == 16 && begin.bytes[1] == 0x40 && begin.bytes[14] == 1);
    assert(encode_voice_begin(0, body.size(), 1).size == 0);
    assert(encode_voice_begin(7, 0, 1).size == 0);
    const auto maximum_begin = encode_voice_begin(
        7, kVoiceMaximumEncodedBytes, 0x12345678U);
    assert(maximum_begin.size == 16);
    assert(encode_voice_begin(7, kVoiceMaximumEncodedBytes + 1U, 1).size == 0);

    const auto data = encode_voice_data(7, 1, 0, body);
    assert(data.size == 21 && data.bytes[1] == 0x41 && data.bytes[6] == 1);
    assert(encode_voice_data(7, 0, 0, body).size == 0);
    std::array<std::uint8_t, kVoiceMaximumDataPayloadBytes + 1> oversized{};
    assert(encode_voice_data(7, 1, 0, oversized).size == 0);
    const std::array<std::uint8_t, 1> final_byte{0x55};
    assert(encode_voice_data(7, 0xffffU,
                             kVoiceMaximumEncodedBytes - 1U,
                             final_byte).size == 13);
    const auto end = encode_voice_end(7, body.size(), voice_crc32(body));
    assert(end.size == 14 && end.bytes[1] == 0x42);
    assert(encode_voice_cancel(7, VoiceStatus::cancelled).size == 7);
    assert(encode_voice_cancel(7, VoiceStatus::ok).size == 0);

    VoiceFrame frame{};
    const std::array<std::uint8_t, 12> ack{1, 0x44, 7, 0, 0, 0,
        9, 0, 0, 0, 4, 0};
    assert(parse_voice_frame(ack, frame));
    assert(frame.kind == VoiceFrameKind::request_ack && frame.offset == 9 &&
           frame.credits == 4 && frame.status == VoiceStatus::ok);
    auto invalid_ack = ack;
    invalid_ack[10] = kVoiceMaximumCredits + 1;
    assert(!parse_voice_frame(invalid_ack, frame));

    const std::array<std::uint8_t, 16> response_begin{1, 0x45, 7, 0, 0, 0,
        8, 0, 0, 0, 9, 0, 0x66, 0x49, 0xcf, 0xcb};
    assert(parse_voice_frame(response_begin, frame));
    assert(frame.response_id == 8 && frame.total_bytes == 9);
    std::vector<std::uint8_t> response_data{1, 0x46, 7, 0, 0, 0,
        8, 0, 0, 0, 0, 0};
    response_data.insert(response_data.end(), body.begin(), body.end());
    assert(parse_voice_frame(response_data, frame));
    assert(frame.payload.size() == body.size() && frame.payload[0] == 'N');
    response_data[2] = 9;
    assert(parse_voice_frame(response_data, frame));  // Session validity is contextual.

    const std::array<std::uint8_t, 7> status{1, 0x48, 7, 0, 0, 0, 6};
    assert(parse_voice_frame(status, frame));
    assert(frame.status == VoiceStatus::gateway_unavailable);
    auto impossible_success_status = status;
    impossible_success_status[6] = static_cast<std::uint8_t>(VoiceStatus::ok);
    assert(!parse_voice_frame(impossible_success_status, frame));
    auto invalid_status = status;
    invalid_status[6] = 0xff;
    assert(!parse_voice_frame(invalid_status, frame));

    const std::array<std::uint8_t, 7> health{1, 0x49, 9, 0, 0, 0, 2};
    assert(parse_voice_frame(health, frame));
    assert(frame.kind == VoiceFrameKind::health &&
           frame.session_id == 9 && frame.health == VoiceHealthState::healthy);
    auto invalid_health = health;
    invalid_health[6] = 3;
    assert(!parse_voice_frame(invalid_health, frame));
    auto zero_health_sequence = health;
    zero_health_sequence[2] = 0;
    assert(!parse_voice_frame(zero_health_sequence, frame));

    assert(voice_health_sequence_is_newer(2, 1));
    assert(!voice_health_sequence_is_newer(1, 1));
    assert(!voice_health_sequence_is_newer(1, 2));
    assert(voice_health_sequence_is_newer(1, 0xffff'ffffU));
    assert(voice_health_is_stale(kVoiceHealthStaleUs + 2, 1));
    assert(!voice_health_is_stale(kVoiceHealthStaleUs, 1));
}

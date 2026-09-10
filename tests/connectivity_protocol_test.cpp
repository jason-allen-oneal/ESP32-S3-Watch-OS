#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "nightglass/services/connectivity_protocol.hpp"

int main() {
    using namespace nightglass::services;
    std::vector<std::uint8_t> frame{kCompanionProtocolVersion, 1, 0x78, 0x56, 0x34, 0x12,
                                    0xC1, 3, 5, 5, 0};
    frame.insert(frame.end(), {'S', 'M', 'S'});
    frame.insert(frame.end(), {'H', 'e', 'l', 'l', 'o'});
    frame.insert(frame.end(), {'A', 0xf0, 'B', '\n', 'C'});
    CompanionMessage message{};
    assert(parse_companion_message(frame, message));
    assert(message.kind == CompanionMessageKind::notification_upsert);
    assert(message.notification.id == 0x12345678);
    assert(message.notification.category == NotificationCategory::message);
    assert(message.notification.alert);
    assert(message.notification.replyable);
    assert(message.notification.body[1] == '?');
    assert(message.notification.body[3] == '?');
    frame[7] = 25;
    assert(!parse_companion_message(frame, message));
    const std::array<std::uint8_t, 2> clear{kCompanionProtocolVersion, 3};
    assert(parse_companion_message(clear, message));
    assert(message.kind == CompanionMessageKind::notification_clear);
    const auto media = encode_media_command(MediaCommand::next, 9);
    assert(media[1] == 0x10 && media[2] == 9 && media[3] == 2);
    assert(encode_media_command(MediaCommand::stop, 10)[3] == 8);
    assert(encode_media_command(MediaCommand::restart, 11)[3] == 9);
    const auto action = encode_notification_action(true, 0x12345678, 4);
    assert(action[1] == 0x11 && action[3] == 0x78 && action[6] == 0x12);
    const auto reply = encode_notification_reply(0x12345678, 7, 0xAABBCCDD, "Yes");
    assert(reply.size == 15 && reply.bytes[1] == 0x13 && reply.bytes[11] == 3);
    assert(reply.bytes[12] == 'Y' && reply.bytes[14] == 's');
    assert(encode_notification_reply(0, 1, 2, "x").size == 0);
    const std::array<std::uint8_t, 12> reply_result{
        1, 0x24, 7, 0, 0x78, 0x56, 0x34, 0x12, 0xDD, 0xCC, 0xBB, 0xAA};
    assert(parse_companion_message(reply_result, message));
    assert(message.kind == CompanionMessageKind::reply_result);
    assert(message.reply_result.request_nonce == 0xAABBCCDD);
    auto invalid_reply_result = reply_result;
    invalid_reply_result[2] = 0;
    assert(!parse_companion_message(invalid_reply_result, message));
    const std::array<std::uint8_t, 13> media_state{
        1, 4, 3, 4, 4, 'S', 'o', 'n', 'g', 'B', 'a', 'n', 'd'};
    assert(parse_companion_message(media_state, message));
    assert(message.kind == CompanionMessageKind::media_state);
    assert(message.media.playing && message.media.available);
    const std::array<std::uint8_t, 21> media_progress{
        1, 7, 7, 4, 4, 0x98, 0x3A, 0, 0, 0xC0, 0xD4, 0x01, 0,
        'S', 'o', 'n', 'g', 'B', 'a', 'n', 'd'};
    assert(parse_companion_message(media_progress, message));
    assert(message.media.seekable && message.media.position_ms == 15000 &&
           message.media.duration_ms == 120000);
    const std::array<std::uint8_t, 26> agenda{
        1, 5, 1, 0, 0xF1, 0x53, 0x65, 0x10, 0xFF, 0x53, 0x65,
        0, 7, 5, 'S', 't', 'a', 'n', 'd', 'u', 'p', 'R', 'o', 'o', 'm'};
    assert(parse_companion_message(agenda, message));
    assert(message.kind == CompanionMessageKind::agenda && message.agenda.count == 1);
    const std::array<std::uint8_t, 5> battery{1, 6, 73, 3, 0};
    assert(parse_companion_message(battery, message));
    const std::array<std::uint8_t, 18> call{1, 8, 0x19, 8,
        0x78, 0x56, 0x34, 0x12, 2, 0,
        'I', 'n', 'c', 'o', 'm', 'i', 'n', 'g'};
    assert(parse_companion_message(call, message));
    assert(message.call.session_id == 0x12345678 && message.call.generation == 2);
    auto invalid_call = call; invalid_call[2] = 0x08;
    assert(!parse_companion_message(invalid_call, message));
    invalid_call = call;
    invalid_call[8] = 0;
    assert(!parse_companion_message(invalid_call, message));
    const auto call_command = encode_call_command(
        CallCommand::answer, 0xfffe, 0x12345678, 2);
    assert(call_command[1] == 0x14 && call_command[2] == 0xfe &&
           call_command[3] == 0xff && call_command[4] == 1 &&
           call_command[5] == 0x78 && call_command[9] == 2);
    const auto invalid_call_command =
        encode_call_command(CallCommand::answer, 0, 0x12345678, 2);
    assert((invalid_call_command == std::array<std::uint8_t, 11>{}));
    assert(encode_phone_command(PhoneCommand::camera, 4)[1] == 0x15);
    assert(encode_phone_command(PhoneCommand::launch_spotify, 5)[3] == 4);
    assert(encode_phone_command(PhoneCommand::launch_discord, 6)[3] == 5);
    const std::array<std::uint8_t, 10> wifi{1, 0x20, 3, 3, 'N', 'e', 't', 's', 'e', 'c'};
    assert(parse_companion_message(wifi, message));
    assert(message.kind == CompanionMessageKind::wifi_provision);
    assert(message.wifi.ssid_length == 3 && message.wifi.password_length == 3);
    const std::array<std::uint8_t, 2> forget_peer{kCompanionProtocolVersion, 0x25};
    assert(parse_companion_message(forget_peer, message));
    assert(message.kind == CompanionMessageKind::peer_forget);
    const std::array<std::uint8_t, 14> weather{1, 0x21, 3, 1, 30, 0,
                                               0, 0, 0, 0, 0, 0, 0, 0};
    assert(parse_companion_message(weather, message));
    assert(message.kind == CompanionMessageKind::weather_settings);
    assert(message.weather.metric && message.weather.location_configured);
    auto invalid_weather = weather;
    invalid_weather[4] = 5;
    assert(!parse_companion_message(invalid_weather, message));

    const CompanionPeerIdentity pinned{1, {1, 2, 3, 4, 5, 6}};
    const CompanionPeerIdentity same{1, {1, 2, 3, 4, 5, 6}};
    const CompanionPeerIdentity wrong{1, {1, 2, 3, 4, 5, 7}};
    assert(valid_peer_identity(pinned));
    assert(peer_identity_matches(pinned, same));
    assert(!peer_identity_matches(pinned, wrong));
    assert(!valid_peer_identity(CompanionPeerIdentity{}));
    assert(authorization_matches(7, 11, pinned, 7, 11, same));
    assert(!authorization_matches(7, 11, pinned, 8, 11, same));
    assert(!authorization_matches(7, 11, pinned, 7, 12, same));
    assert(!authorization_matches(7, 11, pinned, 7, 11, wrong));
    assert(!authorization_matches(7, 0, pinned, 7, 0, same));
    assert(repeat_pairing_reset_allowed(CompanionPeerIdentity{}, false));
    assert(repeat_pairing_reset_allowed(pinned, true));
    assert(!repeat_pairing_reset_allowed(pinned, false));

    const std::array<std::uint8_t, 18> proxy{
        1, 0x23, 0x03, 0,
        0x80, 0xD9, 0xB4, 0x68,  // 2025+ epoch, little-endian.
        0x1E, 0x00,              // phone reports 30 seconds of age
        0xD4, 0x02,              // 72.4
        0xE5, 0x02,              // 74.1
        0x03, 0x00,              // weather code 3
        0x57, 0x00};             // wind 8.7
    assert(parse_companion_message(proxy, message));
    assert(message.kind == CompanionMessageKind::weather_snapshot);
    assert(message.weather_snapshot.metric && message.weather_snapshot.is_day);
    assert(message.weather_snapshot.age_seconds == 30);
    assert(message.weather_snapshot.temperature_tenths == 724);
    assert(message.weather_snapshot.apparent_temperature_tenths == 741);
    assert(message.weather_snapshot.weather_code == 3);
    assert(message.weather_snapshot.wind_tenths == 87);
    auto invalid_proxy = proxy;
    invalid_proxy[3] = 1;
    assert(!parse_companion_message(invalid_proxy, message));
    invalid_proxy = proxy;
    invalid_proxy[14] = 0xE8;  // code 1000
    invalid_proxy[15] = 0x03;
    assert(!parse_companion_message(invalid_proxy, message));
}

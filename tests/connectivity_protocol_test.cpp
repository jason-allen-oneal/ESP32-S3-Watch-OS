#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "nightglass/services/connectivity_protocol.hpp"

int main() {
    using namespace nightglass::services;
    std::vector<std::uint8_t> frame{kCompanionProtocolVersion, 1, 0x78, 0x56, 0x34, 0x12,
                                    1, 3, 5, 5, 0};
    frame.insert(frame.end(), {'S', 'M', 'S'});
    frame.insert(frame.end(), {'H', 'e', 'l', 'l', 'o'});
    frame.insert(frame.end(), {'A', 0xf0, 'B', '\n', 'C'});
    CompanionMessage message{};
    assert(parse_companion_message(frame, message));
    assert(message.kind == CompanionMessageKind::notification_upsert);
    assert(message.notification.id == 0x12345678);
    assert(message.notification.body[1] == '?');
    assert(message.notification.body[3] == '?');
    frame[7] = 25;
    assert(!parse_companion_message(frame, message));
    const std::array<std::uint8_t, 2> clear{kCompanionProtocolVersion, 3};
    assert(parse_companion_message(clear, message));
    assert(message.kind == CompanionMessageKind::notification_clear);
    const auto media = encode_media_command(MediaCommand::next, 9);
    assert(media[1] == 0x10 && media[2] == 9 && media[3] == 2);
    const auto action = encode_notification_action(true, 0x12345678, 4);
    assert(action[1] == 0x11 && action[3] == 0x78 && action[6] == 0x12);
    const std::array<std::uint8_t, 10> wifi{1, 0x20, 3, 3, 'N', 'e', 't', 's', 'e', 'c'};
    assert(parse_companion_message(wifi, message));
    assert(message.kind == CompanionMessageKind::wifi_provision);
    assert(message.wifi.ssid_length == 3 && message.wifi.password_length == 3);
    const std::array<std::uint8_t, 14> weather{1, 0x21, 3, 1, 30, 0,
                                               0, 0, 0, 0, 0, 0, 0, 0};
    assert(parse_companion_message(weather, message));
    assert(message.kind == CompanionMessageKind::weather_settings);
    assert(message.weather.metric && message.weather.location_configured);
    auto invalid_weather = weather;
    invalid_weather[4] = 5;
    assert(!parse_companion_message(invalid_weather, message));

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

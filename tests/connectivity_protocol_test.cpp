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
}

#include "nightglass/services/connectivity_protocol.hpp"

#include <algorithm>

namespace nightglass::services {
namespace {
constexpr std::uint8_t kUpsert = 0x01;
constexpr std::uint8_t kRemove = 0x02;
constexpr std::uint8_t kClear = 0x03;
constexpr std::uint8_t kMedia = 0x10;
constexpr std::uint8_t kDismiss = 0x11;
constexpr std::uint8_t kRead = 0x12;

std::uint32_t read_u32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

template <std::size_t N>
void copy_ascii(std::array<char, N> &destination, const std::uint8_t *source,
                std::size_t length) {
    const auto count = std::min(length, N - 1);
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = source[index];
        destination[index] = value >= 0x20 && value <= 0x7e ? static_cast<char>(value) : '?';
    }
    destination[count] = '\0';
}

void write_u32(std::uint8_t *destination, std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
    destination[2] = static_cast<std::uint8_t>(value >> 16U);
    destination[3] = static_cast<std::uint8_t>(value >> 24U);
}
}  // namespace

bool parse_companion_message(std::span<const std::uint8_t> frame,
                             CompanionMessage &message) noexcept {
    message = {};
    if (frame.size() < 2 || frame[0] != kCompanionProtocolVersion) return false;
    if (frame[1] == kClear) {
        if (frame.size() != 2) return false;
        message.kind = CompanionMessageKind::notification_clear;
        return true;
    }
    if (frame[1] == kRemove) {
        if (frame.size() != 6) return false;
        message.kind = CompanionMessageKind::notification_remove;
        message.notification_id = read_u32(frame.data() + 2);
        return message.notification_id != 0;
    }
    if (frame[1] != kUpsert || frame.size() < 11) return false;
    const auto app_length = static_cast<std::size_t>(frame[7]);
    const auto title_length = static_cast<std::size_t>(frame[8]);
    const auto body_length = static_cast<std::size_t>(frame[9]) |
                             (static_cast<std::size_t>(frame[10]) << 8U);
    if (app_length > 24 || title_length > 48 || body_length > 96) return false;
    if (frame.size() != 11 + app_length + title_length + body_length) return false;
    auto &notification = message.notification;
    notification.id = read_u32(frame.data() + 2);
    if (notification.id == 0) return false;
    notification.category = frame[6] <= static_cast<std::uint8_t>(NotificationCategory::social)
                                ? static_cast<NotificationCategory>(frame[6])
                                : NotificationCategory::other;
    auto offset = std::size_t{11};
    copy_ascii(notification.app, frame.data() + offset, app_length);
    offset += app_length;
    copy_ascii(notification.title, frame.data() + offset, title_length);
    offset += title_length;
    copy_ascii(notification.body, frame.data() + offset, body_length);
    notification.valid = true;
    message.kind = CompanionMessageKind::notification_upsert;
    message.notification_id = notification.id;
    return true;
}

std::array<std::uint8_t, 4> encode_media_command(MediaCommand command,
                                                  std::uint8_t sequence) noexcept {
    return {kCompanionProtocolVersion, kMedia, sequence, static_cast<std::uint8_t>(command)};
}

std::array<std::uint8_t, 7> encode_notification_action(
    bool dismiss, std::uint32_t notification_id, std::uint8_t sequence) noexcept {
    std::array<std::uint8_t, 7> frame{kCompanionProtocolVersion, dismiss ? kDismiss : kRead,
                                      sequence, 0, 0, 0, 0};
    write_u32(frame.data() + 3, notification_id);
    return frame;
}
}  // namespace nightglass::services

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nightglass::services {

constexpr std::uint8_t kCompanionProtocolVersion = 1;
constexpr std::size_t kNotificationCapacity = 6;

enum class NotificationCategory : std::uint8_t {
    other = 0, message = 1, call = 2, email = 3, calendar = 4, social = 5,
};

enum class MediaCommand : std::uint8_t {
    play_pause = 1, next = 2, previous = 3, volume_up = 4, volume_down = 5,
};

struct CompanionNotification {
    std::uint32_t id{0};
    NotificationCategory category{NotificationCategory::other};
    std::array<char, 25> app{};
    std::array<char, 49> title{};
    std::array<char, 97> body{};
    bool valid{false};
};

enum class CompanionMessageKind : std::uint8_t {
    invalid = 0, notification_upsert, notification_remove, notification_clear,
};

struct CompanionMessage {
    CompanionMessageKind kind{CompanionMessageKind::invalid};
    CompanionNotification notification{};
    std::uint32_t notification_id{0};
};

[[nodiscard]] bool parse_companion_message(std::span<const std::uint8_t> frame,
                                           CompanionMessage &message) noexcept;
[[nodiscard]] std::array<std::uint8_t, 4> encode_media_command(MediaCommand command,
                                                               std::uint8_t sequence) noexcept;
[[nodiscard]] std::array<std::uint8_t, 7> encode_notification_action(
    bool dismiss, std::uint32_t notification_id, std::uint8_t sequence) noexcept;

}  // namespace nightglass::services

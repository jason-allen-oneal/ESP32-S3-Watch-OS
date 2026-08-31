#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

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
    // True only for a newly posted phone notification. Replayed cache syncs
    // remain silent so reconnecting cannot produce an alert storm.
    bool alert{false};
    // True only when Android exposes a Notification.Action with RemoteInput.
    bool replyable{false};
    bool valid{false};
};

struct CompanionMediaState {
    std::array<char, 49> title{};
    std::array<char, 49> artist{};
    bool playing{false};
    bool available{false};
};

struct CompanionReplyResult {
    std::uint32_t notification_id{0};
    std::uint32_t request_nonce{0};
    std::uint8_t status{0};
};

enum class CompanionMessageKind : std::uint8_t {
    invalid = 0, notification_upsert, notification_remove, notification_clear,
    media_state, reply_result, wifi_provision, wifi_clear, weather_settings, weather_snapshot,
};

struct CompanionWifiProvisioning {
    std::array<char, 33> ssid{};
    std::array<char, 65> password{};
    std::uint8_t ssid_length{0};
    std::uint8_t password_length{0};
};

struct CompanionWeatherSettings {
    bool enabled{false};
    bool location_configured{false};
    bool metric{false};
    std::uint16_t refresh_minutes{30};
    std::int32_t latitude_e6{0};
    std::int32_t longitude_e6{0};
};

struct CompanionWeatherSnapshot {
    std::uint32_t observed_epoch_seconds{0};
    std::uint16_t age_seconds{0};
    std::int16_t temperature_tenths{0};
    std::int16_t apparent_temperature_tenths{0};
    std::uint16_t weather_code{0};
    std::uint16_t wind_tenths{0};
    bool metric{false};
    bool is_day{false};
};

struct CompanionMessage {
    CompanionMessageKind kind{CompanionMessageKind::invalid};
    CompanionNotification notification{};
    std::uint32_t notification_id{0};
    CompanionWifiProvisioning wifi{};
    CompanionWeatherSettings weather{};
    CompanionWeatherSnapshot weather_snapshot{};
    CompanionMediaState media{};
    CompanionReplyResult reply_result{};
};

struct EncodedReply {
    std::array<std::uint8_t, 108> bytes{};
    std::size_t size{0};
};

[[nodiscard]] bool parse_companion_message(std::span<const std::uint8_t> frame,
                                           CompanionMessage &message) noexcept;
[[nodiscard]] std::array<std::uint8_t, 4> encode_media_command(MediaCommand command,
                                                               std::uint8_t sequence) noexcept;
[[nodiscard]] std::array<std::uint8_t, 7> encode_notification_action(
    bool dismiss, std::uint32_t notification_id, std::uint8_t sequence) noexcept;
[[nodiscard]] EncodedReply encode_notification_reply(
    std::uint32_t notification_id, std::uint8_t sequence,
    std::uint32_t request_nonce, std::string_view reply) noexcept;

}  // namespace nightglass::services

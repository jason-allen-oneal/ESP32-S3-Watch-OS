#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nightglass::services {

constexpr std::uint8_t kCompanionProtocolVersion = 1;
constexpr std::size_t kNotificationCapacity = 6;
constexpr std::size_t kAgendaCapacity = 3;

enum class NotificationCategory : std::uint8_t {
    other = 0, message = 1, call = 2, email = 3, calendar = 4, social = 5,
};

enum class MediaCommand : std::uint8_t {
    play_pause = 1, next = 2, previous = 3, volume_up = 4, volume_down = 5,
    seek_backward = 6, seek_forward = 7, stop = 8, restart = 9,
};

enum class CallCommand : std::uint8_t {
    answer = 1,
    reject = 2,
    mute = 3,
    unmute = 4,
};
enum class PhoneCommand : std::uint8_t {
    ring_start = 1,
    ring_stop = 2,
    camera = 3,
    launch_spotify = 4,
    launch_discord = 5,
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
    bool seekable{false};
    std::uint32_t position_ms{0};
    std::uint32_t duration_ms{0};
};

struct CompanionAgendaEvent {
    std::uint32_t start_epoch_seconds{0};
    std::uint32_t end_epoch_seconds{0};
    std::array<char, 33> title{};
    std::array<char, 16> location{};
    bool all_day{false};
    bool valid{false};
};

struct CompanionAgenda {
    std::uint8_t count{0};
    std::array<CompanionAgendaEvent, kAgendaCapacity> events{};
};

struct CompanionPhoneBattery {
    std::uint8_t percent{0};
    bool charging{false};
    bool power_save{false};
    bool valid{false};
};

struct CompanionCallState {
    std::array<char, 49> label{};
    bool ringing{false};
    bool active{false};
    bool muted{false};
    bool can_answer{false};
    bool can_reject{false};
    // Random for each transition from idle into a call. Commands must echo
    // both values so delayed/replayed commands cannot affect a later call.
    std::uint32_t session_id{0};
    std::uint16_t generation{0};
};

struct CompanionPeerIdentity {
    std::uint8_t address_type{0xff};
    std::array<std::uint8_t, 6> address{};
};

struct CompanionReplyResult {
    std::uint32_t notification_id{0};
    std::uint32_t request_nonce{0};
    std::uint8_t status{0};
};

enum class CompanionMessageKind : std::uint8_t {
    invalid = 0, notification_upsert, notification_remove, notification_clear,
    media_state, agenda, phone_battery, call_state, reply_result,
    wifi_provision, wifi_clear, weather_settings, weather_snapshot, peer_forget,
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
    CompanionAgenda agenda{};
    CompanionPhoneBattery phone_battery{};
    CompanionCallState call{};
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
[[nodiscard]] std::array<std::uint8_t, 11> encode_call_command(
    CallCommand command, std::uint16_t sequence, std::uint32_t session_id,
    std::uint16_t generation) noexcept;
[[nodiscard]] std::array<std::uint8_t, 4> encode_phone_command(PhoneCommand command,
                                                               std::uint8_t sequence) noexcept;
[[nodiscard]] std::array<std::uint8_t, 7> encode_notification_action(
    bool dismiss, std::uint32_t notification_id, std::uint8_t sequence) noexcept;
[[nodiscard]] EncodedReply encode_notification_reply(
    std::uint32_t notification_id, std::uint8_t sequence,
    std::uint32_t request_nonce, std::string_view reply) noexcept;
[[nodiscard]] bool valid_peer_identity(const CompanionPeerIdentity &identity) noexcept;
[[nodiscard]] bool peer_identity_matches(const CompanionPeerIdentity &expected,
                                         const CompanionPeerIdentity &candidate) noexcept;
[[nodiscard]] bool repeat_pairing_reset_allowed(
    const CompanionPeerIdentity &pinned_peer, bool authenticated_reset_armed) noexcept;
[[nodiscard]] bool authorization_matches(std::uint16_t expected_connection_handle,
                                         std::uint32_t expected_generation,
                                         const CompanionPeerIdentity &expected_peer,
                                         std::uint16_t candidate_connection_handle,
                                         std::uint32_t candidate_generation,
                                         const CompanionPeerIdentity &candidate_peer) noexcept;

}  // namespace nightglass::services

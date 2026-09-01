#include "nightglass/services/connectivity_protocol.hpp"

#include <algorithm>

namespace nightglass::services {
namespace {
constexpr std::uint8_t kUpsert = 0x01;
constexpr std::uint8_t kRemove = 0x02;
constexpr std::uint8_t kClear = 0x03;
constexpr std::uint8_t kMediaState = 0x04;
constexpr std::uint8_t kAgenda = 0x05;
constexpr std::uint8_t kPhoneBattery = 0x06;
constexpr std::uint8_t kMediaProgress = 0x07;
constexpr std::uint8_t kCallState = 0x08;
constexpr std::uint8_t kMedia = 0x10;
constexpr std::uint8_t kDismiss = 0x11;
constexpr std::uint8_t kRead = 0x12;
constexpr std::uint8_t kReply = 0x13;
constexpr std::uint8_t kCall = 0x14;
constexpr std::uint8_t kPhone = 0x15;
constexpr std::uint8_t kWifiProvision = 0x20;
constexpr std::uint8_t kWeatherSettings = 0x21;
constexpr std::uint8_t kWifiClear = 0x22;
constexpr std::uint8_t kWeatherSnapshot = 0x23;
constexpr std::uint8_t kReplyResult = 0x24;
constexpr std::uint8_t kPeerForget = 0x25;

std::uint32_t read_u32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::int32_t read_i32(const std::uint8_t *data) {
    return static_cast<std::int32_t>(read_u32(data));
}

std::uint16_t read_u16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::int16_t read_i16(const std::uint8_t *data) {
    return static_cast<std::int16_t>(read_u16(data));
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

void write_u16(std::uint8_t *destination, std::uint16_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
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
    if (frame[1] == kMediaState) {
        if (frame.size() < 5 || (frame[2] & ~std::uint8_t{0x03}) != 0) return false;
        const auto title_length = static_cast<std::size_t>(frame[3]);
        const auto artist_length = static_cast<std::size_t>(frame[4]);
        if (title_length > 48 || artist_length > 48 ||
            frame.size() != 5U + title_length + artist_length) return false;
        message.media.playing = (frame[2] & 0x01U) != 0;
        message.media.available = (frame[2] & 0x02U) != 0;
        if (!message.media.available &&
            (message.media.playing || title_length != 0 || artist_length != 0)) return false;
        copy_ascii(message.media.title, frame.data() + 5, title_length);
        copy_ascii(message.media.artist, frame.data() + 5 + title_length, artist_length);
        message.kind = CompanionMessageKind::media_state;
        return true;
    }
    if (frame[1] == kMediaProgress) {
        if (frame.size() < 13 || (frame[2] & ~std::uint8_t{0x07}) != 0) return false;
        const auto title_length = static_cast<std::size_t>(frame[3]);
        const auto artist_length = static_cast<std::size_t>(frame[4]);
        if (title_length > 48 || artist_length > 48 ||
            frame.size() != 13U + title_length + artist_length) return false;
        message.media.playing = (frame[2] & 0x01U) != 0;
        message.media.available = (frame[2] & 0x02U) != 0;
        message.media.seekable = (frame[2] & 0x04U) != 0;
        message.media.position_ms = read_u32(frame.data() + 5);
        message.media.duration_ms = read_u32(frame.data() + 9);
        if ((!message.media.available &&
             (message.media.playing || message.media.seekable || message.media.position_ms != 0 ||
              message.media.duration_ms != 0 || title_length != 0 || artist_length != 0)) ||
            message.media.duration_ms > 604'800'000U ||
            (message.media.duration_ms != 0 &&
             message.media.position_ms > message.media.duration_ms)) return false;
        copy_ascii(message.media.title, frame.data() + 13, title_length);
        copy_ascii(message.media.artist, frame.data() + 13 + title_length, artist_length);
        message.kind = CompanionMessageKind::media_state;
        return true;
    }
    if (frame[1] == kAgenda) {
        if (frame.size() < 3 || frame[2] > kAgendaCapacity) return false;
        std::size_t offset = 3;
        message.agenda.count = frame[2];
        for (std::size_t index = 0; index < message.agenda.count; ++index) {
            if (frame.size() - offset < 11) return false;
            auto &event = message.agenda.events[index];
            event.start_epoch_seconds = read_u32(frame.data() + offset);
            event.end_epoch_seconds = read_u32(frame.data() + offset + 4);
            const auto flags = frame[offset + 8];
            const auto title_length = static_cast<std::size_t>(frame[offset + 9]);
            const auto location_length = static_cast<std::size_t>(frame[offset + 10]);
            if ((flags & ~std::uint8_t{0x01}) != 0 || title_length > 32 ||
                location_length > 15 ||
                frame.size() - offset < 11 + title_length + location_length ||
                event.start_epoch_seconds < 1'577'836'800U ||
                event.end_epoch_seconds < event.start_epoch_seconds) return false;
            event.all_day = (flags & 0x01U) != 0;
            copy_ascii(event.title, frame.data() + offset + 11, title_length);
            copy_ascii(event.location, frame.data() + offset + 11 + title_length,
                       location_length);
            event.valid = true;
            offset += 11 + title_length + location_length;
        }
        if (offset != frame.size()) return false;
        message.kind = CompanionMessageKind::agenda;
        return true;
    }
    if (frame[1] == kPhoneBattery) {
        if (frame.size() != 5 || frame[2] > 100 ||
            (frame[3] & ~std::uint8_t{0x03}) != 0 || frame[4] != 0) return false;
        message.phone_battery.percent = frame[2];
        message.phone_battery.charging = (frame[3] & 0x01U) != 0;
        message.phone_battery.power_save = (frame[3] & 0x02U) != 0;
        message.phone_battery.valid = true;
        message.kind = CompanionMessageKind::phone_battery;
        return true;
    }
    if (frame[1] == kCallState) {
        if (frame.size() < 10 || (frame[2] & ~std::uint8_t{0x1f}) != 0) return false;
        const auto label_length = static_cast<std::size_t>(frame[3]);
        if (label_length > 48 || frame.size() != 10U + label_length) return false;
        message.call.ringing = (frame[2] & 0x01U) != 0;
        message.call.active = (frame[2] & 0x02U) != 0;
        message.call.muted = (frame[2] & 0x04U) != 0;
        message.call.can_answer = (frame[2] & 0x08U) != 0;
        message.call.can_reject = (frame[2] & 0x10U) != 0;
        message.call.session_id = read_u32(frame.data() + 4);
        message.call.generation = read_u16(frame.data() + 8);
        const bool idle = !message.call.ringing && !message.call.active;
        if ((message.call.ringing && message.call.active) ||
            (message.call.can_answer && !message.call.ringing) ||
            (message.call.can_reject && idle)) return false;
        if ((idle && (message.call.session_id != 0 || message.call.generation != 0)) ||
            (!idle && (message.call.session_id == 0 || message.call.generation == 0))) {
            return false;
        }
        copy_ascii(message.call.label, frame.data() + 10, label_length);
        message.kind = CompanionMessageKind::call_state;
        return true;
    }
    if (frame[1] == kReplyResult) {
        if (frame.size() != 12 || frame[2] == 0 || frame[3] > 5) return false;
        message.reply_result.status = frame[3];
        message.reply_result.notification_id = read_u32(frame.data() + 4);
        message.reply_result.request_nonce = read_u32(frame.data() + 8);
        if (message.reply_result.notification_id == 0 ||
            message.reply_result.request_nonce == 0) return false;
        message.kind = CompanionMessageKind::reply_result;
        return true;
    }
    if (frame[1] == kWifiClear) {
        if (frame.size() != 2) return false;
        message.kind = CompanionMessageKind::wifi_clear;
        return true;
    }
    if (frame[1] == kPeerForget) {
        if (frame.size() != 2) return false;
        message.kind = CompanionMessageKind::peer_forget;
        return true;
    }
    if (frame[1] == kWifiProvision) {
        if (frame.size() < 4) return false;
        const auto ssid_length = frame[2];
        const auto password_length = frame[3];
        if (ssid_length == 0 || ssid_length > 32 || password_length > 64 ||
            frame.size() != 4U + ssid_length + password_length) return false;
        copy_ascii(message.wifi.ssid, frame.data() + 4, ssid_length);
        copy_ascii(message.wifi.password, frame.data() + 4 + ssid_length, password_length);
        message.wifi.ssid_length = ssid_length;
        message.wifi.password_length = password_length;
        message.kind = CompanionMessageKind::wifi_provision;
        return true;
    }
    if (frame[1] == kWeatherSettings) {
        if (frame.size() != 14) return false;
        const std::uint8_t flags = frame[2];
        if ((flags & ~std::uint8_t{0x03}) != 0 || frame[3] > 1) return false;
        message.weather.enabled = (flags & 0x01) != 0;
        message.weather.location_configured = (flags & 0x02) != 0;
        message.weather.metric = frame[3] != 0;
        message.weather.refresh_minutes = static_cast<std::uint16_t>(frame[4]) |
                                          (static_cast<std::uint16_t>(frame[5]) << 8U);
        message.weather.latitude_e6 = read_i32(frame.data() + 6);
        message.weather.longitude_e6 = read_i32(frame.data() + 10);
        message.kind = CompanionMessageKind::weather_settings;
        return message.weather.refresh_minutes >= 15 &&
               message.weather.refresh_minutes <= 360 &&
               message.weather.latitude_e6 >= -90'000'000 &&
               message.weather.latitude_e6 <= 90'000'000 &&
               message.weather.longitude_e6 >= -180'000'000 &&
               message.weather.longitude_e6 <= 180'000'000 &&
               (!message.weather.enabled || message.weather.location_configured);
    }
    if (frame[1] == kWeatherSnapshot) {
        if (frame.size() != 18) return false;
        const std::uint8_t flags = frame[2];
        if ((flags & ~std::uint8_t{0x03}) != 0 || frame[3] != 0) return false;
        auto &weather = message.weather_snapshot;
        weather.metric = (flags & 0x01) != 0;
        weather.is_day = (flags & 0x02) != 0;
        weather.observed_epoch_seconds = read_u32(frame.data() + 4);
        weather.age_seconds = read_u16(frame.data() + 8);
        weather.temperature_tenths = read_i16(frame.data() + 10);
        weather.apparent_temperature_tenths = read_i16(frame.data() + 12);
        weather.weather_code = read_u16(frame.data() + 14);
        weather.wind_tenths = read_u16(frame.data() + 16);
        if (weather.observed_epoch_seconds < 1'577'836'800U ||
            weather.temperature_tenths < -1500 || weather.temperature_tenths > 1500 ||
            weather.apparent_temperature_tenths < -1500 ||
            weather.apparent_temperature_tenths > 1500 ||
            weather.age_seconds > 21'600 || weather.weather_code > 999 ||
            weather.wind_tenths > 5000 ||
            weather.observed_epoch_seconds < weather.age_seconds) {
            return false;
        }
        message.kind = CompanionMessageKind::weather_snapshot;
        return true;
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
    const auto raw_category = frame[6];
    const auto category = static_cast<std::uint8_t>(raw_category & 0x3fU);
    if (category > static_cast<std::uint8_t>(NotificationCategory::social)) return false;
    notification.alert = (raw_category & 0x80U) != 0;
    notification.replyable = (raw_category & 0x40U) != 0;
    notification.category = static_cast<NotificationCategory>(category);
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

EncodedReply encode_notification_reply(std::uint32_t notification_id,
                                       std::uint8_t sequence,
                                       std::uint32_t request_nonce,
                                       std::string_view reply) noexcept {
    EncodedReply frame{};
    if (notification_id == 0 || request_nonce == 0 || reply.empty()) return frame;
    const auto length = std::min<std::size_t>(reply.size(), 96);
    frame.bytes[0] = kCompanionProtocolVersion;
    frame.bytes[1] = kReply;
    frame.bytes[2] = sequence;
    write_u32(frame.bytes.data() + 3, notification_id);
    write_u32(frame.bytes.data() + 7, request_nonce);
    frame.bytes[11] = static_cast<std::uint8_t>(length);
    for (std::size_t index = 0; index < length; ++index) {
        const auto value = static_cast<unsigned char>(reply[index]);
        frame.bytes[12 + index] = value >= 0x20 && value <= 0x7e ? value : '?';
    }
    frame.size = 12 + length;
    return frame;
}

std::array<std::uint8_t, 4> encode_media_command(MediaCommand command,
                                                  std::uint8_t sequence) noexcept {
    return {kCompanionProtocolVersion, kMedia, sequence, static_cast<std::uint8_t>(command)};
}

std::array<std::uint8_t, 11> encode_call_command(CallCommand command,
                                                 std::uint16_t sequence,
                                                 std::uint32_t session_id,
                                                 std::uint16_t generation) noexcept {
    std::array<std::uint8_t, 11> frame{
        kCompanionProtocolVersion, kCall, 0, 0, static_cast<std::uint8_t>(command),
        0, 0, 0, 0, 0, 0};
    if (sequence == 0 || session_id == 0 || generation == 0 ||
        static_cast<std::uint8_t>(command) < static_cast<std::uint8_t>(CallCommand::answer) ||
        static_cast<std::uint8_t>(command) > static_cast<std::uint8_t>(CallCommand::unmute)) {
        return {};
    }
    write_u16(frame.data() + 2, sequence);
    frame[4] = static_cast<std::uint8_t>(command);
    write_u32(frame.data() + 5, session_id);
    write_u16(frame.data() + 9, generation);
    return frame;
}

bool valid_peer_identity(const CompanionPeerIdentity &identity) noexcept {
    if (identity.address_type > 3) return false;
    return std::any_of(identity.address.begin(), identity.address.end(),
                       [](std::uint8_t value) { return value != 0; });
}

bool peer_identity_matches(const CompanionPeerIdentity &expected,
                           const CompanionPeerIdentity &candidate) noexcept {
    return valid_peer_identity(expected) && valid_peer_identity(candidate) &&
           expected.address_type == candidate.address_type &&
           expected.address == candidate.address;
}

bool authorization_matches(std::uint16_t expected_connection_handle,
                           std::uint32_t expected_generation,
                           const CompanionPeerIdentity &expected_peer,
                           std::uint16_t candidate_connection_handle,
                           std::uint32_t candidate_generation,
                           const CompanionPeerIdentity &candidate_peer) noexcept {
    return expected_connection_handle == candidate_connection_handle &&
           expected_generation != 0 && expected_generation == candidate_generation &&
           peer_identity_matches(expected_peer, candidate_peer);
}

std::array<std::uint8_t, 4> encode_phone_command(PhoneCommand command,
                                                 std::uint8_t sequence) noexcept {
    return {kCompanionProtocolVersion, kPhone, sequence, static_cast<std::uint8_t>(command)};
}

std::array<std::uint8_t, 7> encode_notification_action(
    bool dismiss, std::uint32_t notification_id, std::uint8_t sequence) noexcept {
    std::array<std::uint8_t, 7> frame{kCompanionProtocolVersion, dismiss ? kDismiss : kRead,
                                      sequence, 0, 0, 0, 0};
    write_u32(frame.data() + 3, notification_id);
    return frame;
}
}  // namespace nightglass::services

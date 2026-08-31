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
constexpr std::uint8_t kWifiProvision = 0x20;
constexpr std::uint8_t kWeatherSettings = 0x21;
constexpr std::uint8_t kWifiClear = 0x22;
constexpr std::uint8_t kWeatherSnapshot = 0x23;

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
    if (frame[1] == kWifiClear) {
        if (frame.size() != 2) return false;
        message.kind = CompanionMessageKind::wifi_clear;
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
               message.weather.refresh_minutes <= 360;
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

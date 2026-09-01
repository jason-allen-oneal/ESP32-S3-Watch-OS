#include "nightglass/services/connectivity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nightglass/core/health.hpp"
#include "nightglass/services/audio.hpp"
#include "nightglass/services/network_weather.hpp"
#include "nightglass/services/power.hpp"
#include "nightglass/services/update_transport.hpp"
#include "nightglass/services/voice.hpp"
#include "nightglass/services/voice_protocol.hpp"

extern "C" void ble_store_config_init(void);

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_ble";
constexpr char kNamespace[] = "ng_ble";
constexpr char kEnabledKey[] = "enabled";
constexpr char kNameKey[] = "name";
constexpr char kPeerIdentityKey[] = "peer_id";
constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr std::size_t kMaxInboundFrame = kUpdateTransportFrameMaximum;

// UUIDs are stored least-significant byte first by NimBLE.
static const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x68, 0x67, 0x69, 0x6e, 0x2d, 0x77, 0x6f, 0x72,
    0x72, 0x4f, 0x6f, 0x6b, 0x01, 0x40, 0x3b, 0x7a);
static const ble_uuid128_t kStatusUuid = BLE_UUID128_INIT(
    0x68, 0x67, 0x69, 0x6e, 0x2d, 0x77, 0x6f, 0x72,
    0x72, 0x4f, 0x6f, 0x6b, 0x02, 0x40, 0x3b, 0x7a);
static const ble_uuid128_t kInboundUuid = BLE_UUID128_INIT(
    0x68, 0x67, 0x69, 0x6e, 0x2d, 0x77, 0x6f, 0x72,
    0x72, 0x4f, 0x6f, 0x6b, 0x03, 0x40, 0x3b, 0x7a);
static const ble_uuid128_t kOutboundUuid = BLE_UUID128_INIT(
    0x68, 0x67, 0x69, 0x6e, 0x2d, 0x77, 0x6f, 0x72,
    0x72, 0x4f, 0x6f, 0x6b, 0x04, 0x40, 0x3b, 0x7a);

ConnectivityService instance;
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE peer_lock = portMUX_INITIALIZER_UNLOCKED;
ConnectivitySnapshot current{};
std::atomic<std::uint16_t> connection_handle{kNoConnection};
std::uint16_t status_handle{};
std::uint16_t outbound_handle{};
std::atomic_bool outbound_subscribed{false};
std::uint8_t own_address_type{};
std::atomic<std::uint8_t> outbound_sequence{0};
std::atomic<std::uint16_t> outbound_call_sequence{0};
bool host_started{};
std::int64_t reply_started_us{};
CompanionPeerIdentity pinned_peer{};
bool pairing_confirmation_seen{};

struct AuthorizationToken {
    std::uint16_t connection_handle{kNoConnection};
    std::uint32_t generation{0};
    CompanionPeerIdentity peer{};
};

AuthorizationToken active_authorization{};
std::uint32_t authorization_generation{1};
CompanionPeerIdentity one_shot_repair_peer{};
bool one_shot_repair_armed{};

void secure_wipe(void *memory, std::size_t length) {
    auto *bytes = static_cast<volatile std::uint8_t *>(memory);
    while (length--) *bytes++ = 0;
}

bool enabled() {
    portENTER_CRITICAL(&state_lock);
    const bool value = current.settings.enabled;
    portEXIT_CRITICAL(&state_lock);
    return value;
}

void set_detail_locked(const char *detail) {
    std::snprintf(current.detail.data(), current.detail.size(), "%s", detail);
}

ConnectivitySettings load_settings() {
    ConnectivitySettings settings{};
    nvs_handle_t handle{};
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return settings;
    std::uint8_t enabled = settings.enabled ? 1 : 0;
    if (nvs_get_u8(handle, kEnabledKey, &enabled) == ESP_OK) settings.enabled = enabled != 0;
    std::size_t length = settings.device_name.size();
    if (nvs_get_str(handle, kNameKey, settings.device_name.data(), &length) != ESP_OK) {
        settings.device_name = {"Nightglass"};
    }
    nvs_close(handle);
    return settings;
}

bool valid_settings(const ConnectivitySettings &settings) {
    const auto length = strnlen(settings.device_name.data(), settings.device_name.size());
    if (length == 0 || length >= settings.device_name.size()) return false;
    return std::all_of(settings.device_name.begin(), settings.device_name.begin() + length,
                       [](char value) { return value >= 0x20 && value <= 0x7e; });
}

esp_err_t save_settings(const ConnectivitySettings &settings) {
    nvs_handle_t handle{};
    auto result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, kEnabledKey, settings.enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_str(handle, kNameKey, settings.device_name.data());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

CompanionPeerIdentity load_pinned_peer() {
    CompanionPeerIdentity identity{};
    std::array<std::uint8_t, 7> encoded{};
    nvs_handle_t handle{};
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return identity;
    std::size_t length = encoded.size();
    const auto result = nvs_get_blob(handle, kPeerIdentityKey, encoded.data(), &length);
    nvs_close(handle);
    if (result != ESP_OK || length != encoded.size()) return identity;
    identity.address_type = encoded[0];
    std::copy_n(encoded.begin() + 1, identity.address.size(), identity.address.begin());
    return valid_peer_identity(identity) ? identity : CompanionPeerIdentity{};
}

esp_err_t save_pinned_peer(const CompanionPeerIdentity &identity) {
    if (!valid_peer_identity(identity)) return ESP_ERR_INVALID_ARG;
    std::array<std::uint8_t, 7> encoded{};
    encoded[0] = identity.address_type;
    std::copy(identity.address.begin(), identity.address.end(), encoded.begin() + 1);
    nvs_handle_t handle{};
    auto result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, kPeerIdentityKey, encoded.data(), encoded.size());
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    secure_wipe(encoded.data(), encoded.size());
    return result;
}

CompanionPeerIdentity peer_identity(const ble_addr_t &address) {
    CompanionPeerIdentity identity{};
    identity.address_type = address.type;
    std::copy_n(address.val, identity.address.size(), identity.address.begin());
    return identity;
}

CompanionPeerIdentity pinned_peer_snapshot() {
    portENTER_CRITICAL(&peer_lock);
    const auto identity = pinned_peer;
    portEXIT_CRITICAL(&peer_lock);
    return identity;
}

void set_pinned_peer(const CompanionPeerIdentity &identity) {
    portENTER_CRITICAL(&peer_lock);
    pinned_peer = identity;
    portEXIT_CRITICAL(&peer_lock);
}

std::uint32_t next_authorization_generation_locked() {
    if (++authorization_generation == 0) ++authorization_generation;
    return authorization_generation;
}

void invalidate_authorization() {
    portENTER_CRITICAL(&peer_lock);
    active_authorization = {};
    (void)next_authorization_generation_locked();
    portEXIT_CRITICAL(&peer_lock);
}

void establish_authorization(std::uint16_t handle,
                             const CompanionPeerIdentity &identity) {
    portENTER_CRITICAL(&peer_lock);
    active_authorization.connection_handle = handle;
    active_authorization.generation = next_authorization_generation_locked();
    active_authorization.peer = identity;
    portEXIT_CRITICAL(&peer_lock);
}

bool resolve_authorization(std::uint16_t handle, AuthorizationToken &token) {
    ble_gap_conn_desc descriptor{};
    if (handle == kNoConnection || ble_gap_conn_find(handle, &descriptor) != 0 ||
        !descriptor.sec_state.encrypted || !descriptor.sec_state.authenticated ||
        !descriptor.sec_state.bonded) {
        return false;
    }
    const auto candidate = peer_identity(descriptor.peer_id_addr);
    portENTER_CRITICAL(&peer_lock);
    const auto grant = active_authorization;
    const auto expected = pinned_peer;
    const bool authorized = valid_peer_identity(expected) &&
                            peer_identity_matches(expected, candidate) &&
                            authorization_matches(grant.connection_handle, grant.generation,
                                                  grant.peer, handle, grant.generation,
                                                  candidate);
    if (authorized) token = grant;
    portEXIT_CRITICAL(&peer_lock);
    return authorized;
}

bool authorization_still_valid(const AuthorizationToken &token) {
    ble_gap_conn_desc descriptor{};
    if (token.connection_handle == kNoConnection || token.generation == 0 ||
        ble_gap_conn_find(token.connection_handle, &descriptor) != 0 ||
        !descriptor.sec_state.encrypted || !descriptor.sec_state.authenticated ||
        !descriptor.sec_state.bonded) {
        return false;
    }
    const auto candidate = peer_identity(descriptor.peer_id_addr);
    portENTER_CRITICAL(&peer_lock);
    const auto grant = active_authorization;
    const auto expected = pinned_peer;
    const bool authorized = valid_peer_identity(expected) &&
                            peer_identity_matches(expected, candidate) &&
                            authorization_matches(grant.connection_handle, grant.generation,
                                                  grant.peer, token.connection_handle,
                                                  token.generation, token.peer) &&
                            peer_identity_matches(token.peer, candidate);
    portEXIT_CRITICAL(&peer_lock);
    return authorized;
}

bool consume_one_shot_repair(const CompanionPeerIdentity &candidate) {
    portENTER_CRITICAL(&peer_lock);
    const bool allowed = one_shot_repair_armed &&
                         peer_identity_matches(one_shot_repair_peer, candidate);
    if (allowed) {
        one_shot_repair_armed = false;
        one_shot_repair_peer = {};
    }
    portEXIT_CRITICAL(&peer_lock);
    return allowed;
}

nightglass::core::Status clear_pinned_peer_for_repair(
    const AuthorizationToken &authorization) {
    // GATT and GAP callbacks run on the NimBLE host task. Rechecking here,
    // immediately before the NVS side effect, prevents a stale parsed frame
    // from clearing the allowlist after its connection authorization changed.
    if (!authorization_still_valid(authorization)) {
        return {nightglass::core::StatusCode::invalid_state,
                "Companion authorization changed before reset"};
    }
    nvs_handle_t nvs_handle{};
    auto result = nvs_open(kNamespace, NVS_READWRITE, &nvs_handle);
    if (result == ESP_OK) {
        result = nvs_erase_key(nvs_handle, kPeerIdentityKey);
        if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
        if (result == ESP_OK) result = nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    if (result != ESP_OK) {
        return {nightglass::core::StatusCode::io_error,
                "Pinned companion was not cleared"};
    }

    portENTER_CRITICAL(&peer_lock);
    pinned_peer = {};
    one_shot_repair_peer = authorization.peer;
    one_shot_repair_armed = true;
    active_authorization = {};
    (void)next_authorization_generation_locked();
    portEXIT_CRITICAL(&peer_lock);

    portENTER_CRITICAL(&state_lock);
    current.peer_identity_pinned = false;
    current.encrypted = false;
    current.bonded = false;
    current.state = CompanionLinkState::connected_unsecured;
    set_detail_locked("Authorization cleared; re-pair once");
    ++current.sequence;
    portEXIT_CRITICAL(&state_lock);
    return nightglass::core::Status::Ok();
}

bool notification_details_redacted_locked() {
    return current.notification_privacy == NotificationPrivacyPolicy::always_redact ||
           (current.notification_privacy == NotificationPrivacyPolicy::redact_when_locked &&
            !current.notification_details_unlocked);
}

void redact_notification(CompanionNotification &notification) {
    notification.app = {"Notification"};
    notification.title = {"Content hidden"};
    notification.body = {};
    notification.replyable = false;
}

std::uint8_t next_sequence8() {
    std::uint8_t value{};
    do {
        value = outbound_sequence.fetch_add(1) + 1;
    } while (value == 0);
    return value;
}

std::uint16_t next_sequence16() {
    std::uint16_t value{};
    do {
        value = outbound_call_sequence.fetch_add(1) + 1;
    } while (value == 0);
    return value;
}

void remove_notification_locked(std::uint32_t id) {
    for (auto &notification : current.notifications) {
        if (notification.valid && notification.id == id) notification = {};
    }
    current.notification_count = static_cast<std::uint8_t>(std::count_if(
        current.notifications.begin(), current.notifications.end(),
        [](const auto &notification) { return notification.valid; }));
}

SoundCue notification_cue(NotificationCategory category) {
    switch (category) {
        case NotificationCategory::message: return SoundCue::notification;
        case NotificationCategory::call: return SoundCue::call;
        case NotificationCategory::email: return SoundCue::email;
        case NotificationCategory::calendar: return SoundCue::calendar;
        case NotificationCategory::social:
        case NotificationCategory::other:
            return SoundCue::notification;
    }
    return SoundCue::notification;
}

bool apply_message(const CompanionMessage &message,
                   const AuthorizationToken &authorization) {
    if (!authorization_still_valid(authorization)) return false;
    if (message.kind == CompanionMessageKind::media_state) {
        portENTER_CRITICAL(&state_lock);
        current.media = message.media;
        ++current.sequence;
        portEXIT_CRITICAL(&state_lock);
        return true;
    }
    if (message.kind == CompanionMessageKind::agenda) {
        portENTER_CRITICAL(&state_lock);
        current.agenda = message.agenda;
        ++current.sequence;
        portEXIT_CRITICAL(&state_lock);
        return true;
    }
    if (message.kind == CompanionMessageKind::phone_battery) {
        portENTER_CRITICAL(&state_lock);
        current.phone_battery = message.phone_battery;
        ++current.sequence;
        portEXIT_CRITICAL(&state_lock);
        return true;
    }
    if (message.kind == CompanionMessageKind::call_state) {
        portENTER_CRITICAL(&state_lock);
        current.call = message.call;
        ++current.sequence;
        portEXIT_CRITICAL(&state_lock);
        return true;
    }
    if (message.kind == CompanionMessageKind::reply_result) {
        portENTER_CRITICAL(&state_lock);
        const bool matches = current.reply_pending &&
                             current.reply_notification_id ==
                                 message.reply_result.notification_id &&
                             current.reply_nonce == message.reply_result.request_nonce;
        if (matches) {
            current.reply_status = message.reply_result.status;
            current.reply_pending = false;
            reply_started_us = 0;
            ++current.sequence;
            ++current.notification_sequence;
        }
        portEXIT_CRITICAL(&state_lock);
        return true;
    }
    if (message.kind == CompanionMessageKind::wifi_provision) {
        return network_weather_service().provision_credentials(
            message.wifi.ssid.data(), message.wifi.ssid_length,
            message.wifi.password.data(), message.wifi.password_length).is_ok();
    }
    if (message.kind == CompanionMessageKind::wifi_clear) {
        return network_weather_service().clear_credentials().is_ok();
    }
    if (message.kind == CompanionMessageKind::peer_forget) {
        return clear_pinned_peer_for_repair(authorization).is_ok();
    }
    if (message.kind == CompanionMessageKind::weather_settings) {
        NetworkWeatherSettings settings{};
        settings.enabled = message.weather.enabled;
        settings.location_configured = message.weather.location_configured;
        settings.latitude_e6 = message.weather.latitude_e6;
        settings.longitude_e6 = message.weather.longitude_e6;
        settings.units = message.weather.metric ? WeatherUnits::metric : WeatherUnits::imperial;
        settings.refresh_minutes = message.weather.refresh_minutes;
        return network_weather_service().update_settings(settings).is_ok();
    }
    if (message.kind == CompanionMessageKind::weather_snapshot) {
        DecodedWeather weather{};
        weather.temperature = message.weather_snapshot.temperature_tenths / 10.0F;
        weather.apparent_temperature =
            message.weather_snapshot.apparent_temperature_tenths / 10.0F;
        weather.weather_code = message.weather_snapshot.weather_code;
        weather.wind_speed = message.weather_snapshot.wind_tenths / 10.0F;
        weather.is_day = message.weather_snapshot.is_day;
        return network_weather_service().accept_phone_weather(
            message.weather_snapshot.observed_epoch_seconds,
            message.weather_snapshot.age_seconds,
            message.weather_snapshot.metric ? WeatherUnits::metric
                                            : WeatherUnits::imperial,
            weather).is_ok();
    }
    bool queue_alert = false;
    SoundCue alert_cue = SoundCue::notification;
    portENTER_CRITICAL(&state_lock);
    if (message.kind == CompanionMessageKind::notification_clear) {
        current.notifications = {};
        current.notification_count = 0;
    } else if (message.kind == CompanionMessageKind::notification_remove) {
        remove_notification_locked(message.notification_id);
    } else if (message.kind == CompanionMessageKind::notification_upsert) {
        const bool existing = std::any_of(
            current.notifications.begin(), current.notifications.end(),
            [&](const auto &notification) {
                return notification.valid && notification.id == message.notification_id;
            });
        remove_notification_locked(message.notification_id);
        for (std::size_t index = current.notifications.size() - 1; index > 0; --index) {
            current.notifications[index] = current.notifications[index - 1];
        }
        current.notifications[0] = message.notification;
        if (notification_details_redacted_locked()) redact_notification(current.notifications[0]);
        current.notification_count = static_cast<std::uint8_t>(std::count_if(
            current.notifications.begin(), current.notifications.end(),
            [](const auto &notification) { return notification.valid; }));
        queue_alert = message.notification.alert && !existing;
        alert_cue = notification_cue(message.notification.category);
    }
    if (message.kind == CompanionMessageKind::notification_clear ||
        message.kind == CompanionMessageKind::notification_remove ||
        message.kind == CompanionMessageKind::notification_upsert) {
        ++current.notification_sequence;
    }
    ++current.sequence;
    portEXIT_CRITICAL(&state_lock);
    if (queue_alert) (void)audio_service().request_sound(alert_cue);
    return true;
}

int gatt_access(std::uint16_t conn_handle, std::uint16_t attr_handle,
                ble_gatt_access_ctxt *context,
                void *) {
    AuthorizationToken authorization{};
    if (!resolve_authorization(conn_handle, authorization)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == status_handle) {
        const auto snapshot = instance.snapshot();
        const std::array<std::uint8_t, 6> status{
            kCompanionProtocolVersion, static_cast<std::uint8_t>(snapshot.state),
            snapshot.notification_count, snapshot.encrypted ? std::uint8_t{1} : std::uint8_t{0},
            snapshot.bonded ? std::uint8_t{1} : std::uint8_t{0},
            snapshot.peer_identity_pinned ? std::uint8_t{1} : std::uint8_t{0}};
        return os_mbuf_append(context->om, status.data(), status.size()) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    const auto length = static_cast<std::size_t>(OS_MBUF_PKTLEN(context->om));
    if (length == 0 || length > kMaxInboundFrame) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    std::array<std::uint8_t, kMaxInboundFrame> frame{};
    std::uint16_t copied{};
    if (ble_hs_mbuf_to_flat(context->om, frame.data(), frame.size(), &copied) != 0 ||
        copied != length) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (is_update_transport_frame(std::span(frame.data(), length))) {
        UpdateTransportCommand command{};
        const bool parsed = parse_update_transport_frame(std::span(frame.data(), length),
                                                          command);
        const bool queued = parsed && authorization_still_valid(authorization) &&
                            update_transport().enqueue(command);
        secure_wipe(&command, sizeof(command));
        secure_wipe(frame.data(), frame.size());
        return queued ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    const auto opcode = length >= 2 ? frame[1] : std::uint8_t{};
    if (opcode >= static_cast<std::uint8_t>(VoiceFrameKind::request_ack) &&
        opcode <= static_cast<std::uint8_t>(VoiceFrameKind::health)) {
        VoiceFrame voice_frame{};
        const bool parsed = parse_voice_frame(std::span(frame.data(), length), voice_frame);
        const bool applied = parsed && authorization_still_valid(authorization) &&
                             voice_service().accept_frame(voice_frame);
        secure_wipe(frame.data(), frame.size());
        return applied ? 0 : BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    CompanionMessage message{};
    if (!parse_companion_message(std::span(frame.data(), length), message)) {
        secure_wipe(frame.data(), frame.size());
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    // Parsing is side-effect free. Bind the frame to the same authorized
    // connection generation again immediately before applying it.
    const bool applied = authorization_still_valid(authorization) &&
                         apply_message(message, authorization);
    secure_wipe(&message, sizeof(message));
    secure_wipe(frame.data(), frame.size());
    return applied ? 0 : BLE_ATT_ERR_UNLIKELY;
}

const ble_gatt_chr_def gatt_characteristics[]{
    {.uuid = &kStatusUuid.u,
     .access_cb = gatt_access,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
              BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY |
              BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
     .val_handle = &status_handle},
    {.uuid = &kInboundUuid.u,
     .access_cb = gatt_access,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
              BLE_GATT_CHR_F_WRITE_AUTHEN},
    {.uuid = &kOutboundUuid.u,
     .access_cb = gatt_access,
     .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
     .val_handle = &outbound_handle},
    {}};

const ble_gatt_svc_def gatt_services[]{
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &kServiceUuid.u,
     .characteristics = gatt_characteristics},
    {}};

void advertise();

int gap_event(ble_gap_event *event, void *) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                invalidate_authorization();
                connection_handle = event->connect.conn_handle;
                // Wearable-oriented parameters: 40-60 ms connection interval
                // with peripheral latency 4 gives a bounded ~300 ms worst-case
                // notification delay while allowing the controller to sleep
                // between connection events. Use a 20-second supervision window:
                // the previous 6-second value was brittle when both Samsung and
                // the ESP32 controller entered power-saving states simultaneously.
                // The central may negotiate nearby values; failure is non-fatal.
                const ble_gap_upd_params power_params{
                    .itvl_min = 32,
                    .itvl_max = 48,
                    .latency = 4,
                    .supervision_timeout = 2000,
                    .min_ce_len = 0,
                    .max_ce_len = 0,
                };
                const auto update_result =
                    ble_gap_update_params(event->connect.conn_handle, &power_params);
                if (update_result != 0) {
                    ESP_LOGW(kTag, "BLE low-power connection update rejected: %d",
                             update_result);
                }
                portENTER_CRITICAL(&state_lock);
                current.state = CompanionLinkState::connected_unsecured;
                current.encrypted = false;
                set_detail_locked("Connected; securing link");
                ++current.sequence;
                portEXIT_CRITICAL(&state_lock);
                ble_gap_security_initiate(connection_handle);
            } else {
                advertise();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT: {
            invalidate_authorization();
            update_transport().link_lost();
            voice_service().link_lost();
            connection_handle = kNoConnection;
            outbound_subscribed = false;
            portENTER_CRITICAL(&state_lock);
            const bool should_advertise = current.settings.enabled;
            current.state = should_advertise ? CompanionLinkState::advertising
                                             : CompanionLinkState::disabled;
            current.encrypted = false;
            current.reply_pending = false;
            current.reply_notification_id = 0;
            current.reply_nonce = 0;
            reply_started_us = 0;
            current.pairing_passkey = 0;
            current.pairing_passkey_active = false;
            pairing_confirmation_seen = false;
            set_detail_locked(should_advertise ? "Advertising" : "Bluetooth disabled");
            ++current.sequence;
            ++current.notification_sequence;
            portEXIT_CRITICAL(&state_lock);
            if (should_advertise) advertise();
            return 0;
        }
        case BLE_GAP_EVENT_ENC_CHANGE: {
            ble_gap_conn_desc descriptor{};
            if (ble_gap_conn_find(event->enc_change.conn_handle, &descriptor) == 0) {
                const bool authenticated = descriptor.sec_state.encrypted &&
                                           descriptor.sec_state.authenticated &&
                                           descriptor.sec_state.bonded;
                const auto candidate = peer_identity(descriptor.peer_id_addr);
                const auto expected = pinned_peer_snapshot();
                bool authorized = false;
                if (authenticated && valid_peer_identity(candidate)) {
                    if (valid_peer_identity(expected)) {
                        authorized = peer_identity_matches(expected, candidate);
                    } else if (pairing_confirmation_seen &&
                               save_pinned_peer(candidate) == ESP_OK) {
                        set_pinned_peer(candidate);
                        authorized = true;
                    }
                }
                if (authorized) {
                    establish_authorization(event->enc_change.conn_handle, candidate);
                    update_transport().link_ready();
                } else {
                    invalidate_authorization();
                    update_transport().link_lost();
                }
                const auto pinned_after = pinned_peer_snapshot();
                portENTER_CRITICAL(&state_lock);
                current.encrypted = authorized;
                current.bonded = authorized;
                current.peer_identity_pinned = valid_peer_identity(pinned_after);
                current.state = current.encrypted ? CompanionLinkState::connected_encrypted
                                                  : CompanionLinkState::connected_unsecured;
                set_detail_locked(current.encrypted ? "Pinned companion authenticated"
                                  : authenticated && valid_peer_identity(expected)
                                      ? "Unrecognized bonded phone rejected"
                                      : authenticated
                                          ? "Re-pair to authorize companion"
                                          : "Passkey authentication required");
                current.pairing_passkey = 0;
                current.pairing_passkey_active = false;
                ++current.sequence;
                ++current.notification_sequence;
                portEXIT_CRITICAL(&state_lock);
                pairing_confirmation_seen = false;
                if (!authorized) {
                    (void)ble_gap_terminate(event->enc_change.conn_handle,
                                            BLE_ERR_REM_USER_CONN_TERM);
                }
            }
            return 0;
        }
        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            if (event->passkey.params.action != BLE_SM_IOACT_DISP) return 0;
            ble_sm_io passkey{};
            passkey.action = BLE_SM_IOACT_DISP;
            passkey.passkey = esp_random() % 1'000'000U;
            char detail[64]{};
            std::snprintf(detail, sizeof(detail), "Enter %06lu on phone",
                          static_cast<unsigned long>(passkey.passkey));
            portENTER_CRITICAL(&state_lock);
            set_detail_locked(detail);
            current.pairing_passkey = passkey.passkey;
            current.pairing_passkey_active = true;
            pairing_confirmation_seen = true;
            ++current.sequence;
            portEXIT_CRITICAL(&state_lock);
            power_service().note_activity(nightglass::core::WakeReason::notification);
            return ble_sm_inject_io(event->passkey.conn_handle, &passkey);
        }
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == outbound_handle) {
                outbound_subscribed = event->subscribe.cur_notify != 0;
            }
            return 0;
        case BLE_GAP_EVENT_CONN_UPDATE:
            if (event->conn_update.status != 0) {
                ESP_LOGW(kTag, "BLE connection parameter update failed: %d",
                         event->conn_update.status);
            }
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // Bond keys are never deleted merely because a peer asks to pair
            // again. The sole exception is the same identity after an
            // authenticated 0x25 reset explicitly armed one retry in RAM.
            ble_gap_conn_desc descriptor{};
            const auto find_result =
                ble_gap_conn_find(event->repeat_pairing.conn_handle, &descriptor);
            if (find_result != 0) {
                ESP_LOGE(kTag, "Unable to resolve repeat-pairing peer: %d", find_result);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            const auto candidate = peer_identity(descriptor.peer_id_addr);
            const auto expected = pinned_peer_snapshot();
            const bool authenticated_reset = consume_one_shot_repair(candidate);
            if (!repeat_pairing_reset_allowed(expected, authenticated_reset)) {
                ESP_LOGW(kTag, "Rejected repeat pairing without reset authorization");
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            const auto delete_result = ble_store_util_delete_peer(&descriptor.peer_id_addr);
            if (delete_result != 0) {
                ESP_LOGE(kTag, "Unable to remove stale companion bond: %d", delete_result);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            if (valid_peer_identity(expected)) {
                ESP_LOGI(kTag, "Consumed reset authorization; retrying secure pairing");
            } else {
                ESP_LOGI(kTag, "Removed pre-pinning legacy bond; requiring fresh passkey");
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (enabled()) advertise();
            return 0;
        default:
            return 0;
    }
}

void advertise() {
    const auto snapshot = instance.snapshot();
    if (!snapshot.settings.enabled || ble_gap_adv_active()) return;
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t *>(&kServiceUuid);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    const auto adv_result = ble_gap_adv_set_fields(&fields);
    if (adv_result != 0) {
        ESP_LOGE(kTag, "Unable to configure BLE advertisement: %d", adv_result);
        return;
    }
    ble_hs_adv_fields response{};
    response.name = reinterpret_cast<const std::uint8_t *>(snapshot.settings.device_name.data());
    response.name_len = static_cast<std::uint8_t>(strlen(snapshot.settings.device_name.data()));
    response.name_is_complete = 1;
    const auto response_result = ble_gap_adv_rsp_set_fields(&response);
    if (response_result != 0) {
        ESP_LOGE(kTag, "Unable to configure BLE scan response: %d", response_result);
        return;
    }
    ble_gap_adv_params parameters{};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const auto start_result = ble_gap_adv_start(own_address_type, nullptr, BLE_HS_FOREVER,
                                                &parameters, gap_event, nullptr);
    if (start_result == 0) {
        portENTER_CRITICAL(&state_lock);
        current.state = CompanionLinkState::advertising;
        set_detail_locked("Advertising");
        ++current.sequence;
        portEXIT_CRITICAL(&state_lock);
    } else {
        ESP_LOGE(kTag, "Unable to start BLE advertisement: %d", start_result);
    }
}

void on_reset(int reason) {
    ESP_LOGE(kTag, "NimBLE reset: %d", reason);
    invalidate_authorization();
    portENTER_CRITICAL(&state_lock);
    current.state = CompanionLinkState::failed;
    set_detail_locked("Bluetooth stack reset");
    ++current.sequence;
    portEXIT_CRITICAL(&state_lock);
}

void on_sync() {
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &own_address_type) != 0) {
        on_reset(BLE_HS_EINVAL);
        return;
    }
    advertise();
}

void host_task(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool notify_outbound(const std::uint8_t *data, std::size_t length) {
    const auto handle = connection_handle.load();
    const auto state = instance.snapshot();
    const auto mtu = handle == kNoConnection ? std::uint16_t{0} : ble_att_mtu(handle);
    const auto maximum = mtu > 3 ? static_cast<std::size_t>(mtu - 3U) : 0U;
    if (handle == kNoConnection || !outbound_subscribed.load() || !state.encrypted ||
        length == 0 || length > maximum) {
        return false;
    }
    auto *buffer = ble_hs_mbuf_from_flat(data, static_cast<std::uint16_t>(length));
    return buffer && ble_gatts_notify_custom(handle, outbound_handle, buffer) == 0;
}

}  // namespace

nightglass::core::Status ConnectivityService::start() {
    current = {};
    invalidate_authorization();
    portENTER_CRITICAL(&peer_lock);
    one_shot_repair_peer = {};
    one_shot_repair_armed = false;
    portEXIT_CRITICAL(&peer_lock);
    current.settings = load_settings();
    set_pinned_peer(load_pinned_peer());
    current.peer_identity_pinned = valid_peer_identity(pinned_peer_snapshot());
    if (!current.settings.enabled) {
        current.state = CompanionLinkState::disabled;
        set_detail_locked("Bluetooth disabled");
        nightglass::core::health_registry().set(
            "connectivity", nightglass::core::HealthState::degraded,
            "Bluetooth disabled; OTA transport unavailable");
        nightglass::core::health_registry().set(
            "update_transport", nightglass::core::HealthState::failed,
            "OTA transport requires Bluetooth");
        return nightglass::core::Status::Ok();
    }
    const auto initialized = nimble_port_init();
    if (initialized != ESP_OK) {
        current.state = CompanionLinkState::failed;
        set_detail_locked("Bluetooth initialization failed");
        nightglass::core::health_registry().set(
            "connectivity", nightglass::core::HealthState::failed,
            "Bluetooth initialization failed");
        nightglass::core::health_registry().set(
            "update_transport", nightglass::core::HealthState::failed,
            "OTA transport unavailable because Bluetooth failed");
        return {nightglass::core::StatusCode::degraded, "Bluetooth initialization failed"};
    }
    host_started = true;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(gatt_services) != 0 || ble_gatts_add_svcs(gatt_services) != 0 ||
        ble_svc_gap_device_name_set(current.settings.device_name.data()) != 0) {
        current.state = CompanionLinkState::failed;
        set_detail_locked("Bluetooth service registration failed");
        nightglass::core::health_registry().set(
            "connectivity", nightglass::core::HealthState::failed,
            "Bluetooth service registration failed");
        nightglass::core::health_registry().set(
            "update_transport", nightglass::core::HealthState::failed,
            "OTA GATT service registration failed");
        return {nightglass::core::StatusCode::degraded, "Bluetooth service registration failed"};
    }
    ble_store_config_init();
    const auto update_transport_status = update_transport().start(notify_outbound);
    if (!update_transport_status.is_ok()) {
        current.state = CompanionLinkState::failed;
        set_detail_locked("Update transport unavailable");
        nightglass::core::health_registry().set(
            "connectivity", nightglass::core::HealthState::failed,
            "Bluetooth started without OTA transport");
        nightglass::core::health_registry().set(
            "update_transport", nightglass::core::HealthState::failed,
            update_transport_status.detail);
        return update_transport_status;
    }
    nimble_port_freertos_init(host_task);
    nightglass::core::health_registry().set(
        "connectivity", nightglass::core::HealthState::ok,
        "Authenticated companion BLE service started");
    nightglass::core::health_registry().set(
        "update_transport", nightglass::core::HealthState::ok,
        "Signed OTA transport worker started");
    ESP_LOGI(kTag, "Companion BLE service started; notification content logging disabled");
    return nightglass::core::Status::Ok();
}

ConnectivitySnapshot ConnectivityService::snapshot() const {
    portENTER_CRITICAL(&state_lock);
    if (current.reply_pending && reply_started_us > 0 &&
        esp_timer_get_time() - reply_started_us > 15'000'000) {
        current.reply_pending = false;
        current.reply_status = 5;
        reply_started_us = 0;
        ++current.sequence;
        ++current.notification_sequence;
    }
    const auto snapshot = current;
    portEXIT_CRITICAL(&state_lock);
    return snapshot;
}

nightglass::core::Status ConnectivityService::update_settings(
    const ConnectivitySettings &settings) {
    if (!valid_settings(settings)) {
        return {nightglass::core::StatusCode::invalid_state, "Invalid Bluetooth settings"};
    }
    const auto saved = save_settings(settings);
    if (saved != ESP_OK) {
        return {nightglass::core::StatusCode::io_error, "Bluetooth settings were not saved"};
    }
    portENTER_CRITICAL(&state_lock);
    current.settings = settings;
    ++current.sequence;
    portEXIT_CRITICAL(&state_lock);
    if (!host_started) {
        return settings.enabled ? start() : nightglass::core::Status::Ok();
    }
    ble_svc_gap_device_name_set(settings.device_name.data());
    if (!settings.enabled) {
        invalidate_authorization();
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        const auto handle = connection_handle.load();
        if (handle != kNoConnection) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
        portENTER_CRITICAL(&state_lock);
        current.state = CompanionLinkState::disabled;
        set_detail_locked("Bluetooth disabled");
        ++current.sequence;
        portEXIT_CRITICAL(&state_lock);
    } else if (connection_handle == kNoConnection) {
        advertise();
    }
    return nightglass::core::Status::Ok();
}

bool ConnectivityService::send_media(MediaCommand command) {
    const auto frame = encode_media_command(command, next_sequence8());
    return notify_outbound(frame.data(), frame.size());
}

bool ConnectivityService::send_call(CallCommand command) {
    const auto state = snapshot();
    if (state.call.session_id == 0 || state.call.generation == 0 ||
        (!state.call.ringing && !state.call.active)) return false;
    if (command == CallCommand::mute && state.call.muted) return true;
    if (command == CallCommand::unmute && !state.call.muted) return true;
    const auto frame = encode_call_command(command, next_sequence16(),
                                           state.call.session_id,
                                           state.call.generation);
    return notify_outbound(frame.data(), frame.size());
}

bool ConnectivityService::send_phone(PhoneCommand command) {
    const auto frame = encode_phone_command(command, next_sequence8());
    return notify_outbound(frame.data(), frame.size());
}

bool ConnectivityService::mark_notification(std::uint32_t id, bool dismiss) {
    if (id == 0) return false;
    const auto frame = encode_notification_action(dismiss, id,
                                                   next_sequence8());
    if (!notify_outbound(frame.data(), frame.size())) return false;
    if (dismiss) {
        portENTER_CRITICAL(&state_lock);
        remove_notification_locked(id);
        ++current.sequence;
        ++current.notification_sequence;
        portEXIT_CRITICAL(&state_lock);
    }
    return true;
}

bool ConnectivityService::reply_notification(std::uint32_t id, const char *reply) {
    if (id == 0 || reply == nullptr || reply[0] == '\0') return false;
    bool replyable = false;
    bool pending = false;
    portENTER_CRITICAL(&state_lock);
    replyable = std::any_of(current.notifications.begin(), current.notifications.end(),
                            [id](const auto &notification) {
                                return notification.valid && notification.id == id &&
                                       notification.replyable;
                            });
    if (current.reply_pending && reply_started_us > 0 &&
        esp_timer_get_time() - reply_started_us > 15'000'000) {
        current.reply_pending = false;
        current.reply_status = 5;
        reply_started_us = 0;
        ++current.sequence;
    }
    pending = current.reply_pending;
    portEXIT_CRITICAL(&state_lock);
    if (!replyable || pending) return false;
    std::uint32_t nonce{};
    while (nonce == 0) nonce = esp_random();
    const auto frame = encode_notification_reply(
        id, next_sequence8(), nonce, reply);
    if (frame.size == 0 || !notify_outbound(frame.bytes.data(), frame.size)) return false;
    portENTER_CRITICAL(&state_lock);
    current.reply_notification_id = id;
    current.reply_nonce = nonce;
    current.reply_status = 0xff;
    current.reply_pending = true;
    reply_started_us = esp_timer_get_time();
    ++current.sequence;
    portEXIT_CRITICAL(&state_lock);
    return true;
}

bool ConnectivityService::send_voice_frame(std::span<const std::uint8_t> frame) {
    if (frame.size() < 2 || frame.size() > kVoiceMaximumFrameBytes ||
        frame[0] != kVoiceProtocolVersion ||
        frame[1] < static_cast<std::uint8_t>(VoiceFrameKind::request_begin) ||
        frame[1] > static_cast<std::uint8_t>(VoiceFrameKind::request_cancel)) {
        return false;
    }
    return notify_outbound(frame.data(), frame.size());
}

std::size_t ConnectivityService::maximum_outbound_frame() const {
    const auto handle = connection_handle.load();
    if (handle == kNoConnection || !outbound_subscribed.load()) return 0;
    const auto mtu = ble_att_mtu(handle);
    return mtu > 3 ? std::min<std::size_t>(mtu - 3U, kVoiceMaximumFrameBytes) : 0;
}

void ConnectivityService::set_notification_privacy(NotificationPrivacyPolicy policy,
                                                    bool unlocked) {
    portENTER_CRITICAL(&state_lock);
    current.notification_privacy = policy;
    current.notification_details_unlocked = unlocked;
    if (notification_details_redacted_locked()) {
        for (auto &notification : current.notifications) {
            if (notification.valid) redact_notification(notification);
        }
        ++current.notification_sequence;
    }
    ++current.sequence;
    portEXIT_CRITICAL(&state_lock);
}

ConnectivityService &connectivity_service() { return instance; }

}  // namespace nightglass::services

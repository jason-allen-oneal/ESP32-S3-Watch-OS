#include "nightglass/services/connectivity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
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
#include "nightglass/services/network_weather.hpp"

extern "C" void ble_store_config_init(void);

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_ble";
constexpr char kNamespace[] = "ng_ble";
constexpr char kEnabledKey[] = "enabled";
constexpr char kNameKey[] = "name";
constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr std::size_t kMaxInboundFrame = 384;

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
ConnectivitySnapshot current{};
std::atomic<std::uint16_t> connection_handle{kNoConnection};
std::uint16_t status_handle{};
std::uint16_t outbound_handle{};
std::atomic_bool outbound_subscribed{false};
std::uint8_t own_address_type{};
std::atomic<std::uint8_t> outbound_sequence{0};
bool host_started{};

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

void remove_notification_locked(std::uint32_t id) {
    for (auto &notification : current.notifications) {
        if (notification.valid && notification.id == id) notification = {};
    }
    current.notification_count = static_cast<std::uint8_t>(std::count_if(
        current.notifications.begin(), current.notifications.end(),
        [](const auto &notification) { return notification.valid; }));
}

bool apply_message(const CompanionMessage &message) {
    if (message.kind == CompanionMessageKind::wifi_provision) {
        return network_weather_service().provision_credentials(
            message.wifi.ssid.data(), message.wifi.ssid_length,
            message.wifi.password.data(), message.wifi.password_length).is_ok();
    }
    if (message.kind == CompanionMessageKind::wifi_clear) {
        return network_weather_service().clear_credentials().is_ok();
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
    portENTER_CRITICAL(&state_lock);
    if (message.kind == CompanionMessageKind::notification_clear) {
        current.notifications = {};
        current.notification_count = 0;
    } else if (message.kind == CompanionMessageKind::notification_remove) {
        remove_notification_locked(message.notification_id);
    } else if (message.kind == CompanionMessageKind::notification_upsert) {
        remove_notification_locked(message.notification_id);
        for (std::size_t index = current.notifications.size() - 1; index > 0; --index) {
            current.notifications[index] = current.notifications[index - 1];
        }
        current.notifications[0] = message.notification;
        current.notification_count = static_cast<std::uint8_t>(std::count_if(
            current.notifications.begin(), current.notifications.end(),
            [](const auto &notification) { return notification.valid; }));
    }
    ++current.sequence;
    portEXIT_CRITICAL(&state_lock);
    return true;
}

int gatt_access(std::uint16_t, std::uint16_t attr_handle, ble_gatt_access_ctxt *context,
                void *) {
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == status_handle) {
        const auto snapshot = instance.snapshot();
        const std::array<std::uint8_t, 5> status{
            kCompanionProtocolVersion, static_cast<std::uint8_t>(snapshot.state),
            snapshot.notification_count, snapshot.encrypted ? std::uint8_t{1} : std::uint8_t{0},
            snapshot.bonded ? std::uint8_t{1} : std::uint8_t{0}};
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
    CompanionMessage message{};
    if (!parse_companion_message(std::span(frame.data(), length), message)) {
        secure_wipe(frame.data(), frame.size());
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    const bool applied = apply_message(message);
    secure_wipe(&message, sizeof(message));
    secure_wipe(frame.data(), frame.size());
    return applied ? 0 : BLE_ATT_ERR_UNLIKELY;
}

const ble_gatt_chr_def gatt_characteristics[]{
    {.uuid = &kStatusUuid.u,
     .access_cb = gatt_access,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &status_handle},
    {.uuid = &kInboundUuid.u,
     .access_cb = gatt_access,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC},
    {.uuid = &kOutboundUuid.u,
     .access_cb = gatt_access,
     .flags = BLE_GATT_CHR_F_NOTIFY,
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
            connection_handle = kNoConnection;
            outbound_subscribed = false;
            portENTER_CRITICAL(&state_lock);
            const bool should_advertise = current.settings.enabled;
            current.state = should_advertise ? CompanionLinkState::advertising
                                             : CompanionLinkState::disabled;
            current.encrypted = false;
            set_detail_locked(should_advertise ? "Advertising" : "Bluetooth disabled");
            ++current.sequence;
            portEXIT_CRITICAL(&state_lock);
            if (should_advertise) advertise();
            return 0;
        }
        case BLE_GAP_EVENT_ENC_CHANGE: {
            ble_gap_conn_desc descriptor{};
            if (ble_gap_conn_find(event->enc_change.conn_handle, &descriptor) == 0) {
                portENTER_CRITICAL(&state_lock);
                current.encrypted = descriptor.sec_state.encrypted;
                current.bonded = descriptor.sec_state.bonded;
                current.state = current.encrypted ? CompanionLinkState::connected_encrypted
                                                  : CompanionLinkState::connected_unsecured;
                set_detail_locked(current.encrypted ? "Companion connected"
                                                    : "Companion link is not encrypted");
                ++current.sequence;
                portEXIT_CRITICAL(&state_lock);
            }
            return 0;
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
            // The peer has forgotten or replaced its keys while Nightglass
            // still has the previous bond. Delete only that peer's stale bond
            // and let NimBLE restart secure pairing on the existing link.
            ble_gap_conn_desc descriptor{};
            const auto find_result =
                ble_gap_conn_find(event->repeat_pairing.conn_handle, &descriptor);
            if (find_result != 0) {
                ESP_LOGE(kTag, "Unable to resolve repeat-pairing peer: %d", find_result);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            const auto delete_result = ble_store_util_delete_peer(&descriptor.peer_id_addr);
            if (delete_result != 0) {
                ESP_LOGE(kTag, "Unable to remove stale companion bond: %d", delete_result);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            ESP_LOGI(kTag, "Removed stale companion bond; retrying secure pairing");
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
    if (handle == kNoConnection || !outbound_subscribed.load() || !state.encrypted) {
        return false;
    }
    auto *buffer = ble_hs_mbuf_from_flat(data, static_cast<std::uint16_t>(length));
    return buffer && ble_gatts_notify_custom(handle, outbound_handle, buffer) == 0;
}

}  // namespace

nightglass::core::Status ConnectivityService::start() {
    current = {};
    current.settings = load_settings();
    if (!current.settings.enabled) {
        current.state = CompanionLinkState::disabled;
        set_detail_locked("Bluetooth disabled");
        return nightglass::core::Status::Ok();
    }
    const auto initialized = nimble_port_init();
    if (initialized != ESP_OK) {
        current.state = CompanionLinkState::failed;
        set_detail_locked("Bluetooth initialization failed");
        return {nightglass::core::StatusCode::degraded, "Bluetooth initialization failed"};
    }
    host_started = true;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;  // Just Works until the on-watch passkey UI exists.
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(gatt_services) != 0 || ble_gatts_add_svcs(gatt_services) != 0 ||
        ble_svc_gap_device_name_set(current.settings.device_name.data()) != 0) {
        current.state = CompanionLinkState::failed;
        set_detail_locked("Bluetooth service registration failed");
        return {nightglass::core::StatusCode::degraded, "Bluetooth service registration failed"};
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    ESP_LOGI(kTag, "Companion BLE service started; notification content logging disabled");
    return nightglass::core::Status::Ok();
}

ConnectivitySnapshot ConnectivityService::snapshot() const {
    portENTER_CRITICAL(&state_lock);
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
    const auto frame = encode_media_command(command, outbound_sequence.fetch_add(1) + 1);
    return notify_outbound(frame.data(), frame.size());
}

bool ConnectivityService::mark_notification(std::uint32_t id, bool dismiss) {
    if (id == 0) return false;
    const auto frame = encode_notification_action(dismiss, id,
                                                   outbound_sequence.fetch_add(1) + 1);
    if (!notify_outbound(frame.data(), frame.size())) return false;
    if (dismiss) {
        portENTER_CRITICAL(&state_lock);
        remove_notification_locked(id);
        ++current.sequence;
        portEXIT_CRITICAL(&state_lock);
    }
    return true;
}

ConnectivityService &connectivity_service() { return instance; }

}  // namespace nightglass::services

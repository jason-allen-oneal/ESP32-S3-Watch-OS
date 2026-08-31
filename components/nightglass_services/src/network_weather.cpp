#include "nightglass/services/network_weather.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/time.h>

#include "driver/usb_serial_jtag.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nightglass/core/health.hpp"
#include "nightglass/services/power.hpp"
#include "nightglass/services/clock.hpp"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_net";
constexpr char kNvsNamespace[] = "ng_network";
constexpr char kSettingsKey[] = "settings";
constexpr char kCacheKey[] = "weather_cache";
constexpr std::uint32_t kSettingsMagic = 0x4E475753;    // NGWS
constexpr std::uint8_t kStorageVersion = 1;
constexpr std::size_t kMaxSsid = 32;
constexpr std::size_t kMaxPassword = 64;
constexpr std::size_t kMaxResponse = 4096;
constexpr std::int64_t kWorkerPeriodUs = 1'000'000;
constexpr std::uint32_t kMaximumProxyAgeSeconds = 6U * 60U * 60U;
constexpr std::uint32_t kMaximumFutureSkewSeconds = 5U * 60U;

struct CredentialBlob {
    std::uint8_t ssid_length{0};
    std::uint8_t password_length{0};
    std::array<char, kMaxSsid + 1> ssid{};
    std::array<char, kMaxPassword + 1> password{};
};

struct SettingsBlob {
    std::uint32_t magic{kSettingsMagic};
    std::uint8_t version{kStorageVersion};
    std::uint8_t enabled{0};
    std::uint8_t location_configured{0};
    std::uint8_t units{0};
    std::int32_t latitude_e6{0};
    std::int32_t longitude_e6{0};
    std::uint16_t refresh_minutes{30};
    std::uint16_t reserved{0};
    std::uint32_t checksum{0};
};

struct CacheBlob {
    std::uint32_t magic{0x4E475743};  // NGWC
    std::uint8_t version{1};
    std::uint8_t units{0};
    std::uint8_t is_day{0};
    std::uint8_t reserved{0};
    std::uint32_t observed_epoch_seconds{0};
    std::int32_t latitude_e6{0};
    std::int32_t longitude_e6{0};
    std::int16_t temperature_tenths{0};
    std::int16_t apparent_tenths{0};
    std::uint16_t weather_code{0};
    std::uint16_t wind_tenths{0};
    std::uint32_t checksum{0};
};

NetworkWeatherService instance;
TaskHandle_t worker_task = nullptr;
SemaphoreHandle_t credential_mutex = nullptr;
portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
NetworkWeatherSnapshot current{};
std::atomic_bool wifi_initialized{false};
std::atomic_bool wifi_started{false};
std::atomic_bool connect_requested{false};
std::atomic_bool got_ip{false};
std::atomic_bool credentials_dirty{false};
std::atomic_bool sleep_suspended{false};
std::atomic_bool fetch_in_flight{false};
std::atomic<std::int64_t> next_connect_us{0};
std::atomic<std::int64_t> next_fetch_us{0};
std::atomic<std::int64_t> last_good_us{0};
std::uint32_t configuration_generation = 0;
CredentialBlob runtime_credentials{};
std::atomic<std::uint8_t> weather_failure_attempt{0};
esp_event_handler_instance_t wifi_handler{};
esp_event_handler_instance_t ip_handler{};

void wipe(void *memory, std::size_t length) {
    auto *bytes = static_cast<volatile std::uint8_t *>(memory);
    while (length--) *bytes++ = 0;
}

bool valid_password(const char *password, std::size_t length) {
    if (length == 0) return true;
    if (!password || std::memchr(password, '\0', length)) return false;
    if (length >= 8 && length <= 63) return true;
    if (length != 64) return false;
    for (std::size_t index = 0; index < length; ++index) {
        const char c = password[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::uint32_t checksum(const void *data, std::size_t length) {
    auto hash = std::uint32_t{2166136261U};
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    for (std::size_t index = 0; index < length; ++index) {
        hash = (hash ^ bytes[index]) * 16777619U;
    }
    return hash;
}

bool valid_settings(const NetworkWeatherSettings &settings) {
    return (!settings.location_configured ||
            valid_coordinates(settings.latitude_e6, settings.longitude_e6)) &&
           settings.refresh_minutes >= 15 && settings.refresh_minutes <= 360 &&
           (settings.units == WeatherUnits::metric ||
            settings.units == WeatherUnits::imperial);
}

NetworkWeatherSettings load_settings() {
    NetworkWeatherSettings settings{};
    SettingsBlob blob{};
    nvs_handle_t handle{};
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return settings;
    std::size_t length = sizeof(blob);
    const esp_err_t result = nvs_get_blob(handle, kSettingsKey, &blob, &length);
    nvs_close(handle);
    if (result != ESP_OK || length != sizeof(blob) || blob.magic != kSettingsMagic ||
        blob.version != kStorageVersion ||
        checksum(&blob, offsetof(SettingsBlob, checksum)) != blob.checksum) {
        return settings;
    }
    settings.enabled = blob.enabled != 0;
    settings.location_configured = blob.location_configured != 0;
    settings.latitude_e6 = blob.latitude_e6;
    settings.longitude_e6 = blob.longitude_e6;
    settings.units = blob.units == 0 ? WeatherUnits::metric : WeatherUnits::imperial;
    settings.refresh_minutes = blob.refresh_minutes;
    return valid_settings(settings) ? settings : NetworkWeatherSettings{};
}

esp_err_t save_settings(const NetworkWeatherSettings &settings) {
    SettingsBlob blob{};
    blob.enabled = settings.enabled;
    blob.location_configured = settings.location_configured;
    blob.latitude_e6 = settings.latitude_e6;
    blob.longitude_e6 = settings.longitude_e6;
    blob.units = settings.units == WeatherUnits::metric ? 0 : 1;
    blob.refresh_minutes = settings.refresh_minutes;
    blob.checksum = checksum(&blob, offsetof(SettingsBlob, checksum));
    nvs_handle_t handle{};
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_blob(handle, kSettingsKey, &blob, sizeof(blob));
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return result;
}

bool load_cache(CacheBlob &blob) {
    nvs_handle_t handle{};
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t length = sizeof(blob);
    const esp_err_t result = nvs_get_blob(handle, kCacheKey, &blob, &length);
    nvs_close(handle);
    return result == ESP_OK && length == sizeof(blob) && blob.magic == 0x4E475743 &&
           blob.version == 1 && blob.units <= 1 && blob.is_day <= 1 &&
           blob.observed_epoch_seconds >= 1'577'836'800U &&
           valid_coordinates(blob.latitude_e6, blob.longitude_e6) &&
           blob.temperature_tenths >= -1500 && blob.temperature_tenths <= 1500 &&
           blob.apparent_tenths >= -1500 && blob.apparent_tenths <= 1500 &&
           blob.weather_code <= 999 && blob.wind_tenths <= 5000 &&
           checksum(&blob, offsetof(CacheBlob, checksum)) == blob.checksum;
}

esp_err_t save_cache(const NetworkWeatherSnapshot &snapshot) {
    CacheBlob blob{};
    blob.units = snapshot.settings.units == WeatherUnits::metric ? 0 : 1;
    blob.is_day = snapshot.current.is_day ? 1 : 0;
    blob.observed_epoch_seconds = snapshot.observed_epoch_seconds;
    blob.latitude_e6 = snapshot.settings.latitude_e6;
    blob.longitude_e6 = snapshot.settings.longitude_e6;
    blob.temperature_tenths = static_cast<std::int16_t>(
        std::lround(snapshot.current.temperature * 10.0F));
    blob.apparent_tenths = static_cast<std::int16_t>(
        std::lround(snapshot.current.apparent_temperature * 10.0F));
    blob.weather_code = snapshot.current.weather_code;
    blob.wind_tenths = static_cast<std::uint16_t>(
        std::lround(snapshot.current.wind_speed * 10.0F));
    blob.checksum = checksum(&blob, offsetof(CacheBlob, checksum));
    nvs_handle_t handle{};
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_blob(handle, kCacheKey, &blob, sizeof(blob));
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return result;
}

void publish_network(NetworkState state, bool connected, WeatherError error) {
    portENTER_CRITICAL(&state_mux);
    current.network_state = state;
    current.connected = connected;
    if (error != WeatherError::none) current.last_error = error;
    ++current.sequence;
    portEXIT_CRITICAL(&state_mux);
}

void publish_weather_state(WeatherState state, WeatherError error) {
    portENTER_CRITICAL(&state_mux);
    current.weather_state = state;
    current.last_error = error;
    ++current.sequence;
    portEXIT_CRITICAL(&state_mux);
}

void handle_event(void *, esp_event_base_t base, std::int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_started.store(true);
        connect_requested.store(false);
        next_connect_us.store(0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        wifi_started.store(false);
        got_ip.store(false);
        connect_requested.store(false);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        got_ip.store(false);
        connect_requested.store(false);
        const auto now = esp_timer_get_time();
        portENTER_CRITICAL(&state_mux);
        if (current.reconnect_attempt < std::numeric_limits<std::uint8_t>::max()) {
            ++current.reconnect_attempt;
        }
        const auto delay = reconnect_backoff_seconds(current.reconnect_attempt);
        current.retry_in_seconds = delay;
        current.connected = false;
        current.network_state = NetworkState::backoff;
        current.weather_state = current.data_valid ? WeatherState::offline
                                                   : WeatherState::unavailable;
        current.last_error = WeatherError::offline;
        ++current.sequence;
        portEXIT_CRITICAL(&state_mux);
        next_connect_us.store(now + static_cast<std::int64_t>(delay) * 1'000'000);
        nightglass::core::health_registry().set(
            "network", nightglass::core::HealthState::degraded,
            "Wi-Fi disconnected; reconnect backoff active");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        got_ip.store(true);
        connect_requested.store(false);
        next_fetch_us.store(0);
        portENTER_CRITICAL(&state_mux);
        current.connected = true;
        current.network_state = NetworkState::online;
        current.reconnect_attempt = 0;
        current.retry_in_seconds = 0;
        current.last_error = WeatherError::none;
        ++current.sequence;
        portEXIT_CRITICAL(&state_mux);
        nightglass::core::health_registry().set(
            "network", nightglass::core::HealthState::ok,
            "Wi-Fi station online");
    }
    if (worker_task) xTaskNotifyGive(worker_task);
    (void)data;
}

esp_err_t initialize_wifi() {
    if (wifi_initialized.load()) return ESP_OK;
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((result = esp_wifi_init(&init)) != ESP_OK ||
        (result = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (result = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (result = esp_wifi_set_ps(WIFI_PS_MIN_MODEM)) != ESP_OK ||
        (result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                      handle_event, nullptr,
                                                      &wifi_handler)) != ESP_OK ||
        (result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                      handle_event, nullptr,
                                                      &ip_handler)) != ESP_OK) {
        return result;
    }
    wifi_initialized.store(true);
    return ESP_OK;
}

// credential_mutex must be held by the caller. Keeping credential application
// and station start in the same transaction prevents a delayed start from
// escaping credential clearing or light-sleep preparation.
esp_err_t apply_credentials_locked() {
    if (runtime_credentials.ssid_length == 0) return ESP_ERR_INVALID_STATE;
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, runtime_credentials.ssid.data(),
                runtime_credentials.ssid_length);
    std::memcpy(config.sta.password, runtime_credentials.password.data(),
                runtime_credentials.password_length);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = runtime_credentials.password_length == 0
                                        ? WIFI_AUTH_OPEN
                                        : WIFI_AUTH_WPA2_PSK;
    const esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    wipe(&config, sizeof(config));
    return result;
}

esp_err_t ensure_wifi_started() {
    if (!credential_mutex ||
        xSemaphoreTake(credential_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sleep_suspended.load()) {
        xSemaphoreGive(credential_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = initialize_wifi();
    if (result == ESP_OK && !wifi_started.load()) {
        result = apply_credentials_locked();
        if (result == ESP_OK && !sleep_suspended.load()) {
            result = esp_wifi_start();
            // The STA_START event is asynchronous. Publish ownership here so
            // sleep/clear cannot miss a station that has already been started.
            if (result == ESP_OK) wifi_started.store(true);
        } else if (result == ESP_OK) {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    xSemaphoreGive(credential_mutex);
    return result;
}

struct HttpBuffer {
    std::array<char, kMaxResponse + 1> bytes{};
    std::size_t length{0};
    bool overflow{false};
};

esp_err_t http_event(esp_http_client_event_t *event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data ||
        !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    auto &buffer = *static_cast<HttpBuffer *>(event->user_data);
    const auto incoming = static_cast<std::size_t>(event->data_len);
    if (incoming > kMaxResponse - buffer.length) {
        buffer.overflow = true;
        return ESP_FAIL;
    }
    std::memcpy(buffer.bytes.data() + buffer.length, event->data, incoming);
    buffer.length += incoming;
    buffer.bytes[buffer.length] = '\0';
    return ESP_OK;
}

WeatherError fetch_weather(const NetworkWeatherSettings &settings,
                           DecodedWeather &decoded) {
    const auto clock = clock_service().snapshot();
    if (!clock.time_valid || clock.utc_epoch_seconds < 1'577'836'800) {
        return WeatherError::time_unavailable;
    }
    const timeval wall_time{.tv_sec = clock.utc_epoch_seconds, .tv_usec = 0};
    if (settimeofday(&wall_time, nullptr) != 0) return WeatherError::time_unavailable;
    char latitude[20]{};
    char longitude[20]{};
    std::snprintf(latitude, sizeof(latitude), "%.6f",
                  static_cast<double>(settings.latitude_e6) / 1'000'000.0);
    std::snprintf(longitude, sizeof(longitude), "%.6f",
                  static_cast<double>(settings.longitude_e6) / 1'000'000.0);
    const char *temperature_unit = settings.units == WeatherUnits::imperial
                                       ? "fahrenheit" : "celsius";
    const char *wind_unit = settings.units == WeatherUnits::imperial ? "mph" : "kmh";
    char url[512]{};
    const int written = std::snprintf(
        url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&"
        "current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,is_day&"
        "temperature_unit=%s&wind_speed_unit=%s&timezone=auto&forecast_days=1",
        latitude, longitude, temperature_unit, wind_unit);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(url)) {
        return WeatherError::transport;
    }

    HttpBuffer response{};
    esp_http_client_config_t config{};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = http_event;
    config.user_data = &response;
    config.timeout_ms = 8000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.user_agent = "Nightglass/1";
    config.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return WeatherError::transport;
    const esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (response.overflow) return WeatherError::response_too_large;
    if (result != ESP_OK) return WeatherError::transport;
    if (status != 200) return WeatherError::http_status;
    if (!decode_open_meteo_current(
            std::string_view(response.bytes.data(), response.length), decoded)) {
        return WeatherError::invalid_response;
    }
    return WeatherError::none;
}

void connect_if_due(std::int64_t now) {
    if (!wifi_started.load() || got_ip.load() || connect_requested.load() ||
        now < next_connect_us.load()) return;
    if (esp_wifi_connect() == ESP_OK) {
        connect_requested.store(true);
        publish_network(NetworkState::connecting, false, WeatherError::none);
    } else {
        next_connect_us.store(now + 5'000'000);
        publish_network(NetworkState::error, false, WeatherError::transport);
    }
}

void update_age(std::int64_t now) {
    const auto clock = clock_service().snapshot();
    portENTER_CRITICAL(&state_mux);
    if (current.data_valid) {
        std::uint32_t wall_age = 0;
        const bool wall_age_valid = clock.time_valid && weather_observation_age(
            current.observed_epoch_seconds, clock.utc_epoch_seconds,
            kMaximumFutureSkewSeconds, wall_age);
        if (clock.time_valid && current.observed_epoch_seconds > 0 && !wall_age_valid) {
            current.data_valid = false;
            current.stale = false;
            current.source = WeatherSource::none;
            current.weather_state = WeatherState::unavailable;
            current.last_error = WeatherError::stale_payload;
            ++current.sequence;
            portEXIT_CRITICAL(&state_mux);
            return;
        }
        const auto last_good = last_good_us.load();
        const auto age = wall_age_valid
                             ? static_cast<std::int64_t>(wall_age)
                             : last_good > 0
                                   ? std::max<std::int64_t>(0, now - last_good) / 1'000'000
                                   : 0;
        current.age_seconds = age > std::numeric_limits<std::uint32_t>::max()
                                  ? std::numeric_limits<std::uint32_t>::max()
                                  : static_cast<std::uint32_t>(age);
        current.stale = weather_is_stale(current.age_seconds,
                                         current.settings.refresh_minutes);
        if (current.stale && current.weather_state == WeatherState::fresh) {
            current.weather_state = WeatherState::stale;
        }
    }
    ++current.sequence;
    portEXIT_CRITICAL(&state_mux);
}

void worker(void *) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        const auto now = esp_timer_get_time();
        NetworkWeatherSnapshot snapshot{};
        std::uint32_t observed_generation = 0;
        portENTER_CRITICAL(&state_mux);
        snapshot = current;
        observed_generation = configuration_generation;
        portEXIT_CRITICAL(&state_mux);

        if (sleep_suspended.load()) continue;

        update_age(now);
        portENTER_CRITICAL(&state_mux);
        snapshot = current;
        portEXIT_CRITICAL(&state_mux);

        // A fresh phone snapshot is the preferred path: keep the watch Wi-Fi
        // radio off and let the companion use whatever Internet transport the
        // phone currently owns (Wi-Fi or cellular).
        if (snapshot.data_valid && snapshot.source == WeatherSource::phone &&
            !snapshot.stale) {
            if (wifi_started.load()) {
                esp_wifi_disconnect();
                esp_wifi_stop();
                wifi_started.store(false);
                got_ip.store(false);
                connect_requested.store(false);
            }
            publish_network(NetworkState::disabled, false, WeatherError::none);
            continue;
        }

        if (!snapshot.settings.enabled || !snapshot.credentials_configured) {
            if (wifi_started.load()) {
                esp_wifi_disconnect();
                esp_wifi_stop();
                wifi_started.store(false);
                got_ip.store(false);
                connect_requested.store(false);
            }
            publish_network(snapshot.settings.enabled ? NetworkState::unprovisioned
                                                      : NetworkState::disabled,
                            false, WeatherError::none);
            continue;
        }
        const esp_err_t startup = ensure_wifi_started();
        if (startup != ESP_OK) {
            if (sleep_suspended.load()) continue;
            publish_network(NetworkState::error, false, WeatherError::transport);
            nightglass::core::health_registry().set(
                "network", nightglass::core::HealthState::degraded,
                "Wi-Fi station initialization failed");
            continue;
        }
        if (credentials_dirty.exchange(false) && wifi_started.load()) {
            if (!credential_mutex ||
                xSemaphoreTake(credential_mutex, portMAX_DELAY) != pdTRUE) {
                publish_network(NetworkState::error, false, WeatherError::transport);
                continue;
            }
            esp_wifi_disconnect();
            connect_requested.store(false);
            got_ip.store(false);
            const esp_err_t applied = sleep_suspended.load()
                                          ? ESP_ERR_INVALID_STATE
                                          : apply_credentials_locked();
            xSemaphoreGive(credential_mutex);
            if (applied != ESP_OK) {
                if (sleep_suspended.load()) continue;
                publish_network(NetworkState::error, false, WeatherError::transport);
                continue;
            }
            next_connect_us.store(0);
        }
        connect_if_due(now);
        portENTER_CRITICAL(&state_mux);
        snapshot = current;
        portEXIT_CRITICAL(&state_mux);
        const auto power = power_service().snapshot();
        const bool interactive = power.state == nightglass::core::PowerState::active ||
                                 power.state == nightglass::core::PowerState::dim;
        if (!snapshot.connected || !snapshot.settings.location_configured ||
            !interactive || now < next_fetch_us.load()) {
            if (!snapshot.settings.location_configured) {
                publish_weather_state(WeatherState::unavailable,
                                      WeatherError::no_location);
            }
            continue;
        }

        publish_weather_state(WeatherState::fetching, WeatherError::none);
        fetch_in_flight.store(true);
        if (sleep_suspended.load()) {
            fetch_in_flight.store(false);
            continue;
        }
        DecodedWeather decoded{};
        const WeatherError fetch = fetch_weather(snapshot.settings, decoded);
        fetch_in_flight.store(false);
        if (fetch == WeatherError::none) {
            wifi_ap_record_t ap{};
            const auto rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
            const auto observed_epoch = clock_service().snapshot().utc_epoch_seconds;
            portENTER_CRITICAL(&state_mux);
            if (configuration_generation != observed_generation ||
                !current.settings.enabled || !current.credentials_configured ||
                !current.connected) {
                portEXIT_CRITICAL(&state_mux);
                continue;
            }
            current.current = decoded;
            current.data_valid = true;
            current.stale = false;
            current.age_seconds = 0;
            current.observed_epoch_seconds = observed_epoch;
            current.source = WeatherSource::direct;
            current.rssi_dbm = static_cast<std::int8_t>(rssi);
            current.weather_state = WeatherState::fresh;
            current.last_error = WeatherError::none;
            ++current.sequence;
            portEXIT_CRITICAL(&state_mux);
            const auto saved = instance.snapshot();
            if (save_cache(saved) != ESP_OK) {
                nightglass::core::health_registry().set(
                    "weather", nightglass::core::HealthState::degraded,
                    "Last-good weather cache persistence failed");
            }
            weather_failure_attempt.store(0);
            last_good_us.store(esp_timer_get_time());
            next_fetch_us.store(
                last_good_us.load() +
                static_cast<std::int64_t>(snapshot.settings.refresh_minutes) * 60'000'000);
            nightglass::core::health_registry().set(
                "weather", nightglass::core::HealthState::ok,
                "Open-Meteo current weather updated over verified HTTPS");
        } else {
            portENTER_CRITICAL(&state_mux);
            if (configuration_generation != observed_generation ||
                !current.settings.enabled || !current.credentials_configured) {
                portEXIT_CRITICAL(&state_mux);
                continue;
            }
            current.weather_state = current.data_valid ? WeatherState::stale
                                                       : WeatherState::error;
            current.last_error = fetch;
            current.stale = current.data_valid;
            ++current.sequence;
            portEXIT_CRITICAL(&state_mux);
            const auto failure = weather_failure_attempt.fetch_add(1);
            const auto retry_seconds = std::max<std::uint32_t>(
                30, reconnect_backoff_seconds(static_cast<std::uint8_t>(
                        std::min<unsigned>(failure + 5U, 10U))));
            next_fetch_us.store(esp_timer_get_time() +
                                static_cast<std::int64_t>(retry_seconds) * 1'000'000);
            nightglass::core::health_registry().set(
                "weather", nightglass::core::HealthState::degraded,
                "Weather refresh failed; last-good data retained");
        }
    }
}

}  // namespace

nightglass::core::Status NetworkWeatherService::start() {
    if (worker_task) return nightglass::core::Status::Ok();
    credential_mutex = xSemaphoreCreateMutex();
    if (!credential_mutex) {
        return {nightglass::core::StatusCode::no_memory,
                "credential mutex creation failed"};
    }
    const auto settings = load_settings();
    const bool provisioned = false;
    CacheBlob cache{};
    const auto clock = clock_service().snapshot();
    const bool cache_valid = load_cache(cache) && clock.time_valid &&
        settings.location_configured && cache.latitude_e6 == settings.latitude_e6 &&
        cache.longitude_e6 == settings.longitude_e6 &&
        weather_cache_is_usable(cache.units == 0 ? WeatherUnits::metric
                                                 : WeatherUnits::imperial,
                                settings.units, cache.observed_epoch_seconds,
                                clock.utc_epoch_seconds, kMaximumFutureSkewSeconds);

    portENTER_CRITICAL(&state_mux);
    current = {};
    current.settings = settings;
    current.credentials_configured = provisioned;
    current.network_state = !settings.enabled ? NetworkState::disabled
                            : provisioned ? NetworkState::connecting
                                          : NetworkState::unprovisioned;
    current.weather_state = WeatherState::unavailable;
    current.last_error = settings.location_configured ? WeatherError::none
                                                       : WeatherError::no_location;
    if (cache_valid) {
        current.current.temperature = cache.temperature_tenths / 10.0F;
        current.current.apparent_temperature = cache.apparent_tenths / 10.0F;
        current.current.weather_code = cache.weather_code;
        current.current.wind_speed = cache.wind_tenths / 10.0F;
        current.current.is_day = cache.is_day != 0;
        current.observed_epoch_seconds = cache.observed_epoch_seconds;
        current.data_valid = true;
        current.stale = true;
        current.source = WeatherSource::cache;
        current.weather_state = WeatherState::offline;
    }
    ++current.sequence;
    portEXIT_CRITICAL(&state_mux);

    if (xTaskCreatePinnedToCore(worker, "nightglass_network", 8192, nullptr, 4,
                                &worker_task, 0) != pdPASS) {
        worker_task = nullptr;
        vSemaphoreDelete(credential_mutex);
        credential_mutex = nullptr;
        return {nightglass::core::StatusCode::no_memory, "network task creation failed"};
    }
    nightglass::core::health_registry().set(
        "network", settings.enabled ? nightglass::core::HealthState::degraded
                                    : nightglass::core::HealthState::ok,
        settings.enabled ? "Wi-Fi waiting for connection"
                         : "Wi-Fi disabled by settings");
    ESP_LOGI(kTag, "Network service active: enabled=%u provisioned=%u location=%u",
             settings.enabled, provisioned, settings.location_configured);
    return nightglass::core::Status::Ok();
}

NetworkWeatherSnapshot NetworkWeatherService::snapshot() const {
    NetworkWeatherSnapshot copy{};
    portENTER_CRITICAL(&state_mux);
    copy = current;
    portEXIT_CRITICAL(&state_mux);
    return copy;
}

nightglass::core::Status NetworkWeatherService::update_settings(
    const NetworkWeatherSettings &settings) {
    if (!valid_settings(settings)) {
        return {nightglass::core::StatusCode::invalid_state,
                "invalid network/weather settings"};
    }
    const esp_err_t result = save_settings(settings);
    if (result != ESP_OK) {
        return {nightglass::core::StatusCode::io_error,
                "network/weather settings persistence failed"};
    }
    portENTER_CRITICAL(&state_mux);
    const bool data_scope_changed = current.settings.units != settings.units ||
        current.settings.location_configured != settings.location_configured ||
        current.settings.latitude_e6 != settings.latitude_e6 ||
        current.settings.longitude_e6 != settings.longitude_e6;
    current.settings = settings;
    if (data_scope_changed) {
        current.current = {};
        current.data_valid = false;
        current.stale = false;
        current.age_seconds = 0;
        current.observed_epoch_seconds = 0;
        current.source = WeatherSource::none;
        current.weather_state = WeatherState::unavailable;
    }
    current.network_state = !settings.enabled ? NetworkState::disabled
                            : current.credentials_configured ? NetworkState::connecting
                                                             : NetworkState::unprovisioned;
    ++configuration_generation;
    ++current.sequence;
    portEXIT_CRITICAL(&state_mux);
    next_fetch_us.store(0);
    weather_failure_attempt.store(0);
    if (worker_task) xTaskNotifyGive(worker_task);
    return nightglass::core::Status::Ok();
}

nightglass::core::Status NetworkWeatherService::provision_credentials(
    const char *ssid, std::size_t ssid_length, const char *password,
    std::size_t password_length) {
    if (!ssid || ssid_length == 0 || ssid_length > kMaxSsid ||
        std::memchr(ssid, '\0', ssid_length) || password_length > kMaxPassword ||
        !valid_password(password, password_length)) {
        return {nightglass::core::StatusCode::invalid_state,
                "invalid Wi-Fi credential lengths"};
    }
    CredentialBlob candidate{};
    candidate.ssid_length = static_cast<std::uint8_t>(ssid_length);
    candidate.password_length = static_cast<std::uint8_t>(password_length);
    std::memcpy(candidate.ssid.data(), ssid, ssid_length);
    if (password_length) std::memcpy(candidate.password.data(), password, password_length);
    if (credential_mutex &&
        xSemaphoreTake(credential_mutex, portMAX_DELAY) != pdTRUE) {
        wipe(&candidate, sizeof(candidate));
        return {nightglass::core::StatusCode::invalid_state,
                "credential store busy"};
    }
    wipe(&runtime_credentials, sizeof(runtime_credentials));
    runtime_credentials = candidate;
    if (credential_mutex) xSemaphoreGive(credential_mutex);
    wipe(&candidate, sizeof(candidate));
    credentials_dirty.store(true);
    portENTER_CRITICAL(&state_mux);
    current.credentials_configured = true;
    current.network_state = current.settings.enabled ? NetworkState::connecting
                                                      : NetworkState::disabled;
    ++configuration_generation;
    ++current.sequence;
    portEXIT_CRITICAL(&state_mux);
    next_connect_us.store(0);
    connect_requested.store(false);
    if (worker_task) xTaskNotifyGive(worker_task);
    return nightglass::core::Status::Ok();
}

nightglass::core::Status NetworkWeatherService::clear_credentials() {
    if (credential_mutex &&
        xSemaphoreTake(credential_mutex, portMAX_DELAY) != pdTRUE) {
        return {nightglass::core::StatusCode::invalid_state,
                "credential store busy"};
    }
    wipe(&runtime_credentials, sizeof(runtime_credentials));
    if (wifi_started.load()) esp_wifi_disconnect();
    esp_err_t driver_clear = ESP_OK;
    if (wifi_initialized.load()) {
        wifi_config_t blank{};
        blank.sta.threshold.authmode = WIFI_AUTH_OPEN;
        driver_clear = esp_wifi_set_config(WIFI_IF_STA, &blank);
        wipe(&blank, sizeof(blank));
        if (wifi_started.load()) esp_wifi_stop();
    }
    if (credential_mutex) xSemaphoreGive(credential_mutex);
    portENTER_CRITICAL(&state_mux);
    current.credentials_configured = false;
    current.connected = false;
    current.network_state = current.settings.enabled ? NetworkState::unprovisioned
                                                      : NetworkState::disabled;
    ++configuration_generation;
    ++current.sequence;
    portEXIT_CRITICAL(&state_mux);
    if (worker_task) xTaskNotifyGive(worker_task);
    if (driver_clear != ESP_OK) {
        return {nightglass::core::StatusCode::io_error,
                "Wi-Fi driver credential clearing failed"};
    }
    return nightglass::core::Status::Ok();
}

void NetworkWeatherService::request_refresh() {
    next_fetch_us.store(0);
    weather_failure_attempt.store(0);
    if (worker_task) xTaskNotifyGive(worker_task);
}

nightglass::core::Status NetworkWeatherService::accept_phone_weather(
    std::uint32_t observed_epoch_seconds, std::uint16_t reported_age_seconds,
    WeatherUnits units,
    const DecodedWeather &weather) {
    const auto clock = clock_service().snapshot();
    if (observed_epoch_seconds < 1'577'836'800U ||
        reported_age_seconds > kMaximumProxyAgeSeconds ||
        observed_epoch_seconds < reported_age_seconds) {
        return {nightglass::core::StatusCode::invalid_state,
                "phone weather snapshot outside freshness window"};
    }
    if (units != WeatherUnits::metric && units != WeatherUnits::imperial) {
        return {nightglass::core::StatusCode::invalid_state, "invalid weather units"};
    }
    auto effective_observed = observed_epoch_seconds - reported_age_seconds;
    std::uint32_t age_seconds = reported_age_seconds;
    if (clock.time_valid && clock.utc_epoch_seconds >= 1'577'836'800U) {
        const auto now = static_cast<std::uint32_t>(clock.utc_epoch_seconds);
        std::uint32_t wall_age = 0;
        if (weather_observation_age(effective_observed, now,
                                    kMaximumFutureSkewSeconds, wall_age) &&
            wall_age <= kMaximumProxyAgeSeconds) {
            age_seconds = wall_age;
        } else {
            // The bonded phone delivered this observation over the live encrypted
            // GATT connection. If its wall clock disagrees with the hardware RTC,
            // preserve the phone-reported age and timestamp receipt against the
            // watch clock instead of rejecting current data.
            effective_observed = now - std::min<std::uint32_t>(reported_age_seconds, now);
        }
    }
    if (weather.temperature < -150.0F || weather.temperature > 150.0F ||
        weather.apparent_temperature < -150.0F ||
        weather.apparent_temperature > 150.0F || weather.wind_speed < 0.0F ||
        weather.wind_speed > 500.0F || weather.weather_code > 999) {
        return {nightglass::core::StatusCode::invalid_state,
                "phone weather snapshot outside valid range"};
    }

    NetworkWeatherSnapshot saved{};
    portENTER_CRITICAL(&state_mux);
    if (!current.settings.enabled || !current.settings.location_configured ||
        current.settings.units != units ||
        !weather_candidate_wins(
            current.source == WeatherSource::phone
                ? WeatherCandidateSource::phone
                : current.source == WeatherSource::direct
                      ? WeatherCandidateSource::direct
                      : current.source == WeatherSource::cache
                            ? WeatherCandidateSource::cache
                            : WeatherCandidateSource::none,
            current.observed_epoch_seconds, WeatherCandidateSource::phone,
            effective_observed)) {
        portEXIT_CRITICAL(&state_mux);
        return {nightglass::core::StatusCode::invalid_state,
                "phone weather snapshot does not supersede current data"};
    }
    current.current = weather;
    current.observed_epoch_seconds = effective_observed;
    current.age_seconds = age_seconds;
    current.data_valid = true;
    current.stale = false;
    current.source = WeatherSource::phone;
    current.weather_state = WeatherState::fresh;
    current.last_error = WeatherError::none;
    ++configuration_generation;  // Invalidates any concurrent direct fetch.
    ++current.sequence;
    saved = current;
    portEXIT_CRITICAL(&state_mux);
    last_good_us.store(esp_timer_get_time());
    if (save_cache(saved) != ESP_OK) {
        nightglass::core::health_registry().set(
            "weather", nightglass::core::HealthState::degraded,
            "Phone weather is live but last-good cache persistence failed");
    } else {
        nightglass::core::health_registry().set(
            "weather", nightglass::core::HealthState::ok,
            "Current weather received from bonded phone proxy");
    }
    if (worker_task) xTaskNotifyGive(worker_task);
    return nightglass::core::Status::Ok();
}

bool NetworkWeatherService::prepare_for_light_sleep() {
    sleep_suspended.store(true);
    if (fetch_in_flight.load()) {
        sleep_suspended.store(false);
        return false;
    }
    if (credential_mutex &&
        xSemaphoreTake(credential_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        sleep_suspended.store(false);
        return false;
    }
    if (wifi_started.load()) {
        const esp_err_t stopped = esp_wifi_stop();
        if (stopped != ESP_OK) {
            if (credential_mutex) xSemaphoreGive(credential_mutex);
            sleep_suspended.store(false);
            return false;
        }
    }
    if (credential_mutex) xSemaphoreGive(credential_mutex);
    wifi_started.store(false);
    got_ip.store(false);
    connect_requested.store(false);
    portENTER_CRITICAL(&state_mux);
    current.connected = false;
    if (current.settings.enabled && current.credentials_configured) {
        current.network_state = NetworkState::connecting;
        current.weather_state = current.data_valid ? WeatherState::offline
                                                   : WeatherState::unavailable;
    }
    ++current.sequence;
    portEXIT_CRITICAL(&state_mux);
    return true;
}

void NetworkWeatherService::resume_from_light_sleep() {
    sleep_suspended.store(false);
    next_connect_us.store(0);
    if (worker_task) xTaskNotifyGive(worker_task);
}

NetworkWeatherService &network_weather_service() { return instance; }

}  // namespace nightglass::services

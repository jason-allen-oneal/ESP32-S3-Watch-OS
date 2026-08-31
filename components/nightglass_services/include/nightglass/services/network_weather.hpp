#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "nightglass/core/status.hpp"
#include "nightglass/services/weather_logic.hpp"

namespace nightglass::services {

enum class NetworkState : std::uint8_t {
    disabled,
    unprovisioned,
    connecting,
    online,
    backoff,
    error,
};

enum class WeatherState : std::uint8_t {
    unavailable,
    fetching,
    fresh,
    stale,
    offline,
    error,
};

enum class WeatherError : std::uint8_t {
    none,
    no_location,
    offline,
    transport,
    http_status,
    response_too_large,
    invalid_response,
    persistence,
    time_unavailable,
    stale_payload,
};

enum class WeatherSource : std::uint8_t { none, phone, direct, cache };

struct NetworkWeatherSettings {
    bool enabled{false};
    bool location_configured{false};
    std::int32_t latitude_e6{0};
    std::int32_t longitude_e6{0};
    WeatherUnits units{WeatherUnits::imperial};
    std::uint16_t refresh_minutes{30};
};

static_assert(std::is_trivially_copyable_v<NetworkWeatherSettings>);

struct NetworkWeatherSnapshot {
    std::uint32_t sequence{0};
    NetworkState network_state{NetworkState::disabled};
    WeatherState weather_state{WeatherState::unavailable};
    WeatherError last_error{WeatherError::none};
    NetworkWeatherSettings settings{};
    bool credentials_configured{false};
    bool connected{false};
    bool data_valid{false};
    bool stale{false};
    std::int8_t rssi_dbm{0};
    std::uint8_t reconnect_attempt{0};
    std::uint32_t retry_in_seconds{0};
    std::uint32_t age_seconds{0};
    std::uint32_t observed_epoch_seconds{0};
    WeatherSource source{WeatherSource::none};
    DecodedWeather current{};
};

static_assert(std::is_trivially_copyable_v<NetworkWeatherSnapshot>);

class NetworkWeatherService {
public:
    nightglass::core::Status start();
    [[nodiscard]] NetworkWeatherSnapshot snapshot() const;
    nightglass::core::Status update_settings(const NetworkWeatherSettings &settings);
    nightglass::core::Status provision_credentials(const char *ssid,
                                                   std::size_t ssid_length,
                                                   const char *password,
                                                   std::size_t password_length);
    nightglass::core::Status clear_credentials();
    void request_refresh();
    nightglass::core::Status accept_phone_weather(
        std::uint32_t observed_epoch_seconds, WeatherUnits units,
        const DecodedWeather &weather);
    [[nodiscard]] bool prepare_for_light_sleep();
    void resume_from_light_sleep();
};

NetworkWeatherService &network_weather_service();

}  // namespace nightglass::services

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nightglass::services {

enum class WeatherUnits : std::uint8_t { metric, imperial };

struct DecodedWeather {
    float temperature{0.0F};
    float apparent_temperature{0.0F};
    float wind_speed{0.0F};
    std::uint16_t weather_code{0};
    bool is_day{false};
};

[[nodiscard]] bool valid_coordinates(std::int32_t latitude_e6,
                                     std::int32_t longitude_e6);
[[nodiscard]] bool decode_open_meteo_current(std::string_view json,
                                             DecodedWeather &out);
[[nodiscard]] std::uint32_t reconnect_backoff_seconds(std::uint8_t attempt);
[[nodiscard]] bool weather_is_stale(std::uint32_t age_seconds,
                                    std::uint16_t refresh_minutes);

}  // namespace nightglass::services

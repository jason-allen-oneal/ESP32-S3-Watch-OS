#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nightglass::services {

enum class WeatherUnits : std::uint8_t { metric, imperial };
enum class WeatherCandidateSource : std::uint8_t { none, cache, direct, phone };

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
[[nodiscard]] bool weather_candidate_wins(WeatherCandidateSource current_source,
                                          std::uint32_t current_observed,
                                          WeatherCandidateSource candidate_source,
                                          std::uint32_t candidate_observed);
[[nodiscard]] bool weather_observation_age(std::uint32_t observed_epoch,
                                           std::uint32_t now_epoch,
                                           std::uint32_t maximum_future_skew,
                                           std::uint32_t &age_seconds);
[[nodiscard]] bool weather_cache_is_usable(WeatherUnits cached_units,
                                           WeatherUnits requested_units,
                                           std::uint32_t observed_epoch,
                                           std::uint32_t now_epoch,
                                           std::uint32_t maximum_future_skew);

}  // namespace nightglass::services

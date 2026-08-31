#include <cassert>
#include <string>

#include "nightglass/services/weather_logic.hpp"

using namespace nightglass::services;

int main() {
    assert(valid_coordinates(35'779'600, -78'638'200));
    assert(valid_coordinates(-90'000'000, 180'000'000));
    assert(!valid_coordinates(90'000'001, 0));
    assert(!valid_coordinates(0, -180'000'001));

    constexpr auto response = R"json({
      "latitude":35.78,"longitude":-78.64,
      "current_units":{"temperature_2m":"F"},
      "current":{"time":"2026-08-30T10:00","interval":900,
      "temperature_2m":72.4,"apparent_temperature":74.1,
      "is_day":1,"weather_code":3,"wind_speed_10m":8.7}
    })json";
    DecodedWeather decoded{};
    assert(decode_open_meteo_current(response, decoded));
    assert(decoded.temperature > 72.3F && decoded.temperature < 72.5F);
    assert(decoded.apparent_temperature > 74.0F);
    assert(decoded.wind_speed > 8.6F && decoded.wind_speed < 8.8F);
    assert(decoded.weather_code == 3);
    assert(decoded.is_day);

    assert(!decode_open_meteo_current("{}", decoded));
    assert(!decode_open_meteo_current(
        R"({"current":{"temperature_2m":999,"apparent_temperature":1,"is_day":0,"weather_code":0,"wind_speed_10m":0}})", decoded));
    assert(!decode_open_meteo_current(
        R"({"current":{"temperature_2m":1,"apparent_temperature":1,"is_day":2,"weather_code":0,"wind_speed_10m":0}})", decoded));
    assert(!decode_open_meteo_current(
        R"({"current":{"temperature_2m":1,"temperature_2m":2,"apparent_temperature":1,"is_day":1,"weather_code":0,"wind_speed_10m":0}})", decoded));
    assert(decode_open_meteo_current(
        R"({"current":{"note":"temperature_2m","temperature_2m":1,"apparent_temperature":1,"is_day":1,"weather_code":0,"wind_speed_10m":0}})", decoded));
    assert(decoded.temperature == 1.0F);
    assert(!decode_open_meteo_current(std::string(4097, 'x'), decoded));

    assert(reconnect_backoff_seconds(0) == 1);
    assert(reconnect_backoff_seconds(1) == 2);
    assert(reconnect_backoff_seconds(8) == 256);
    assert(reconnect_backoff_seconds(9) == 256);
    assert(!weather_is_stale(3599, 30));
    assert(!weather_is_stale(3600, 30));
    assert(weather_is_stale(3601, 30));

    assert(weather_candidate_wins(WeatherCandidateSource::none, 0,
                                  WeatherCandidateSource::phone, 100));
    assert(weather_candidate_wins(WeatherCandidateSource::direct, 100,
                                  WeatherCandidateSource::phone, 101));
    assert(!weather_candidate_wins(WeatherCandidateSource::phone, 101,
                                   WeatherCandidateSource::direct, 100));
    assert(weather_candidate_wins(WeatherCandidateSource::direct, 100,
                                  WeatherCandidateSource::phone, 100));
    assert(!weather_candidate_wins(WeatherCandidateSource::phone, 100,
                                   WeatherCandidateSource::direct, 100));
    std::uint32_t age = 99;
    assert(weather_observation_age(100, 120, 5, age) && age == 20);
    assert(weather_observation_age(123, 120, 5, age) && age == 0);
    assert(!weather_observation_age(126, 120, 5, age));
    assert(!weather_observation_age(0, 120, 5, age));
    assert(weather_cache_is_usable(WeatherUnits::metric, WeatherUnits::metric,
                                   100, 120, 5));
    assert(!weather_cache_is_usable(WeatherUnits::imperial, WeatherUnits::metric,
                                    100, 120, 5));
    assert(!weather_cache_is_usable(WeatherUnits::metric, WeatherUnits::metric,
                                    126, 120, 5));
    assert(weather_icon_for_code(0, true) == WeatherIcon::clear_day);
    assert(weather_icon_for_code(0, false) == WeatherIcon::clear_night);
    assert(weather_icon_for_code(2, true) == WeatherIcon::partly_cloudy);
    assert(weather_icon_for_code(3, true) == WeatherIcon::cloudy);
    assert(weather_icon_for_code(45, true) == WeatherIcon::fog);
    assert(weather_icon_for_code(61, true) == WeatherIcon::rain);
    assert(weather_icon_for_code(75, true) == WeatherIcon::snow);
    assert(weather_icon_for_code(95, true) == WeatherIcon::storm);
}

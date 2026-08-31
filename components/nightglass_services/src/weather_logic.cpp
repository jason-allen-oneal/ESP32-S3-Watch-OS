#include "nightglass/services/weather_logic.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace nightglass::services {
namespace {

bool find_number(std::string_view object, std::string_view key, double &value) {
    if (object.size() > 4096 || key.empty()) return false;
    std::size_t key_start = std::string_view::npos;
    std::size_t colon = std::string_view::npos;
    std::size_t search_from = 0;
    while (true) {
        const auto candidate = object.find(key, search_from);
        if (candidate == std::string_view::npos) break;
        search_from = candidate + key.size();
        if (candidate == 0 || candidate + key.size() >= object.size() ||
            object[candidate - 1] != '"' || object[candidate + key.size()] != '"') {
            continue;
        }
        std::size_t separator = candidate + key.size() + 1;
        while (separator < object.size() &&
               (object[separator] == ' ' || object[separator] == '\t' ||
                object[separator] == '\r' || object[separator] == '\n')) {
            ++separator;
        }
        if (separator >= object.size() || object[separator] != ':') continue;
        if (key_start != std::string_view::npos) return false;  // Duplicate key.
        key_start = candidate;
        colon = separator;
    }
    if (key_start == std::string_view::npos) return false;
    std::size_t start = colon + 1;
    while (start < object.size() &&
           (object[start] == ' ' || object[start] == '\t' || object[start] == '\r' ||
            object[start] == '\n')) {
        ++start;
    }
    if (start >= object.size()) return false;

    // strtod needs a terminator. Open-Meteo values are short; bounding this copy
    // rejects pathological responses without allocating.
    char number[40]{};
    std::size_t length = 0;
    while (start + length < object.size() && length + 1 < sizeof(number)) {
        const char c = object[start + length];
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
              c == 'e' || c == 'E')) {
            break;
        }
        number[length++] = c;
    }
    if (start + length < object.size()) {
        const char next = object[start + length];
        if ((next >= '0' && next <= '9') || next == '-' || next == '+' ||
            next == '.' || next == 'e' || next == 'E') {
            return false;
        }
    }
    if (length == 0) return false;
    char *end = nullptr;
    errno = 0;
    value = std::strtod(number, &end);
    return errno == 0 && end == number + length && std::isfinite(value);
}

}  // namespace

bool valid_coordinates(std::int32_t latitude_e6, std::int32_t longitude_e6) {
    return latitude_e6 >= -90'000'000 && latitude_e6 <= 90'000'000 &&
           longitude_e6 >= -180'000'000 && longitude_e6 <= 180'000'000;
}

bool decode_open_meteo_current(std::string_view json, DecodedWeather &out) {
    if (json.empty() || json.size() > 4096) return false;
    const auto current_key = json.find("\"current\"");
    if (current_key == std::string_view::npos) return false;
    const auto object_start = json.find('{', current_key + 9);
    const auto object_end = object_start == std::string_view::npos
                                ? std::string_view::npos
                                : json.find('}', object_start + 1);
    if (object_start == std::string_view::npos || object_end == std::string_view::npos) {
        return false;
    }
    const auto current = json.substr(object_start, object_end - object_start + 1);

    double temperature = 0.0;
    double apparent = 0.0;
    double wind = 0.0;
    double code = 0.0;
    double is_day = 0.0;
    if (!find_number(current, "temperature_2m", temperature) ||
        !find_number(current, "apparent_temperature", apparent) ||
        !find_number(current, "wind_speed_10m", wind) ||
        !find_number(current, "weather_code", code) ||
        !find_number(current, "is_day", is_day)) {
        return false;
    }
    if (temperature < -150.0 || temperature > 150.0 || apparent < -150.0 ||
        apparent > 150.0 || wind < 0.0 || wind > 500.0 || code < 0.0 ||
        code > 999.0 || std::floor(code) != code ||
        (is_day != 0.0 && is_day != 1.0)) {
        return false;
    }
    out.temperature = static_cast<float>(temperature);
    out.apparent_temperature = static_cast<float>(apparent);
    out.wind_speed = static_cast<float>(wind);
    out.weather_code = static_cast<std::uint16_t>(code);
    out.is_day = is_day == 1.0;
    return true;
}

std::uint32_t reconnect_backoff_seconds(std::uint8_t attempt) {
    if (attempt == 0) return 1;
    const std::uint8_t shift = attempt > 8 ? 8 : attempt;
    const std::uint32_t seconds = 1U << shift;
    return seconds > 300U ? 300U : seconds;
}

bool weather_is_stale(std::uint32_t age_seconds, std::uint16_t refresh_minutes) {
    const std::uint32_t refresh_seconds =
        static_cast<std::uint32_t>(refresh_minutes) * 60U;
    return refresh_seconds == 0 || age_seconds > refresh_seconds * 2U;
}

bool weather_candidate_wins(WeatherCandidateSource current_source,
                            std::uint32_t current_observed,
                            WeatherCandidateSource candidate_source,
                            std::uint32_t candidate_observed) {
    if (candidate_source == WeatherCandidateSource::none || candidate_observed == 0) {
        return false;
    }
    if (current_source == WeatherCandidateSource::none || current_observed == 0) return true;
    if (candidate_observed > current_observed) return true;
    if (candidate_observed < current_observed) return false;
    return static_cast<std::uint8_t>(candidate_source) >
           static_cast<std::uint8_t>(current_source);
}

bool weather_observation_age(std::uint32_t observed_epoch, std::uint32_t now_epoch,
                             std::uint32_t maximum_future_skew,
                             std::uint32_t &age_seconds) {
    age_seconds = 0;
    if (observed_epoch == 0 || now_epoch == 0 ||
        observed_epoch > now_epoch + maximum_future_skew) {
        return false;
    }
    age_seconds = observed_epoch > now_epoch ? 0 : now_epoch - observed_epoch;
    return true;
}

bool weather_cache_is_usable(WeatherUnits cached_units, WeatherUnits requested_units,
                             std::uint32_t observed_epoch, std::uint32_t now_epoch,
                             std::uint32_t maximum_future_skew) {
    std::uint32_t ignored_age = 0;
    return cached_units == requested_units &&
           weather_observation_age(observed_epoch, now_epoch, maximum_future_skew,
                                   ignored_age);
}

WeatherIcon weather_icon_for_code(std::uint16_t code, bool is_day) {
    if (code == 0) return is_day ? WeatherIcon::clear_day : WeatherIcon::clear_night;
    if (code <= 2) return WeatherIcon::partly_cloudy;
    if (code == 3) return WeatherIcon::cloudy;
    if (code == 45 || code == 48) return WeatherIcon::fog;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        return WeatherIcon::rain;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return WeatherIcon::snow;
    }
    if (code >= 95 && code <= 99) return WeatherIcon::storm;
    return WeatherIcon::cloudy;
}

}  // namespace nightglass::services

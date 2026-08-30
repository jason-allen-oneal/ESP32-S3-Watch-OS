#include "nightglass/services/time_math.hpp"

namespace nightglass::services {
namespace {

constexpr bool leap(std::int32_t year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

constexpr std::uint8_t days_in_month(std::int32_t year, std::uint8_t month) noexcept {
    constexpr std::uint8_t days[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap(year) ? 29 : days[month - 1];
}

// Howard Hinnant's civil calendar algorithms, shifted to the Unix epoch.
constexpr std::int64_t days_from_civil(std::int32_t year, unsigned month,
                                       unsigned day) noexcept {
    year -= month <= 2;
    const auto era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const auto doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}

constexpr CivilTime civil_from_days(std::int64_t days) noexcept {
    days += 719468;
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    auto year = static_cast<std::int32_t>(yoe) + static_cast<std::int32_t>(era) * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    const auto day = doy - (153 * mp + 2) / 5 + 1;
    const auto month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;
    CivilTime result{};
    result.year = year;
    result.month = static_cast<std::uint8_t>(month);
    result.day = static_cast<std::uint8_t>(day);
    return result;
}

}  // namespace

bool valid_civil(const CivilTime &value) noexcept {
    return value.year >= 2000 && value.year <= 2199 && value.month >= 1 && value.month <= 12 &&
           value.day >= 1 && value.day <= days_in_month(value.year, value.month) &&
           value.hour <= 23 && value.minute <= 59 && value.second <= 59;
}

std::int64_t civil_to_epoch(const CivilTime &value) noexcept {
    return days_from_civil(value.year, value.month, value.day) * 86400 +
           static_cast<std::int64_t>(value.hour) * 3600 +
           static_cast<std::int64_t>(value.minute) * 60 + value.second;
}

CivilTime epoch_to_civil(std::int64_t epoch_seconds) noexcept {
    auto days = epoch_seconds / 86400;
    auto seconds = epoch_seconds % 86400;
    if (seconds < 0) {
        seconds += 86400;
        --days;
    }
    auto result = civil_from_days(days);
    result.weekday = static_cast<std::uint8_t>((days + 4) % 7);
    if (static_cast<std::int8_t>(result.weekday) < 0) result.weekday += 7;
    result.hour = static_cast<std::uint8_t>(seconds / 3600);
    result.minute = static_cast<std::uint8_t>((seconds % 3600) / 60);
    result.second = static_cast<std::uint8_t>(seconds % 60);
    return result;
}

CivilTime apply_utc_offset(const CivilTime &utc, std::int16_t offset_minutes) noexcept {
    return epoch_to_civil(civil_to_epoch(utc) + static_cast<std::int64_t>(offset_minutes) * 60);
}

}  // namespace nightglass::services

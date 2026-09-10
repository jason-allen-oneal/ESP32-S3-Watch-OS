#include "nightglass/services/activity_units.hpp"

#include <cstdio>

namespace nightglass::services {
namespace {
constexpr std::uint64_t kMmPerMile = 1'609'344;
constexpr std::uint16_t kMinimumStepLengthMm = 300;
constexpr std::uint16_t kMaximumStepLengthMm = 1'500;

void format_hundredths(char *buffer, std::size_t size, std::uint64_t hundredths,
                       const char *unit) {
    std::snprintf(buffer, size, "%llu.%02llu %s",
                  static_cast<unsigned long long>(hundredths / 100U),
                  static_cast<unsigned long long>(hundredths % 100U), unit);
}
}

void format_activity_distance(char *buffer, std::size_t size,
                              std::uint64_t distance_mm, ActivityUnits units) {
    if (!buffer || size == 0) return;
    if (units == ActivityUnits::metric) {
        if (distance_mm < 1'000'000) {
            std::snprintf(buffer, size, "%llu m",
                          static_cast<unsigned long long>((distance_mm + 500) / 1000));
        } else {
            format_hundredths(buffer, size, (distance_mm + 5'000U) / 10'000U, "km");
        }
        return;
    }
    if (distance_mm < kMmPerMile / 10) {
        std::snprintf(buffer, size, "%llu ft",
                      static_cast<unsigned long long>((distance_mm * 10 + 1'524) / 3'048));
    } else {
        auto whole_miles = distance_mm / kMmPerMile;
        const auto remainder_mm = distance_mm % kMmPerMile;
        auto hundredths = (remainder_mm * 100U + kMmPerMile / 2U) / kMmPerMile;
        if (hundredths == 100U) {
            ++whole_miles;
            hundredths = 0;
        }
        format_hundredths(buffer, size, whole_miles * 100U + hundredths, "mi");
    }
}

void format_step_length(char *buffer, std::size_t size,
                        std::uint16_t step_length_mm, ActivityUnits units) {
    if (!buffer || size == 0) return;
    if (units == ActivityUnits::metric) {
        const auto hundredths = (static_cast<unsigned>(step_length_mm) + 5U) / 10U;
        std::snprintf(buffer, size, "STEP LENGTH  %u.%02u m", hundredths / 100U,
                      hundredths % 100U);
    } else {
        const auto tenths = (static_cast<unsigned>(step_length_mm) * 100U + 127U) / 254U;
        std::snprintf(buffer, size, "STEP LENGTH  %u.%01u in", tenths / 10U,
                      tenths % 10U);
    }
}

std::uint16_t next_step_length(std::uint16_t step_length_mm, ActivityUnits units) {
    if (units == ActivityUnits::metric) {
        const auto next = static_cast<std::uint16_t>(step_length_mm + 50);
        return next > kMaximumStepLengthMm ? kMinimumStepLengthMm : next;
    }
    // Half-inch increments keep the displayed value and each tap aligned.
    constexpr unsigned kMinimumHalfInches = 24;   // 12.0 in
    constexpr unsigned kMaximumHalfInches = 118;  // 59.0 in
    const auto current_half_inches = static_cast<unsigned>(
        (static_cast<std::uint32_t>(step_length_mm) * 10U + 63U) / 127U);
    const auto next_half_inches = current_half_inches >= kMaximumHalfInches
                                      ? kMinimumHalfInches
                                      : current_half_inches + 1U;
    return static_cast<std::uint16_t>((next_half_inches * 127U + 5U) / 10U);
}

}  // namespace nightglass::services

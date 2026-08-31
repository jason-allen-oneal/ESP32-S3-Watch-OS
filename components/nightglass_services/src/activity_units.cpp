#include "nightglass/services/activity_units.hpp"

#include <cstdio>

namespace nightglass::services {
namespace {
constexpr std::uint64_t kMmPerMile = 1'609'344;
constexpr std::uint16_t kMinimumStepLengthMm = 300;
constexpr std::uint16_t kMaximumStepLengthMm = 1'500;
}

void format_activity_distance(char *buffer, std::size_t size,
                              std::uint64_t distance_mm, ActivityUnits units) {
    if (!buffer || size == 0) return;
    if (units == ActivityUnits::metric) {
        if (distance_mm < 1'000'000) {
            std::snprintf(buffer, size, "%llu m",
                          static_cast<unsigned long long>((distance_mm + 500) / 1000));
        } else {
            std::snprintf(buffer, size, "%.2f km", distance_mm / 1'000'000.0);
        }
        return;
    }
    if (distance_mm < kMmPerMile / 10) {
        std::snprintf(buffer, size, "%llu ft",
                      static_cast<unsigned long long>((distance_mm * 10 + 1'524) / 3'048));
    } else {
        std::snprintf(buffer, size, "%.2f mi", distance_mm /
                      static_cast<double>(kMmPerMile));
    }
}

void format_step_length(char *buffer, std::size_t size,
                        std::uint16_t step_length_mm, ActivityUnits units) {
    if (!buffer || size == 0) return;
    if (units == ActivityUnits::metric) {
        std::snprintf(buffer, size, "STEP LENGTH  %.2f m", step_length_mm / 1000.0);
    } else {
        std::snprintf(buffer, size, "STEP LENGTH  %.1f in", step_length_mm / 25.4);
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

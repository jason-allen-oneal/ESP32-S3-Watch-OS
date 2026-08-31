#include "nightglass/services/activity_units.hpp"

#include <cstdio>

namespace nightglass::services {
namespace {
constexpr std::uint64_t kMmPerMile = 1'609'344;
constexpr std::uint16_t kMinimumStrideMm = 300;
constexpr std::uint16_t kMaximumStrideMm = 1'500;
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

void format_stride_length(char *buffer, std::size_t size,
                          std::uint16_t stride_mm, ActivityUnits units) {
    if (!buffer || size == 0) return;
    if (units == ActivityUnits::metric) {
        std::snprintf(buffer, size, "STRIDE  %.2f m", stride_mm / 1000.0);
    } else {
        std::snprintf(buffer, size, "STRIDE  %.1f in", stride_mm / 25.4);
    }
}

std::uint16_t next_stride_length(std::uint16_t stride_mm, ActivityUnits units) {
    if (units == ActivityUnits::metric) {
        const auto next = static_cast<std::uint16_t>(stride_mm + 50);
        return next > kMaximumStrideMm ? kMinimumStrideMm : next;
    }
    const auto current_inches = static_cast<unsigned>((stride_mm + 13) / 25.4);
    const auto next_inches = current_inches >= 59 ? 12U : current_inches + 1U;
    return static_cast<std::uint16_t>(next_inches * 25.4 + 0.5);
}

}  // namespace nightglass::services

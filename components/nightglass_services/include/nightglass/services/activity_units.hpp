#pragma once

#include <cstddef>
#include <cstdint>

namespace nightglass::services {

enum class ActivityUnits : std::uint8_t { imperial, metric };

// Formats a measured distance without hiding its unit. Short distances use
// feet/metres; longer distances use miles/kilometres with two decimals.
void format_activity_distance(char *buffer, std::size_t size,
                              std::uint64_t distance_mm, ActivityUnits units);
void format_stride_length(char *buffer, std::size_t size,
                          std::uint16_t stride_mm, ActivityUnits units);
[[nodiscard]] std::uint16_t next_stride_length(std::uint16_t stride_mm,
                                                ActivityUnits units);

}  // namespace nightglass::services

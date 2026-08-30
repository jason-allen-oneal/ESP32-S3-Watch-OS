#pragma once

#include <cstdint>

namespace nightglass::services {

struct CivilTime {
    std::int32_t year{1970};
    std::uint8_t month{1};
    std::uint8_t day{1};
    std::uint8_t weekday{4};
    std::uint8_t hour{0};
    std::uint8_t minute{0};
    std::uint8_t second{0};
};

[[nodiscard]] bool valid_civil(const CivilTime &value) noexcept;
[[nodiscard]] std::int64_t civil_to_epoch(const CivilTime &value) noexcept;
[[nodiscard]] CivilTime epoch_to_civil(std::int64_t epoch_seconds) noexcept;
[[nodiscard]] CivilTime apply_utc_offset(const CivilTime &utc,
                                         std::int16_t offset_minutes) noexcept;

}  // namespace nightglass::services

#pragma once

#include <cstdint>

namespace nightglass::services {

struct ActivityDayState {
    std::int64_t tracked_local_day{0};
    std::int64_t fallback_rollover_us{0};
};

struct ActivityDayUpdate {
    bool reset_count{false};
    bool date_valid{false};
    std::int64_t local_day{0};
};

// Advances daily state without RTOS or persistence dependencies. A valid wall
// clock is authoritative, but is never allowed to roll counts backward. When
// wall time is unavailable, a monotonic 24-hour boundary keeps long-running
// sessions bounded until the RTC becomes trustworthy again.
ActivityDayUpdate advance_activity_day(ActivityDayState &state, bool wall_time_valid,
                                        std::int64_t local_day,
                                        std::uint32_t local_second_of_day,
                                        std::int64_t now_us) noexcept;

}  // namespace nightglass::services

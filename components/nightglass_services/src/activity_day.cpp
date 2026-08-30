#include "nightglass/services/activity_day.hpp"

#include <algorithm>

namespace nightglass::services {
namespace {
constexpr std::int64_t kDayUs = 86'400LL * 1'000'000LL;
}

ActivityDayUpdate advance_activity_day(ActivityDayState &state, bool wall_time_valid,
                                        std::int64_t local_day,
                                        std::uint32_t local_second_of_day,
                                        std::int64_t now_us) noexcept {
    ActivityDayUpdate result{};
    if (state.fallback_rollover_us <= 0) state.fallback_rollover_us = now_us + kDayUs;

    if (wall_time_valid && local_second_of_day < 86'400U) {
        if (state.tracked_local_day == 0) {
            state.tracked_local_day = local_day;
        } else if (local_day > state.tracked_local_day) {
            state.tracked_local_day = local_day;
            result.reset_count = true;
        } else if (local_day < state.tracked_local_day) {
            // A bad RTC or timezone edit must not resurrect/reset an older day.
            result.local_day = state.tracked_local_day;
            return result;
        }
        const auto seconds_to_midnight = std::max<std::uint32_t>(1U, 86'400U - local_second_of_day);
        state.fallback_rollover_us =
            now_us + static_cast<std::int64_t>(seconds_to_midnight) * 1'000'000LL;
        result.date_valid = true;
        result.local_day = state.tracked_local_day;
        return result;
    }

    if (now_us >= state.fallback_rollover_us) {
        const auto elapsed_days = 1 + (now_us - state.fallback_rollover_us) / kDayUs;
        state.fallback_rollover_us += elapsed_days * kDayUs;
        // Do not fabricate a civil date from uptime. The monotonic boundary
        // may reset the count, but only valid wall time may advance the
        // persisted calendar key.
        result.reset_count = true;
    }
    result.local_day = state.tracked_local_day;
    return result;
}

}  // namespace nightglass::services

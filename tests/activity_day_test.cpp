#include <cassert>

#include "nightglass/services/activity_day.hpp"

using nightglass::services::ActivityDayState;
using nightglass::services::advance_activity_day;

int main() {
    ActivityDayState state{};
    auto update = advance_activity_day(state, false, 0, 0, 1'000'000);
    assert(!update.reset_count && !update.date_valid && update.local_day == 0);

    update = advance_activity_day(state, true, 20'000, 3600, 2'000'000);
    assert(!update.reset_count && update.date_valid && update.local_day == 20'000);

    update = advance_activity_day(state, true, 20'001, 0, 3'000'000);
    assert(update.reset_count && update.date_valid && update.local_day == 20'001);

    update = advance_activity_day(state, true, 19'999, 0, 4'000'000);
    assert(!update.reset_count && !update.date_valid && update.local_day == 20'001);

    ActivityDayState fallback{};
    advance_activity_day(fallback, false, 0, 0, 10);
    constexpr std::int64_t day_us = 86'400LL * 1'000'000LL;
    update = advance_activity_day(fallback, false, 0, 0, 10 + day_us - 1);
    assert(!update.reset_count);
    update = advance_activity_day(fallback, false, 0, 0, 10 + day_us);
    assert(update.reset_count && !update.date_valid);
    update = advance_activity_day(fallback, false, 0, 0, 10 + 3 * day_us);
    assert(update.reset_count);
    return 0;
}

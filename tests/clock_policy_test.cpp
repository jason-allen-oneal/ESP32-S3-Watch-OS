#include <cassert>

#include "nightglass/services/clock.hpp"

int main() {
    using namespace nightglass::services;
    AlarmSettings alarm{};
    assert(valid_alarm_settings(alarm));
    alarm.repeat_days = kWeekdayMask;
    assert(alarm_runs_on_weekday(alarm, 1) && alarm_runs_on_weekday(alarm, 5));
    assert(!alarm_runs_on_weekday(alarm, 0) && !alarm_runs_on_weekday(alarm, 6));
    alarm.repeat_days = kWeekendMask;
    assert(alarm_runs_on_weekday(alarm, 0) && alarm_runs_on_weekday(alarm, 6));
    alarm.repeat_days = 0;
    assert(!valid_alarm_settings(alarm));
    alarm = {};
    alarm.label.back() = 'x';
    assert(!valid_alarm_settings(alarm));

    QuietHoursSettings quiet{true, 22 * 60, 7 * 60};
    assert(valid_quiet_hours(quiet));
    assert(quiet_hours_active(quiet, 23 * 60));
    assert(quiet_hours_active(quiet, 6 * 60 + 59));
    assert(!quiet_hours_active(quiet, 12 * 60));
    quiet.start_minute = 9 * 60;
    quiet.end_minute = 17 * 60;
    assert(quiet_hours_active(quiet, 12 * 60));
    assert(!quiet_hours_active(quiet, 18 * 60));
    quiet.enabled = false;
    assert(!quiet_hours_active(quiet, 12 * 60));
    return 0;
}

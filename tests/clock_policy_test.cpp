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

    alarm = {};
    alarm.enabled = true;
    alarm.hour = 7;
    alarm.minute = 30;
    alarm.repeat_days = kWeekdayMask;
    constexpr std::int64_t day = 20'000;
    const auto target = day * 86400 + 7 * 3600 + 30 * 60;
    assert(alarm_due_in_window(alarm, 1, target + 47, day - 1));
    assert(!alarm_due_in_window(alarm, 1, target + 47, day));
    assert(!alarm_due_in_window(alarm, 1, target + 5 * 60, day - 1));
    assert(!alarm_due_in_window(alarm, 0, target + 1, day - 1));

    std::array<AlarmSettings, kAlarmCapacity> alarms{};
    alarms[0] = alarm;
    alarms[0].hour = 9;
    alarms[1] = alarm;
    alarms[1].hour = 8;
    assert(next_alarm_index(alarms, day * 86400 + 7 * 3600, 1) == 1);
    alarms[1].enabled = false;
    assert(next_alarm_index(alarms, day * 86400 + 7 * 3600, 1) == 0);
    assert(next_alarm_index(alarms, day * 86400 + 10 * 3600, 1) == 0);
    alarms[0].enabled = false;
    assert(next_alarm_index(alarms, day * 86400 + 7 * 3600, 1) == kNoAlarmIndex);
    return 0;
}

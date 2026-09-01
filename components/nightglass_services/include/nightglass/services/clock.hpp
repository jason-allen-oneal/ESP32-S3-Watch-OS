#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "nightglass/core/status.hpp"
#include "nightglass/services/time_math.hpp"

namespace nightglass::services {

enum class AlertKind : std::uint8_t { none, alarm, countdown, both };

struct ClockSettings {
    bool use_24_hour{true};
    bool daylight_saving{false};
    std::int16_t utc_offset_minutes{0};
};

struct AlarmSettings {
    bool enabled{false};
    std::uint8_t hour{7};
    std::uint8_t minute{0};
    // Bit 0 is Sunday through bit 6 Saturday. Zero intentionally means no
    // scheduled weekdays and is therefore rejected by valid_alarm_settings().
    std::uint8_t repeat_days{0x7f};
    std::array<char, 13> label{"Wake up"};
};

enum class AlarmAdjustment : std::uint8_t {
    hour_forward,
    hour_backward,
    minute_forward,
    minute_backward,
    toggle_enabled,
    cycle_repeat,
    cycle_label,
};

inline constexpr std::size_t kAlarmCapacity = 4;
inline constexpr std::uint8_t kEveryDayMask = 0x7f;
inline constexpr std::uint8_t kWeekdayMask = 0x3e;
inline constexpr std::uint8_t kWeekendMask = 0x41;
inline constexpr std::uint8_t kNoAlarmIndex = 0xff;

struct QuietHoursSettings {
    bool enabled{false};
    std::uint16_t start_minute{22 * 60};
    std::uint16_t end_minute{7 * 60};
};

constexpr bool valid_alarm_settings(const AlarmSettings &alarm) noexcept {
    if (alarm.hour > 23 || alarm.minute > 59 || alarm.repeat_days == 0 ||
        (alarm.repeat_days & ~kEveryDayMask) != 0 || alarm.label.back() != '\0') {
        return false;
    }
    return true;
}

constexpr bool alarm_label_is(const AlarmSettings &alarm, const char *label) noexcept {
    std::size_t index = 0;
    for (; index < alarm.label.size() && label[index] != '\0'; ++index) {
        if (alarm.label[index] != label[index]) return false;
    }
    return index < alarm.label.size() && alarm.label[index] == '\0' && label[index] == '\0';
}

constexpr void set_alarm_label(AlarmSettings &alarm, const char *label) noexcept {
    alarm.label.fill('\0');
    for (std::size_t index = 0;
         index + 1 < alarm.label.size() && label[index] != '\0'; ++index) {
        alarm.label[index] = label[index];
    }
}

// Apply mutations to the worker-owned alarm state. UI taps enqueue these
// operations rather than resending an asynchronously stale absolute snapshot.
constexpr AlarmSettings adjusted_alarm(AlarmSettings alarm,
                                        AlarmAdjustment adjustment) noexcept {
    switch (adjustment) {
        case AlarmAdjustment::hour_forward:
            alarm.hour = static_cast<std::uint8_t>((alarm.hour + 1U) % 24U);
            break;
        case AlarmAdjustment::hour_backward:
            alarm.hour = static_cast<std::uint8_t>((alarm.hour + 23U) % 24U);
            break;
        case AlarmAdjustment::minute_forward:
            alarm.minute = static_cast<std::uint8_t>((alarm.minute + 5U) % 60U);
            break;
        case AlarmAdjustment::minute_backward:
            alarm.minute = static_cast<std::uint8_t>((alarm.minute + 55U) % 60U);
            break;
        case AlarmAdjustment::toggle_enabled:
            alarm.enabled = !alarm.enabled;
            break;
        case AlarmAdjustment::cycle_repeat:
            alarm.repeat_days = alarm.repeat_days == kEveryDayMask
                                    ? kWeekdayMask
                                    : alarm.repeat_days == kWeekdayMask
                                          ? kWeekendMask : kEveryDayMask;
            break;
        case AlarmAdjustment::cycle_label:
            if (alarm_label_is(alarm, "Wake up")) set_alarm_label(alarm, "Work");
            else if (alarm_label_is(alarm, "Work")) set_alarm_label(alarm, "Medication");
            else if (alarm_label_is(alarm, "Medication")) set_alarm_label(alarm, "Exercise");
            else set_alarm_label(alarm, "Wake up");
            break;
    }
    return alarm;
}

constexpr bool restored_snooze_valid(std::int64_t deadline_utc,
                                     std::int64_t now_utc,
                                     std::uint8_t alarm_index,
                                     bool alarm_enabled,
                                     std::int64_t max_future_seconds = 60 * 60) noexcept {
    return alarm_index < kAlarmCapacity && alarm_enabled && deadline_utc > now_utc &&
           max_future_seconds > 0 && deadline_utc - now_utc <= max_future_seconds;
}

constexpr bool valid_quiet_hours(const QuietHoursSettings &quiet) noexcept {
    return quiet.start_minute < 24 * 60 && quiet.end_minute < 24 * 60;
}

constexpr bool quiet_hours_active(const QuietHoursSettings &quiet,
                                  std::uint16_t minute_of_day) noexcept {
    if (!quiet.enabled || minute_of_day >= 24 * 60 ||
        quiet.start_minute == quiet.end_minute) return false;
    return quiet.start_minute < quiet.end_minute
               ? minute_of_day >= quiet.start_minute && minute_of_day < quiet.end_minute
               : minute_of_day >= quiet.start_minute || minute_of_day < quiet.end_minute;
}

constexpr bool alarm_runs_on_weekday(const AlarmSettings &alarm,
                                     std::uint8_t weekday) noexcept {
    return weekday < 7 && (alarm.repeat_days & (1U << weekday)) != 0;
}

// Accept a bounded late observation so a 100 ms worker, RTC refresh, or a
// brief light-sleep interval cannot miss an alarm merely because second 0/1
// was not sampled. last_fired_day preserves the once-per-local-day contract.
constexpr bool alarm_due_in_window(const AlarmSettings &alarm,
                                   std::uint8_t weekday,
                                   std::int64_t local_epoch_seconds,
                                   std::int64_t last_fired_day,
                                   std::uint32_t late_window_seconds = 5 * 60) noexcept {
    if (!alarm.enabled || !alarm_runs_on_weekday(alarm, weekday) ||
        local_epoch_seconds < 0 || late_window_seconds == 0) return false;
    const auto day = local_epoch_seconds / 86400;
    if (day == last_fired_day) return false;
    const auto target = day * 86400 + static_cast<std::int64_t>(alarm.hour) * 3600 +
                        static_cast<std::int64_t>(alarm.minute) * 60;
    return local_epoch_seconds >= target &&
           local_epoch_seconds - target < static_cast<std::int64_t>(late_window_seconds);
}

struct ClockSnapshot {
    std::uint32_t sequence{0};
    bool time_valid{false};
    std::int64_t utc_epoch_seconds{0};
    CivilTime local_time{};
    ClockSettings settings{};
    std::array<AlarmSettings, kAlarmCapacity> alarms{};
    // Legacy slot-zero view retained for existing faces and callers.
    AlarmSettings alarm{};
    QuietHoursSettings quiet_hours{};
    bool quiet_hours_active{false};
    bool alarm_ringing{false};
    std::uint8_t ringing_alarm_index{kNoAlarmIndex};
    bool alarm_snoozed{false};
    std::uint16_t snooze_minutes{10};
    std::uint32_t timer_configured_seconds{300};
    std::uint32_t timer_remaining_seconds{300};
    bool timer_running{false};
    bool timer_ringing{false};
    std::uint64_t stopwatch_elapsed_ms{0};
    bool stopwatch_running{false};
    bool persistence_ok{true};
};

constexpr std::uint8_t next_alarm_index(
    const std::array<AlarmSettings, kAlarmCapacity> &alarms,
    std::int64_t local_epoch_seconds, std::uint8_t weekday) noexcept {
    if (local_epoch_seconds < 0 || weekday >= 7) return kNoAlarmIndex;
    const auto day_start = (local_epoch_seconds / 86400) * 86400;
    std::int64_t best = std::numeric_limits<std::int64_t>::max();
    std::uint8_t selected = kNoAlarmIndex;
    for (std::size_t index = 0; index < alarms.size(); ++index) {
        const auto &alarm = alarms[index];
        if (!alarm.enabled) continue;
        for (std::uint8_t offset = 0; offset <= 7; ++offset) {
            if (!alarm_runs_on_weekday(alarm,
                                       static_cast<std::uint8_t>((weekday + offset) % 7))) {
                continue;
            }
            const auto target = day_start + static_cast<std::int64_t>(offset) * 86400 +
                                static_cast<std::int64_t>(alarm.hour) * 3600 +
                                static_cast<std::int64_t>(alarm.minute) * 60;
            if (target < local_epoch_seconds) continue;
            if (target < best) {
                best = target;
                selected = static_cast<std::uint8_t>(index);
            }
            break;
        }
    }
    return selected;
}

static_assert(std::is_trivially_copyable_v<ClockSnapshot>);

class ClockService {
public:
    nightglass::core::Status start();
    [[nodiscard]] ClockSnapshot snapshot() const;
    [[nodiscard]] AlertKind active_alert() const;
    [[nodiscard]] std::int64_t next_wake_delay_us() const;

    bool update_clock_settings(const ClockSettings &settings);
    bool update_alarm(const AlarmSettings &alarm);
    bool update_alarm(std::size_t index, const AlarmSettings &alarm);
    bool adjust_alarm(std::size_t index, AlarmAdjustment adjustment);
    bool update_quiet_hours(const QuietHoursSettings &settings);
    bool snooze_alarm(std::uint16_t minutes = 10);
    bool set_timer_duration(std::uint32_t seconds);
    bool toggle_timer();
    bool reset_timer();
    bool toggle_stopwatch();
    bool reset_stopwatch();
    bool dismiss_alerts();
};

ClockService &clock_service();

}  // namespace nightglass::services

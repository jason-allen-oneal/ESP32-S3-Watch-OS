#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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

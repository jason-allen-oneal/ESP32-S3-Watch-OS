#pragma once

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
};

struct ClockSnapshot {
    std::uint32_t sequence{0};
    bool time_valid{false};
    std::int64_t utc_epoch_seconds{0};
    CivilTime local_time{};
    ClockSettings settings{};
    AlarmSettings alarm{};
    bool alarm_ringing{false};
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
    bool set_timer_duration(std::uint32_t seconds);
    bool toggle_timer();
    bool reset_timer();
    bool toggle_stopwatch();
    bool reset_stopwatch();
    bool dismiss_alerts();
};

ClockService &clock_service();

}  // namespace nightglass::services

#pragma once

#include <cstdint>
#include <type_traits>

#include "nightglass/core/status.hpp"
#include "nightglass/services/activity_units.hpp"

namespace nightglass::services {

enum class ActivityReadiness : std::uint8_t {
    unavailable,
    warming_up,
    ready,
    stale,
};

struct ActivitySettings {
    // Distance is steps multiplied by the distance covered by one step.
    // A gait stride spans two steps, so expose the unambiguous step length.
    std::uint16_t step_length_mm{750};
    std::uint32_t daily_goal_steps{10'000};
    ActivityUnits units{ActivityUnits::imperial};
};

struct ActivitySnapshot {
    std::uint32_t sequence{0};
    ActivityReadiness readiness{ActivityReadiness::unavailable};
    bool sensor_present{false};
    bool sample_valid{false};
    bool calibrated{false};
    std::uint16_t calibration_samples{0};
    std::uint16_t calibration_required{0};
    std::uint32_t calibration_restarts{0};
    std::uint32_t steps_today{0};
    std::uint32_t distance_m{0};
    std::uint64_t distance_mm{0};
    std::uint8_t goal_percent{0};
    ActivitySettings settings{};
    bool date_valid{false};
    std::int64_t local_day{0};
    bool persistence_ok{true};
    std::int64_t last_step_us{0};
    std::int64_t sampled_at_us{0};
};

static_assert(std::is_trivially_copyable_v<ActivitySnapshot>);

class ActivityService {
public:
    nightglass::core::Status start();
    [[nodiscard]] ActivitySnapshot snapshot() const;
    bool update_settings(const ActivitySettings &settings);
    bool reset_today();
};

ActivityService &activity_service();

}  // namespace nightglass::services

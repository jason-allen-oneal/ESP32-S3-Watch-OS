#pragma once

#include <cstdint>
#include <type_traits>

#include "driver/i2c_master.h"
#include "nightglass/core/status.hpp"

namespace nightglass::services {

struct RtcSnapshot {
    bool present{false};
    bool valid{false};
    std::uint16_t year{0};
    std::uint8_t month{0};
    std::uint8_t day{0};
    std::uint8_t weekday{0};
    std::uint8_t hour{0};
    std::uint8_t minute{0};
    std::uint8_t second{0};
    std::int64_t sampled_at_us{0};
};

struct BatterySnapshot {
    bool pmic_present{false};
    bool battery_present{false};
    bool voltage_valid{false};
    bool percent_valid{false};
    bool charging{false};
    bool discharging{false};
    std::uint16_t voltage_mv{0};
    std::uint8_t percent{0};
    std::int64_t sampled_at_us{0};
};

struct MotionSnapshot {
    bool present{false};
    bool valid{false};
    bool moving{false};
    bool gyro_calibrated{false};
    std::uint16_t gyro_calibration_samples{0};
    std::uint16_t gyro_calibration_required{0};
    std::uint32_t gyro_calibration_restarts{0};
    float accel_x_g{0.0F};
    float accel_y_g{0.0F};
    float accel_z_g{0.0F};
    float gyro_x_dps{0.0F};
    float gyro_y_dps{0.0F};
    float gyro_z_dps{0.0F};
    float gyro_bias_x_dps{0.0F};
    float gyro_bias_y_dps{0.0F};
    float gyro_bias_z_dps{0.0F};
    std::int64_t sampled_at_us{0};
};

struct HapticSnapshot {
    bool actuator_present{false};
    bool ready{false};
    bool supply_state_known{false};
    bool supply_enabled{false};
    bool pulse_active{false};
    std::uint16_t supply_voltage_mv{0};
    std::uint32_t accepted_pulses{0};
    std::uint32_t failed_pulses{0};
    std::int64_t last_pulse_us{0};
};

struct HardwareSnapshot {
    std::uint32_t sequence{0};
    RtcSnapshot rtc{};
    BatterySnapshot battery{};
    MotionSnapshot motion{};
    HapticSnapshot haptic{};
};

static_assert(std::is_trivially_copyable_v<HardwareSnapshot>);

class HardwareService {
public:
    nightglass::core::Status start(i2c_master_bus_handle_t bus_handle);
    [[nodiscard]] HardwareSnapshot snapshot() const;
    bool request_haptic(std::uint16_t duration_ms = 120);
};

HardwareService &hardware_service();

}  // namespace nightglass::services

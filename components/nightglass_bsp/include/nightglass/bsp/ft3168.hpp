#pragma once

#include <cstdint>

namespace nightglass::bsp {

constexpr std::uint8_t kFt3168DeviceIdRegister = 0xa0;
constexpr std::uint8_t kFt3168DeviceId = 0x03;
constexpr std::uint8_t kFt3168PowerModeRegister = 0xa5;
constexpr std::uint8_t kFt3168ActiveMode = 0x00;

// The BSP's FT5x06 coordinate reader is compatible, but its threshold/idle
// initialization is not an FT3168 configuration. Restore the controller's
// factory tuning before selecting the documented FT3168 active mode. Monitor
// mode can NACK after another slave uses this board's shared I2C bus.
// Register identities: Waveshare's FT3x68 hardware reference. FT3168 datasheet
// v0.6, table 3-5 requires at least 70 ms from reset release to reporting. Wait
// 100 ms before configuration, rather than the vendor example's 50 ms, so
// initialization has finished before mode writes and the first real sample.
template <typename Io>
bool configure_ft3168(Io &io, std::uint8_t &device_id, std::uint8_t &power_mode) {
    if (!io.set_reset(true)) return false;
    io.delay_ms(20);
    if (!io.set_reset(false)) return false;
    io.delay_ms(100);
    if (!io.read(kFt3168DeviceIdRegister, device_id) || device_id != kFt3168DeviceId) {
        return false;
    }
    if (!io.write(kFt3168PowerModeRegister, kFt3168ActiveMode)) return false;
    io.delay_ms(20);
    return io.read(kFt3168PowerModeRegister, power_mode) &&
           power_mode == kFt3168ActiveMode;
}

}  // namespace nightglass::bsp

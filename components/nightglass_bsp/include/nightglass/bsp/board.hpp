#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "nightglass/core/status.hpp"

namespace nightglass::bsp {

class Board {
public:
    nightglass::core::Status prepare_safe_outputs();
    nightglass::core::Status start_essential();
    [[nodiscard]] i2c_master_bus_handle_t i2c_bus() const;
    nightglass::core::Status sleep_display();
    nightglass::core::Status wake_display();
    nightglass::core::Status set_brightness(std::uint8_t percent);
    [[nodiscard]] std::uint8_t brightness() const;
    // Cached state after successful command submission, not panel readback.
    [[nodiscard]] bool display_sleep_requested() const { return display_sleeping_; }
    bool lock_display(int timeout_ms);
    void unlock_display();

private:
    std::uint8_t target_brightness_{30};
    bool display_sleeping_{false};
};

Board &board();

}  // namespace nightglass::bsp

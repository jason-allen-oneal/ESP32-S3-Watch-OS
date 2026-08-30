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
    nightglass::core::Status set_brightness(std::uint8_t percent);
    [[nodiscard]] std::uint8_t brightness() const;
    bool lock_display(int timeout_ms);
    void unlock_display();
};

Board &board();

}  // namespace nightglass::bsp

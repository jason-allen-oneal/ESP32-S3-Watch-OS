#pragma once

#include "driver/i2c_master.h"
#include "morrow/core/status.hpp"

namespace morrow::bsp {

class Board {
public:
    morrow::core::Status prepare_safe_outputs();
    morrow::core::Status start_essential();
    [[nodiscard]] i2c_master_bus_handle_t i2c_bus() const;
    bool lock_display(int timeout_ms);
    void unlock_display();
};

Board &board();

}  // namespace morrow::bsp

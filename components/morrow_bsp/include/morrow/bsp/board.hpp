#pragma once

#include "morrow/core/status.hpp"

namespace morrow::bsp {

class Board {
public:
    morrow::core::Status start_essential();
    bool lock_display(int timeout_ms);
    void unlock_display();
};

Board &board();

}  // namespace morrow::bsp

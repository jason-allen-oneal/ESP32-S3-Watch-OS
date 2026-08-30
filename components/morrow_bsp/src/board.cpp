#include "morrow/bsp/board.hpp"

#include "bsp/esp-bsp.h"
#include "morrow/core/health.hpp"

namespace morrow::bsp {

namespace {
Board instance;
}

morrow::core::Status Board::start_essential() {
    bsp_display_start();
    morrow::core::health_registry().set("display", morrow::core::HealthState::ok,
                                       "BSP display task started");
    return morrow::core::Status::Ok();
}

bool Board::lock_display(int timeout_ms) { return bsp_display_lock(timeout_ms); }
void Board::unlock_display() { bsp_display_unlock(); }
Board &board() { return instance; }

}  // namespace morrow::bsp

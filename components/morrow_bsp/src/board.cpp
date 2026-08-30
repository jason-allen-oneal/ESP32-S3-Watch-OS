#include "morrow/bsp/board.hpp"

#include "bsp/esp-bsp.h"
#include "morrow/core/health.hpp"

namespace morrow::bsp {

namespace {
Board instance;
}

morrow::core::Status Board::start_essential() {
    auto *display = bsp_display_start();
    if (!display) {
        morrow::core::health_registry().set("display", morrow::core::HealthState::failed,
                                           "BSP display initialization failed");
        return {morrow::core::StatusCode::io_error, "display initialization failed"};
    }

    if (!bsp_display_get_input_dev()) {
        morrow::core::health_registry().set("touch", morrow::core::HealthState::failed,
                                           "BSP touch initialization failed");
        return {morrow::core::StatusCode::io_error, "touch initialization failed"};
    }

    // The vendor BSP powers the AMOLED up at 100%. Use a conservative bench
    // level until PowerService owns adaptive brightness and burn-in policy.
    const bool brightness_ready = bsp_display_brightness_set(30) == ESP_OK;
    if (!brightness_ready) {
        morrow::core::health_registry().set("display", morrow::core::HealthState::degraded,
                                           "Display online; brightness control failed");
    } else {
        morrow::core::health_registry().set("display", morrow::core::HealthState::ok,
                                           "BSP display and touch started");
    }

    morrow::core::health_registry().set("touch", morrow::core::HealthState::ok,
                                       "BSP touch input active");
    return morrow::core::Status::Ok();
}

bool Board::lock_display(int timeout_ms) { return bsp_display_lock(timeout_ms); }
void Board::unlock_display() { bsp_display_unlock(); }
Board &board() { return instance; }

}  // namespace morrow::bsp

#include "morrow/bsp/board.hpp"

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "morrow/core/health.hpp"

namespace morrow::bsp {

namespace {
Board instance;
}

morrow::core::Status Board::prepare_safe_outputs() {
    // Fail-safe the transistor-driven vibration output before any peripheral
    // or UI startup can delay service initialization.
    const gpio_config_t haptic_safe_config{
        .pin_bit_mask = 1ULL << GPIO_NUM_18,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&haptic_safe_config) != ESP_OK ||
        gpio_set_level(GPIO_NUM_18, 0) != ESP_OK) {
        return {morrow::core::StatusCode::io_error, "haptic fail-safe failed"};
    }
    return morrow::core::Status::Ok();
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

i2c_master_bus_handle_t Board::i2c_bus() const { return bsp_i2c_get_handle(); }
bool Board::lock_display(int timeout_ms) { return bsp_display_lock(timeout_ms); }
void Board::unlock_display() { bsp_display_unlock(); }
Board &board() { return instance; }

}  // namespace morrow::bsp

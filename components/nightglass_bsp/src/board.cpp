#include "nightglass/bsp/board.hpp"

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "nightglass/core/health.hpp"

namespace nightglass::bsp {

namespace {
Board instance;

bool configure_safe_output(gpio_num_t gpio, bool configure_sleep_pull_down) {
    const gpio_config_t config{
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK || gpio_set_level(gpio, 0) != ESP_OK ||
        gpio_get_level(gpio) != 0) {
        return false;
    }
    if (!configure_sleep_pull_down) return true;

    // GPIO46 drives the active-high audio PA. During light sleep, release the
    // active output and retain a pull-down rather than trusting the last
    // output latch. This is a separate physical barrier from codec mute.
    return gpio_sleep_set_direction(gpio, GPIO_MODE_DISABLE) == ESP_OK &&
           gpio_sleep_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY) == ESP_OK &&
           gpio_sleep_sel_en(gpio) == ESP_OK;
}
}

nightglass::core::Status Board::prepare_safe_outputs() {
    // Fail-safe the transistor-driven vibration output before any peripheral
    // or UI startup can delay service initialization.
    if (!configure_safe_output(GPIO_NUM_18, false)) {
        return {nightglass::core::StatusCode::io_error, "haptic fail-safe failed"};
    }
    // GPIO46 is the ES8311 power-amplifier enable. Keep it physically low
    // before the display, I2C, or any codec code starts. The audio service
    // alone may raise it during a bounded explicit speaker diagnostic.
    if (!configure_safe_output(GPIO_NUM_46, true)) {
        return {nightglass::core::StatusCode::io_error, "audio PA fail-safe failed"};
    }
    return nightglass::core::Status::Ok();
}

nightglass::core::Status Board::start_essential() {

    auto *display = bsp_display_start();
    if (!display) {
        nightglass::core::health_registry().set("display", nightglass::core::HealthState::failed,
                                           "BSP display initialization failed");
        return {nightglass::core::StatusCode::io_error, "display initialization failed"};
    }

    if (!bsp_display_get_input_dev()) {
        nightglass::core::health_registry().set("touch", nightglass::core::HealthState::failed,
                                           "BSP touch initialization failed");
        return {nightglass::core::StatusCode::io_error, "touch initialization failed"};
    }

    // The vendor BSP powers the AMOLED up at 100%. Use a conservative bench
    // level until PowerService owns adaptive brightness and burn-in policy.
    const bool brightness_ready = set_brightness(30).is_ok();
    if (!brightness_ready) {
        nightglass::core::health_registry().set("display", nightglass::core::HealthState::degraded,
                                           "Display online; brightness control failed");
    } else {
        nightglass::core::health_registry().set("display", nightglass::core::HealthState::ok,
                                           "BSP display and touch started");
    }

    nightglass::core::health_registry().set("touch", nightglass::core::HealthState::ok,
                                       "BSP touch input active");
    return nightglass::core::Status::Ok();
}

i2c_master_bus_handle_t Board::i2c_bus() const { return bsp_i2c_get_handle(); }
nightglass::core::Status Board::set_brightness(std::uint8_t percent) {
    if (percent > 100) {
        return {nightglass::core::StatusCode::invalid_state, "brightness outside 0-100"};
    }
    // The vendor BSP currently returns ESP_OK even if its DCS write fails.
    // Treat this as a requested/cached level until physical HIL confirms it.
    if (bsp_display_brightness_set(percent) != ESP_OK) {
        return {nightglass::core::StatusCode::io_error, "brightness request failed"};
    }
    return nightglass::core::Status::Ok();
}

std::uint8_t Board::brightness() const {
    const int value = bsp_display_brightness_get();
    return value < 0 ? 0 : value > 100 ? 100 : static_cast<std::uint8_t>(value);
}

bool Board::lock_display(int timeout_ms) { return bsp_display_lock(timeout_ms); }
void Board::unlock_display() { bsp_display_unlock(); }
Board &board() { return instance; }

}  // namespace nightglass::bsp

#include "nightglass/services/power.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nightglass/bsp/board.hpp"
#include "nightglass/core/health.hpp"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_power";
constexpr gpio_num_t kSideKeyGpio = GPIO_NUM_10;
constexpr std::uint8_t kActiveBrightness = 30;
constexpr std::uint8_t kDimBrightness = 8;
constexpr std::int64_t kDimAfterUs = 15'000'000;
constexpr std::int64_t kBlankAfterUs = 30'000'000;
constexpr TickType_t kSupervisorPeriod = pdMS_TO_TICKS(20);
constexpr std::uint8_t kDebounceSamples = 2;

PowerService instance;
TaskHandle_t supervisor_task = nullptr;
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
PowerSnapshot current{};

std::uint8_t brightness_for(nightglass::core::PowerState state) {
    switch (state) {
        case nightglass::core::PowerState::active: return kActiveBrightness;
        case nightglass::core::PowerState::dim: return kDimBrightness;
        case nightglass::core::PowerState::screen_blank: return 0;
        default: return 0;
    }
}

void publish_activity(std::int64_t when, nightglass::core::WakeReason reason) {
    portENTER_CRITICAL(&snapshot_mux);
    current.last_activity_us = when;
    if (current.state != nightglass::core::PowerState::active) current.last_wake = reason;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
}

void publish_key(bool ready, bool pressed) {
    portENTER_CRITICAL(&snapshot_mux);
    current.side_key_ready = ready;
    current.side_key_pressed = pressed;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
}

void apply_state(nightglass::core::PowerState target, std::int64_t observed_activity_us) {
    PowerSnapshot before{};
    portENTER_CRITICAL(&snapshot_mux);
    before = current;
    portEXIT_CRITICAL(&snapshot_mux);
    if (before.state == target) return;

    const auto brightness = brightness_for(target);
    auto &board = nightglass::bsp::board();
    if (!board.lock_display(100)) {
        nightglass::core::health_registry().set("power", nightglass::core::HealthState::degraded,
                                           "Display busy; brightness transition deferred");
        return;
    }
    PowerSnapshot locked_snapshot{};
    portENTER_CRITICAL(&snapshot_mux);
    locked_snapshot = current;
    portEXIT_CRITICAL(&snapshot_mux);
    if (locked_snapshot.last_activity_us != observed_activity_us ||
        locked_snapshot.state == target) {
        board.unlock_display();
        return;
    }
    const auto status = board.set_brightness(brightness);
    board.unlock_display();
    if (!status.is_ok()) {
        nightglass::core::health_registry().set("power", nightglass::core::HealthState::failed,
                                           status.detail);
        ESP_LOGE(kTag, "Brightness transition failed: %s", status.detail);
        return;
    }

    portENTER_CRITICAL(&snapshot_mux);
    current.state = target;
    current.requested_brightness = brightness;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    ESP_LOGI(kTag, "Display policy state=%u brightness=%u",
             static_cast<unsigned>(target), brightness);
}

void supervisor(void *) {
    bool debounced_key = gpio_get_level(kSideKeyGpio) > 0;
    bool candidate_key = debounced_key;
    std::uint8_t stable_samples = kDebounceSamples;
    while (true) {
        ulTaskNotifyTake(pdTRUE, kSupervisorPeriod);
        const auto now = esp_timer_get_time();
        const bool raw_key = gpio_get_level(kSideKeyGpio) > 0;
        if (raw_key != candidate_key) {
            candidate_key = raw_key;
            stable_samples = 1;
        } else if (stable_samples < kDebounceSamples) {
            ++stable_samples;
        }
        if (stable_samples >= kDebounceSamples && candidate_key != debounced_key) {
            debounced_key = candidate_key;
            publish_key(true, debounced_key);
            if (debounced_key) publish_activity(now, nightglass::core::WakeReason::button);
            ESP_LOGI(kTag, "GPIO10 side key %s", debounced_key ? "pressed" : "released");
        }

        PowerSnapshot snapshot{};
        portENTER_CRITICAL(&snapshot_mux);
        snapshot = current;
        portEXIT_CRITICAL(&snapshot_mux);
        const auto inactive_us = now - snapshot.last_activity_us;
        const auto target = inactive_us >= kBlankAfterUs
            ? nightglass::core::PowerState::screen_blank
            : inactive_us >= kDimAfterUs ? nightglass::core::PowerState::dim
                                         : nightglass::core::PowerState::active;
        apply_state(target, snapshot.last_activity_us);
    }
}

}  // namespace

nightglass::core::Status PowerService::start() {
    if (supervisor_task) return nightglass::core::Status::Ok();

    const gpio_config_t side_key_config{
        .pin_bit_mask = 1ULL << kSideKeyGpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&side_key_config) != ESP_OK) {
        nightglass::core::health_registry().set("power", nightglass::core::HealthState::failed,
                                           "GPIO10 side key setup failed");
        return {nightglass::core::StatusCode::io_error, "side key setup failed"};
    }

    const auto now = esp_timer_get_time();
    const bool side_key_pressed = gpio_get_level(kSideKeyGpio) > 0;
    portENTER_CRITICAL(&snapshot_mux);
    current = {};
    current.state = nightglass::core::PowerState::active;
    current.last_wake = nightglass::core::WakeReason::cold_boot;
    current.requested_brightness = kActiveBrightness;
    current.side_key_ready = true;
    current.side_key_pressed = side_key_pressed;
    current.last_activity_us = now;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);

    if (xTaskCreatePinnedToCore(supervisor, "nightglass_power", 4096, nullptr, 5,
                                &supervisor_task, 0) != pdPASS) {
        supervisor_task = nullptr;
        portENTER_CRITICAL(&snapshot_mux);
        current.side_key_ready = false;
        ++current.sequence;
        portEXIT_CRITICAL(&snapshot_mux);
        nightglass::core::health_registry().set("power", nightglass::core::HealthState::failed,
                                           "Power supervisor task creation failed");
        return {nightglass::core::StatusCode::no_memory, "power task creation failed"};
    }

    nightglass::core::health_registry().set(
        "power", nightglass::core::HealthState::degraded,
        "Active/dim/blank policy active; brightness transport awaits HIL");
    ESP_LOGI(kTag, "Power supervisor active: dim=%llds blank=%llds GPIO10=%d",
             kDimAfterUs / 1'000'000, kBlankAfterUs / 1'000'000,
             side_key_pressed);
    return nightglass::core::Status::Ok();
}

void PowerService::note_activity(nightglass::core::WakeReason reason) {
    publish_activity(esp_timer_get_time(), reason);
    if (supervisor_task) xTaskNotifyGive(supervisor_task);
}

PowerSnapshot PowerService::snapshot() const {
    PowerSnapshot copy{};
    portENTER_CRITICAL(&snapshot_mux);
    copy = current;
    portEXIT_CRITICAL(&snapshot_mux);
    return copy;
}

PowerService &power_service() { return instance; }

}  // namespace nightglass::services

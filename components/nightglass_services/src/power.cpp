#include "nightglass/services/power.hpp"

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nightglass/bsp/board.hpp"
#include "nightglass/core/health.hpp"
#include "nightglass/services/clock.hpp"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_power";
constexpr gpio_num_t kSideKeyGpio = GPIO_NUM_10;
constexpr gpio_num_t kTouchInterruptGpio = GPIO_NUM_38;
constexpr TickType_t kSupervisorPeriod = pdMS_TO_TICKS(20);
constexpr std::uint8_t kDebounceSamples = 2;
constexpr char kNvsNamespace[] = "ng_power";

PowerService instance;
TaskHandle_t supervisor_task = nullptr;
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
PowerSnapshot current{};

std::uint8_t brightness_for(nightglass::core::PowerState state,
                            const PowerSettings &settings) {
    switch (state) {
        case nightglass::core::PowerState::active: return settings.active_brightness;
        case nightglass::core::PowerState::dim: return settings.dim_brightness;
        case nightglass::core::PowerState::screen_blank: return 0;
        default: return 0;
    }
}

bool valid_settings(const PowerSettings &settings) {
    return settings.active_brightness >= 5 && settings.active_brightness <= 100 &&
           settings.dim_brightness <= 50 &&
           settings.dim_brightness < settings.active_brightness &&
           settings.dim_after_seconds >= 5 && settings.dim_after_seconds <= 600 &&
           settings.blank_after_seconds > settings.dim_after_seconds &&
           settings.blank_after_seconds <= 1800 &&
           settings.sleep_after_blank_seconds <= 3600;
}

PowerSettings load_settings() {
    PowerSettings settings{};
    nvs_handle_t handle{};
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return settings;

    std::uint8_t u8{};
    std::uint16_t u16{};
    if (nvs_get_u8(handle, "active", &u8) == ESP_OK) settings.active_brightness = u8;
    if (nvs_get_u8(handle, "dim", &u8) == ESP_OK) settings.dim_brightness = u8;
    if (nvs_get_u16(handle, "dim_s", &u16) == ESP_OK) settings.dim_after_seconds = u16;
    if (nvs_get_u16(handle, "blank_s", &u16) == ESP_OK) settings.blank_after_seconds = u16;
    if (nvs_get_u16(handle, "sleep_s", &u16) == ESP_OK) settings.sleep_after_blank_seconds = u16;
    nvs_close(handle);
    return valid_settings(settings) ? settings : PowerSettings{};
}

esp_err_t save_settings(const PowerSettings &settings) {
    nvs_handle_t handle{};
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    if ((result = nvs_set_u8(handle, "active", settings.active_brightness)) == ESP_OK &&
        (result = nvs_set_u8(handle, "dim", settings.dim_brightness)) == ESP_OK &&
        (result = nvs_set_u16(handle, "dim_s", settings.dim_after_seconds)) == ESP_OK &&
        (result = nvs_set_u16(handle, "blank_s", settings.blank_after_seconds)) == ESP_OK &&
        (result = nvs_set_u16(handle, "sleep_s", settings.sleep_after_blank_seconds)) == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

void publish_activity(std::int64_t when, nightglass::core::WakeReason reason) {
    portENTER_CRITICAL(&snapshot_mux);
    current.last_activity_us = when;
    if (current.state != nightglass::core::PowerState::active) current.last_wake = reason;
    if (reason == nightglass::core::WakeReason::touch) current.wake_touch_pending = false;
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
    const auto brightness = brightness_for(target, before.settings);
    if (before.state == target && before.requested_brightness == brightness) return;

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
        (locked_snapshot.state == target &&
         locked_snapshot.requested_brightness == brightness)) {
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

void enter_light_sleep(std::int64_t observed_activity_us) {
    if (gpio_get_level(kTouchInterruptGpio) == 0) {
        publish_activity(esp_timer_get_time(), nightglass::core::WakeReason::touch);
        return;
    }
    if (gpio_get_level(kSideKeyGpio) > 0) {
        publish_activity(esp_timer_get_time(), nightglass::core::WakeReason::button);
        return;
    }

    portENTER_CRITICAL(&snapshot_mux);
    if (current.last_activity_us != observed_activity_us ||
        current.state != nightglass::core::PowerState::screen_blank) {
        portEXIT_CRITICAL(&snapshot_mux);
        return;
    }
    current.state = nightglass::core::PowerState::light_sleep;
    current.sleeping = true;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);

    // The touch driver normally owns GPIO38 as falling-edge IRQ. Switch it to
    // level wake only for the sleep window, then restore its live IRQ mode.
    if (gpio_intr_disable(kTouchInterruptGpio) != ESP_OK ||
        gpio_wakeup_enable(kTouchInterruptGpio, GPIO_INTR_LOW_LEVEL) != ESP_OK) {
        portENTER_CRITICAL(&snapshot_mux);
        current.state = nightglass::core::PowerState::screen_blank;
        current.sleeping = false;
        ++current.sequence;
        portEXIT_CRITICAL(&snapshot_mux);
        gpio_set_intr_type(kTouchInterruptGpio, GPIO_INTR_NEGEDGE);
        gpio_intr_enable(kTouchInterruptGpio);
        nightglass::core::health_registry().set("power", nightglass::core::HealthState::failed,
                                               "Touch wake setup failed");
        return;
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    const auto deadline_wake_us = clock_service().next_wake_delay_us();
    if (deadline_wake_us > 0 && esp_sleep_enable_timer_wakeup(deadline_wake_us) != ESP_OK) {
        gpio_wakeup_disable(kTouchInterruptGpio);
        gpio_set_intr_type(kTouchInterruptGpio, GPIO_INTR_NEGEDGE);
        gpio_intr_enable(kTouchInterruptGpio);
        portENTER_CRITICAL(&snapshot_mux);
        current.state = nightglass::core::PowerState::screen_blank;
        current.sleeping = false;
        ++current.sequence;
        portEXIT_CRITICAL(&snapshot_mux);
        nightglass::core::health_registry().set("power", nightglass::core::HealthState::failed,
                                               "Deadline wake setup failed; sleep skipped");
        return;
    }

    const auto started_us = esp_timer_get_time();
    ESP_LOGI(kTag, "Entering light sleep");
    const esp_err_t result = esp_light_sleep_start();
    const auto woke_us = esp_timer_get_time();
    const auto cause = esp_sleep_get_wakeup_cause();
    gpio_wakeup_disable(kTouchInterruptGpio);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    gpio_set_intr_type(kTouchInterruptGpio, GPIO_INTR_NEGEDGE);
    gpio_intr_enable(kTouchInterruptGpio);
    const auto wake_reason = cause == ESP_SLEEP_WAKEUP_GPIO
                                 ? nightglass::core::WakeReason::touch
                             : cause == ESP_SLEEP_WAKEUP_EXT1
                                 ? nightglass::core::WakeReason::button
                             : cause == ESP_SLEEP_WAKEUP_TIMER
                                 ? nightglass::core::WakeReason::timer
                                 : nightglass::core::WakeReason::unknown;

    portENTER_CRITICAL(&snapshot_mux);
    current.state = nightglass::core::PowerState::screen_blank;
    current.sleeping = false;
    current.last_wake = wake_reason;
    current.wake_touch_pending = wake_reason == nightglass::core::WakeReason::touch;
    current.last_activity_us = woke_us;
    ++current.sleep_count;
    current.last_sleep_ms = static_cast<std::uint32_t>((woke_us - started_us) / 1000);
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    ESP_LOGI(kTag, "Light sleep returned status=%s cause=%u duration=%lums",
             esp_err_to_name(result), static_cast<unsigned>(cause),
             static_cast<unsigned long>((woke_us - started_us) / 1000));
}

void supervisor(void *) {
    bool debounced_key = gpio_get_level(kSideKeyGpio) > 0;
    bool candidate_key = debounced_key;
    bool usb_sleep_inhibited = false;
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
        if (snapshot.wake_touch_pending && inactive_us > 1'000'000) {
            portENTER_CRITICAL(&snapshot_mux);
            current.wake_touch_pending = false;
            ++current.sequence;
            portEXIT_CRITICAL(&snapshot_mux);
        }
        const auto dim_after_us = static_cast<std::int64_t>(snapshot.settings.dim_after_seconds) * 1'000'000;
        const auto blank_after_us = static_cast<std::int64_t>(snapshot.settings.blank_after_seconds) * 1'000'000;
        const auto sleep_after_us = static_cast<std::int64_t>(snapshot.settings.sleep_after_blank_seconds) * 1'000'000;
        const auto target = inactive_us >= blank_after_us
            ? nightglass::core::PowerState::screen_blank
            : inactive_us >= dim_after_us ? nightglass::core::PowerState::dim
                                          : nightglass::core::PowerState::active;
        apply_state(target, snapshot.last_activity_us);
        if (snapshot.settings.sleep_after_blank_seconds > 0 &&
            inactive_us >= blank_after_us + sleep_after_us) {
            if (usb_serial_jtag_is_connected()) {
                if (!usb_sleep_inhibited) {
                    ESP_LOGI(kTag, "USB host connected; light sleep inhibited");
                    usb_sleep_inhibited = true;
                }
            } else {
                if (usb_sleep_inhibited) {
                    ESP_LOGI(kTag, "USB host disconnected; light sleep restored");
                    usb_sleep_inhibited = false;
                }
                PowerSnapshot latest{};
                portENTER_CRITICAL(&snapshot_mux);
                latest = current;
                portEXIT_CRITICAL(&snapshot_mux);
                enter_light_sleep(latest.last_activity_us);
            }
        } else {
            usb_sleep_inhibited = false;
        }
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

    if (esp_sleep_enable_gpio_wakeup() != ESP_OK ||
        esp_sleep_enable_ext1_wakeup_io(1ULL << kSideKeyGpio,
                                       ESP_EXT1_WAKEUP_ANY_HIGH) != ESP_OK) {
        nightglass::core::health_registry().set("power", nightglass::core::HealthState::failed,
                                               "GPIO light-sleep wake setup failed");
        return {nightglass::core::StatusCode::io_error, "light-sleep wake setup failed"};
    }

    const auto settings = load_settings();
    const auto now = esp_timer_get_time();
    const bool side_key_pressed = gpio_get_level(kSideKeyGpio) > 0;
    portENTER_CRITICAL(&snapshot_mux);
    current = {};
    current.state = nightglass::core::PowerState::active;
    current.last_wake = nightglass::core::WakeReason::cold_boot;
    current.requested_brightness = settings.active_brightness;
    current.side_key_ready = true;
    current.side_key_pressed = side_key_pressed;
    current.last_activity_us = now;
    current.light_sleep_enabled = settings.sleep_after_blank_seconds > 0;
    current.settings = settings;
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
        "power", nightglass::core::HealthState::ok,
        "Configurable brightness and GPIO light sleep active");
    ESP_LOGI(kTag, "Power supervisor active: dim=%us blank=%us sleep=%us GPIO10=%d",
             settings.dim_after_seconds, settings.blank_after_seconds,
             settings.sleep_after_blank_seconds, side_key_pressed);
    return nightglass::core::Status::Ok();
}

nightglass::core::Status PowerService::update_settings(const PowerSettings &settings) {
    if (!valid_settings(settings)) {
        return {nightglass::core::StatusCode::invalid_state, "invalid power settings"};
    }
    const esp_err_t persisted = save_settings(settings);
    portENTER_CRITICAL(&snapshot_mux);
    current.settings = settings;
    current.light_sleep_enabled = settings.sleep_after_blank_seconds > 0;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    note_activity(nightglass::core::WakeReason::touch);
    if (persisted != ESP_OK) {
        nightglass::core::health_registry().set("power", nightglass::core::HealthState::degraded,
                                               "Power settings active but not persisted");
        return {nightglass::core::StatusCode::io_error, "power settings persistence failed"};
    }
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

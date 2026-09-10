#include "nightglass/services/power.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nightglass/bsp/board.hpp"
#include "nightglass/core/health.hpp"
#include "nightglass/services/clock.hpp"
#include "nightglass/services/activity.hpp"
#include "nightglass/services/network_weather.hpp"
#include "nightglass/services/connectivity.hpp"
#include "nightglass/services/premium.hpp"
#include "nightglass/services/hardware.hpp"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_power";
constexpr gpio_num_t kSideKeyGpio = GPIO_NUM_10;
constexpr gpio_num_t kTouchInterruptGpio = GPIO_NUM_38;
constexpr std::int64_t kSideKeyDebounceUs = 20'000;
constexpr std::int64_t kSleepRetryUs = 1'000'000;
// A state transition synchronously quiesces the LVGL pipeline through the UI
// observer. Keep enough headroom for that cross-service call chain; the former
// 4 KiB stack overflowed at the first screen-blank transition on hardware.
constexpr std::uint32_t kSupervisorStackBytes = 8192;
constexpr char kNvsNamespace[] = "ng_power";

PowerService instance;
TaskHandle_t supervisor_task = nullptr;
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
PowerSnapshot current{};
portMUX_TYPE observer_mux = portMUX_INITIALIZER_UNLOCKED;
PowerStateObserver state_observer = nullptr;
void *state_observer_context = nullptr;

void IRAM_ATTR side_key_interrupt(void *) {
    if (!supervisor_task) return;
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(supervisor_task, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) portYIELD_FROM_ISR();
}

void notify_state_observer(nightglass::core::PowerState state) {
    PowerStateObserver observer = nullptr;
    void *context = nullptr;
    portENTER_CRITICAL(&observer_mux);
    observer = state_observer;
    context = state_observer_context;
    portEXIT_CRITICAL(&observer_mux);
    if (observer) observer(state, context);
}

std::uint8_t brightness_for(nightglass::core::PowerState state,
                            const PowerSettings &settings) {
    switch (state) {
        case nightglass::core::PowerState::active: return settings.active_brightness;
        case nightglass::core::PowerState::dim: return settings.dim_brightness;
        case nightglass::core::PowerState::ambient: return 2;
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

bool disable_automatic_light_sleep() {
    esp_pm_config_t config{};
    if (esp_pm_get_configuration(&config) != ESP_OK) return false;
    // GPIO38 is normally a falling-edge FT5x06 interrupt. ESP-IDF's
    // gpio_wakeup_enable() requires a level trigger and also replaces the
    // pin's normal interrupt type. Until that level/edge hand-off passes a
    // repeated hardware wake test, automatic light sleep can miss the touch
    // edge and strand a blank watch. Keep DFS and explicit, bracketed light
    // sleep, but never enter automatic light sleep with an unarmed touch IRQ.
    config.light_sleep_enable = false;
    return esp_pm_configure(&config) == ESP_OK;
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
    current.last_activity_reason = reason;
    if (reason == nightglass::core::WakeReason::touch ||
        reason == nightglass::core::WakeReason::button) {
        current.last_physical_input_us = when;
    }
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
    notify_state_observer(target);
    ESP_LOGI(kTag, "Display policy state=%u brightness=%u",
             static_cast<unsigned>(target), brightness);
}

void enter_light_sleep(std::int64_t observed_activity_us) {
    // Manual long light sleep is not entered while the companion radio is
    // active. ESP-IDF automatic light sleep and BLE modem sleep remain active
    // between connection events, preserving notifications without pinning the
    // CPU and controller fully awake.
    if (connectivity_service().snapshot().settings.enabled) return;
    // CPU-side recognition stops in manual light sleep. Until QMI8658 INT1
    // wake-on-motion passes its own hardware gate, keep the IMU stream alive
    // whenever raise-to-wake is enabled.
    const auto activity = activity_service().snapshot();
    if (activity.service_started && activity.sensor_present && activity.sample_valid &&
        activity.settings.raise_to_wake) {
        return;
    }
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

    if (!network_weather_service().prepare_for_light_sleep()) {
        gpio_wakeup_disable(kTouchInterruptGpio);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        gpio_set_intr_type(kTouchInterruptGpio, GPIO_INTR_NEGEDGE);
        gpio_intr_enable(kTouchInterruptGpio);
        portENTER_CRITICAL(&snapshot_mux);
        current.state = nightglass::core::PowerState::screen_blank;
        current.sleeping = false;
        ++current.sequence;
        portEXIT_CRITICAL(&snapshot_mux);
        nightglass::core::health_registry().set(
            "power", nightglass::core::HealthState::degraded,
            "Wi-Fi stop failed; light sleep skipped");
        return;
    }
    const auto started_us = esp_timer_get_time();
    ESP_LOGI(kTag, "Entering light sleep");
    const esp_err_t result = esp_light_sleep_start();
    const auto woke_us = esp_timer_get_time();
    network_weather_service().resume_from_light_sleep();
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
    if (wake_reason == nightglass::core::WakeReason::touch ||
        wake_reason == nightglass::core::WakeReason::button) {
        current.last_activity_us = woke_us;
        current.last_physical_input_us = woke_us;
    }
    ++current.sleep_count;
    current.last_sleep_ms = static_cast<std::uint32_t>((woke_us - started_us) / 1000);
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    ESP_LOGI(kTag, "Light sleep returned status=%s cause=%u duration=%lums",
             esp_err_to_name(result), static_cast<unsigned>(cause),
             static_cast<unsigned long>((woke_us - started_us) / 1000));
    // Re-evaluate immediately: a physical wake must restore the display,
    // while a background wake should retain the blank state and old activity
    // deadline.
    if (supervisor_task) xTaskNotifyGive(supervisor_task);
}

void supervisor(void *) {
    bool debounced_key = gpio_get_level(kSideKeyGpio) > 0;
    bool candidate_key = debounced_key;
    bool usb_sleep_inhibited = false;
    std::int64_t candidate_since_us = esp_timer_get_time();
    std::int64_t next_sleep_attempt_us = 0;
    TickType_t timeout_ticks = 1;
    while (true) {
        ulTaskNotifyTake(pdTRUE, timeout_ticks);
        const auto now = esp_timer_get_time();
        const bool raw_key = gpio_get_level(kSideKeyGpio) > 0;
        if (raw_key != candidate_key) {
            candidate_key = raw_key;
            candidate_since_us = now;
        }
        if (candidate_key != debounced_key &&
            now - candidate_since_us >= kSideKeyDebounceUs) {
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
        const auto battery = hardware_service().snapshot().battery;
        const bool always_on = premium_aod_allowed(premium_service().profile().flags & kPremiumAlwaysOn,
            battery.battery_present, battery.percent_valid, battery.percent, now - battery.sampled_at_us);
        const auto resting_state = always_on ? nightglass::core::PowerState::ambient
                                             : nightglass::core::PowerState::screen_blank;
        const auto target = inactive_us >= blank_after_us
            ? resting_state
            : inactive_us >= dim_after_us ? nightglass::core::PowerState::dim
                                          : nightglass::core::PowerState::active;
        apply_state(target, snapshot.last_activity_us);
        if (!always_on && snapshot.settings.sleep_after_blank_seconds > 0 &&
            inactive_us >= blank_after_us + sleep_after_us) {
            if (usb_serial_jtag_is_connected()) {
                if (!usb_sleep_inhibited) {
                    ESP_LOGI(kTag, "USB host connected; light sleep inhibited");
                    usb_sleep_inhibited = true;
                }
                next_sleep_attempt_us = now + kSleepRetryUs;
            } else {
                if (usb_sleep_inhibited) {
                    ESP_LOGI(kTag, "USB host disconnected; light sleep restored");
                    usb_sleep_inhibited = false;
                }
                if (now >= next_sleep_attempt_us) {
                    PowerSnapshot latest{};
                    portENTER_CRITICAL(&snapshot_mux);
                    latest = current;
                    portEXIT_CRITICAL(&snapshot_mux);
                    enter_light_sleep(latest.last_activity_us);
                    next_sleep_attempt_us = esp_timer_get_time() + kSleepRetryUs;
                }
            }
        } else {
            usb_sleep_inhibited = false;
        }

        portENTER_CRITICAL(&snapshot_mux);
        snapshot = current;
        portEXIT_CRITICAL(&snapshot_mux);
        const auto after_us = esp_timer_get_time();
        const auto after_inactive_us = after_us - snapshot.last_activity_us;
        const auto after_dim_us = static_cast<std::int64_t>(
            snapshot.settings.dim_after_seconds) * 1'000'000;
        const auto after_blank_us = static_cast<std::int64_t>(
            snapshot.settings.blank_after_seconds) * 1'000'000;
        const auto expected_state = after_inactive_us >= after_blank_us
            ? resting_state
            : after_inactive_us >= after_dim_us ? nightglass::core::PowerState::dim
                                                : nightglass::core::PowerState::active;

        std::int64_t next_deadline_us = 0;
        if (snapshot.state != expected_state) {
            next_deadline_us = after_us;
        } else if (after_inactive_us < after_dim_us) {
            next_deadline_us = snapshot.last_activity_us + after_dim_us;
        } else if (after_inactive_us < after_blank_us) {
            next_deadline_us = snapshot.last_activity_us + after_blank_us;
        } else if (always_on) {
            // Recheck battery freshness even with no input. AOD never prevents
            // a depleted or unreadable battery from returning to normal sleep.
            next_deadline_us = after_us + 5'000'000;
        } else if (snapshot.settings.sleep_after_blank_seconds > 0) {
            const auto sleep_deadline_us = snapshot.last_activity_us + after_blank_us +
                static_cast<std::int64_t>(snapshot.settings.sleep_after_blank_seconds) *
                    1'000'000;
            next_deadline_us = sleep_deadline_us > after_us
                                   ? sleep_deadline_us
                                   : std::max(next_sleep_attempt_us, after_us);
        }
        if (snapshot.wake_touch_pending) {
            const auto touch_deadline_us = snapshot.last_activity_us + 1'000'000;
            if (next_deadline_us == 0 || touch_deadline_us < next_deadline_us) {
                next_deadline_us = touch_deadline_us;
            }
        }
        if (candidate_key != debounced_key) {
            const auto debounce_deadline_us = candidate_since_us + kSideKeyDebounceUs;
            if (next_deadline_us == 0 || debounce_deadline_us < next_deadline_us) {
                next_deadline_us = debounce_deadline_us;
            }
        }
        if (next_deadline_us == 0) {
            timeout_ticks = portMAX_DELAY;
        } else {
            const auto remaining_us = std::max<std::int64_t>(0, next_deadline_us - after_us);
            const auto remaining_ms = static_cast<std::uint64_t>(
                (remaining_us + 999) / 1000);
            timeout_ticks = std::max<TickType_t>(
                1, pdMS_TO_TICKS(remaining_ms));
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
        .intr_type = GPIO_INTR_ANYEDGE,
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
    if (!disable_automatic_light_sleep()) {
        nightglass::core::health_registry().set(
            "power", nightglass::core::HealthState::failed,
            "Could not disable unsafe automatic light sleep");
        return {nightglass::core::StatusCode::io_error,
                "automatic light-sleep safety setup failed"};
    }
    constexpr bool automatic_light_sleep_enabled = false;
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
    current.last_physical_input_us = now;
    current.light_sleep_enabled = settings.sleep_after_blank_seconds > 0;
    current.automatic_light_sleep_enabled = automatic_light_sleep_enabled;
    current.settings = settings;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);

    if (xTaskCreatePinnedToCore(supervisor, "nightglass_power", kSupervisorStackBytes,
                                nullptr, 5,
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

    const esp_err_t isr_service = gpio_install_isr_service(0);
    if ((isr_service != ESP_OK && isr_service != ESP_ERR_INVALID_STATE) ||
        gpio_isr_handler_add(kSideKeyGpio, side_key_interrupt, nullptr) != ESP_OK) {
        vTaskDelete(supervisor_task);
        supervisor_task = nullptr;
        portENTER_CRITICAL(&snapshot_mux);
        current.side_key_ready = false;
        ++current.sequence;
        portEXIT_CRITICAL(&snapshot_mux);
        nightglass::core::health_registry().set("power", nightglass::core::HealthState::failed,
                                               "GPIO10 side key interrupt setup failed");
        return {nightglass::core::StatusCode::io_error,
                "side key interrupt setup failed"};
    }

    nightglass::core::health_registry().set(
        "power", nightglass::core::HealthState::ok,
        "DFS and guarded GPIO light-sleep wake active");
    ESP_LOGI(kTag,
             "Power supervisor active: dim=%us blank=%us sleep=%us auto_light_sleep=%d GPIO10=%d",
             settings.dim_after_seconds, settings.blank_after_seconds,
             settings.sleep_after_blank_seconds, automatic_light_sleep_enabled,
             side_key_pressed);
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

void PowerService::set_state_observer(PowerStateObserver observer, void *context) {
    portENTER_CRITICAL(&observer_mux);
    state_observer = observer;
    state_observer_context = context;
    portEXIT_CRITICAL(&observer_mux);
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

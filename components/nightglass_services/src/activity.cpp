#include "nightglass/services/activity.hpp"

#include <algorithm>
#include <limits>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "nvs.h"
#include "nightglass/core/health.hpp"
#include "nightglass/services/activity_day.hpp"
#include "nightglass/services/activity_processor.hpp"
#include "nightglass/services/clock.hpp"
#include "nightglass/services/connectivity.hpp"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/gesture_processor.hpp"
#include "nightglass/services/gesture_policy.hpp"
#include "nightglass/services/power.hpp"
#include "nightglass/services/time_math.hpp"
#include "nightglass/services/update_transport.hpp"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_activity";
constexpr char kNvsNamespace[] = "ng_activity";
constexpr TickType_t kPeriod = pdMS_TO_TICKS(40);
constexpr std::int64_t kStaleAfterUs = 2'000'000;
constexpr std::int64_t kPeriodicSaveUs = 300'000'000;
constexpr std::int64_t kSaveRetryBackoffUs = 30'000'000;
constexpr std::uint32_t kSaveStepBatch = 20;
constexpr std::uint16_t kMinimumStepLengthMm = 300;
constexpr std::uint16_t kMaximumStepLengthMm = 1'500;
constexpr std::uint32_t kMinimumGoal = 500;
constexpr std::uint32_t kMaximumGoal = 100'000;

enum class CommandType : std::uint8_t { settings, reset_today };
struct Command {
    CommandType type{};
    ActivitySettings settings{};
};

ActivityService instance;
TaskHandle_t worker_task = nullptr;
QueueHandle_t command_queue = nullptr;
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
ActivitySnapshot current{};
ActivityProcessor processor;
GestureProcessor gesture_processor;
ActivityDayState day_state{};
std::uint32_t persisted_steps = 0;
std::int64_t last_save_us = 0;
std::int64_t next_save_attempt_us = 0;
bool dirty = false;
bool priority_dirty = false;

bool valid_settings(const ActivitySettings &settings) {
    return settings.step_length_mm >= kMinimumStepLengthMm &&
           settings.step_length_mm <= kMaximumStepLengthMm &&
           settings.daily_goal_steps >= kMinimumGoal &&
           settings.daily_goal_steps <= kMaximumGoal &&
           (settings.units == ActivityUnits::imperial ||
            settings.units == ActivityUnits::metric);
}

void update_derived(ActivitySnapshot &snapshot) {
    const auto distance_mm = static_cast<std::uint64_t>(snapshot.steps_today) *
                             snapshot.settings.step_length_mm;
    snapshot.distance_mm = distance_mm;
    snapshot.distance_m = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        distance_mm / 1000U, std::numeric_limits<std::uint32_t>::max()));
    snapshot.goal_percent = static_cast<std::uint8_t>(std::min<std::uint64_t>(
        100U, static_cast<std::uint64_t>(snapshot.steps_today) * 100U /
                  snapshot.settings.daily_goal_steps));
}

bool save_state() {
    ActivitySnapshot copy{};
    portENTER_CRITICAL(&snapshot_mux);
    copy = current;
    portEXIT_CRITICAL(&snapshot_mux);

    nvs_handle_t handle{};
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return false;
    if ((result = nvs_set_u8(handle, "version", 2)) == ESP_OK &&
        // Retain the legacy NVS key so existing calibrated values migrate in place.
        (result = nvs_set_u16(handle, "stride_mm", copy.settings.step_length_mm)) == ESP_OK &&
        (result = nvs_set_u8(handle, "units", static_cast<std::uint8_t>(copy.settings.units))) == ESP_OK &&
        (result = nvs_set_u32(handle, "goal", copy.settings.daily_goal_steps)) == ESP_OK &&
        (result = nvs_set_u8(handle, "g_raise", copy.settings.raise_to_wake)) == ESP_OK &&
        (result = nvs_set_u8(handle, "g_twist", copy.settings.double_twist_quick_settings)) == ESP_OK &&
        (result = nvs_set_u8(handle, "g_shake", copy.settings.shake_notifications)) == ESP_OK &&
        (result = nvs_set_u8(handle, "g_flick", copy.settings.flick_media_next)) == ESP_OK &&
        (result = nvs_set_u32(handle, "steps", copy.steps_today)) == ESP_OK &&
        (result = nvs_set_i64(handle, "local_day", day_state.tracked_local_day)) == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result == ESP_OK;
}

void set_persistence(bool ok) {
    bool changed = false;
    bool ready = false;
    portENTER_CRITICAL(&snapshot_mux);
    changed = current.persistence_ok != ok;
    current.persistence_ok = ok;
    ready = current.readiness == ActivityReadiness::ready;
    if (changed) ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    if (!ok) {
        nightglass::core::health_registry().set(
            "activity", nightglass::core::HealthState::degraded,
            "Activity tracking active; NVS persistence failed");
    } else if (changed && ready) {
        nightglass::core::health_registry().set(
            "activity", nightglass::core::HealthState::ok,
            "Activity calibrated and tracking steps");
    }
}

void load_state() {
    ActivitySnapshot loaded{};
    bool persistence_ok = true;
    nvs_handle_t handle{};
    const auto opened = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (opened == ESP_OK) {
        std::uint16_t step_length{};
        std::uint32_t value{};
        std::int64_t local_day{};
        if (nvs_get_u16(handle, "stride_mm", &step_length) == ESP_OK) {
            loaded.settings.step_length_mm = step_length;
        }
        std::uint8_t units{};
        if (nvs_get_u8(handle, "units", &units) == ESP_OK) {
            loaded.settings.units = units == static_cast<std::uint8_t>(ActivityUnits::metric)
                                        ? ActivityUnits::metric : ActivityUnits::imperial;
        }
        if (nvs_get_u32(handle, "goal", &value) == ESP_OK) loaded.settings.daily_goal_steps = value;
        std::uint8_t enabled{};
        if (nvs_get_u8(handle, "g_raise", &enabled) == ESP_OK) {
            loaded.settings.raise_to_wake = enabled != 0;
        }
        if (nvs_get_u8(handle, "g_twist", &enabled) == ESP_OK) {
            loaded.settings.double_twist_quick_settings = enabled != 0;
        }
        if (nvs_get_u8(handle, "g_shake", &enabled) == ESP_OK) {
            loaded.settings.shake_notifications = enabled != 0;
        }
        if (nvs_get_u8(handle, "g_flick", &enabled) == ESP_OK) {
            loaded.settings.flick_media_next = enabled != 0;
        }
        if (nvs_get_u32(handle, "steps", &value) == ESP_OK) loaded.steps_today = value;
        if (nvs_get_i64(handle, "local_day", &local_day) == ESP_OK) day_state.tracked_local_day = local_day;
        nvs_close(handle);
    } else if (opened != ESP_ERR_NVS_NOT_FOUND) {
        persistence_ok = false;
    }
    if (!valid_settings(loaded.settings)) loaded.settings = {};
    loaded.persistence_ok = persistence_ok;
    loaded.calibration_required = ActivityProcessor::kWarmupSamples;
    update_derived(loaded);
    current = loaded;
    persisted_steps = loaded.steps_today;
    last_save_us = esp_timer_get_time();
}

bool submit(const Command &command) {
    return command_queue && xQueueSend(command_queue, &command, 0) == pdTRUE;
}

void publish_processor(const ActivityProcessorOutput &output, const MotionSnapshot &motion,
                       std::int64_t now_us) {
    portENTER_CRITICAL(&snapshot_mux);
    current.sensor_present = motion.present;
    current.sample_valid = output.sample_valid;
    current.calibration_required = output.warmup_required;
    current.calibration_samples = output.warmup_samples;
    current.calibration_restarts = output.warmup_restarts;
    current.calibrated = output.state == ActivityProcessorState::ready;
    current.sampled_at_us = motion.sampled_at_us;
    if (!motion.present) {
        current.readiness = ActivityReadiness::unavailable;
    } else if (!output.sample_valid || now_us - motion.sampled_at_us > kStaleAfterUs) {
        current.readiness = ActivityReadiness::stale;
    } else if (output.state == ActivityProcessorState::warming_up) {
        current.readiness = ActivityReadiness::warming_up;
    } else {
        current.readiness = ActivityReadiness::ready;
    }
    if (output.step_detected && current.steps_today < std::numeric_limits<std::uint32_t>::max()) {
        ++current.steps_today;
        current.last_step_us = now_us;
        dirty = true;
    }
    update_derived(current);
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
}

void publish_gesture(const GestureProcessorOutput &output, std::int64_t now_us,
                     bool external_power, bool recent_physical_input,
                     bool screen_inactive) {
    if (output.detected == GestureKind::none) return;
    ActivitySettings settings{};
    portENTER_CRITICAL(&snapshot_mux);
    settings = current.settings;
    portEXIT_CRITICAL(&snapshot_mux);
    bool enabled = false;
    switch (output.detected) {
        case GestureKind::raise: enabled = settings.raise_to_wake; break;
        case GestureKind::double_twist:
            enabled = settings.double_twist_quick_settings; break;
        case GestureKind::shake: enabled = settings.shake_notifications; break;
        case GestureKind::flick: enabled = settings.flick_media_next; break;
        case GestureKind::none: break;
    }
    const auto update = update_transport().snapshot();
    const bool critical_overlay =
        clock_service().active_alert() != AlertKind::none ||
        connectivity_service().snapshot().pairing_passkey_active ||
        update.session != 0 || update.awaiting_confirmation || update.ready_to_reboot;
    const auto action = decide_gesture_action({
        .kind = output.detected,
        .enabled = enabled,
        .screen_inactive = screen_inactive,
        .external_power = external_power,
        .recent_physical_input = recent_physical_input,
        .critical_overlay = critical_overlay,
    });
    const bool should_wake = action != GestureAction::none;
    const bool actionable = action != GestureAction::none &&
                            action != GestureAction::wake_only;
    portENTER_CRITICAL(&snapshot_mux);
    current.last_gesture = output.detected;
    current.last_gesture_us = now_us;
    current.last_gesture_strength = output.strength;
    current.last_gesture_actionable = actionable;
    current.last_gesture_screen_inactive = screen_inactive;
    ++current.gesture_sequence;
    switch (output.detected) {
        case GestureKind::raise: ++current.raise_count; break;
        case GestureKind::double_twist: ++current.double_twist_count; break;
        case GestureKind::shake: ++current.shake_count; break;
        case GestureKind::flick: ++current.flick_count; break;
        case GestureKind::none: break;
    }
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    if (should_wake) {
        power_service().note_activity(nightglass::core::WakeReason::motion);
    }
    ESP_LOGI(kTag, "Gesture %s detected strength=%.2f action=%s",
             gesture_name(output.detected), output.strength,
             actionable ? "armed" : should_wake ? "wake-only" : "suppressed");
}

void worker(void *) {
    TickType_t wake = xTaskGetTickCount();
    std::int64_t last_motion_sample_us = 0;
    std::int64_t last_gesture_motion_sample_us = 0;
    bool readiness_announced = false;
    while (true) {
        const auto now_us = esp_timer_get_time();
        Command command{};
        bool force_save = false;
        while (xQueueReceive(command_queue, &command, 0) == pdTRUE) {
            portENTER_CRITICAL(&snapshot_mux);
            if (command.type == CommandType::settings && valid_settings(command.settings)) {
                current.settings = command.settings;
                update_derived(current);
                dirty = true;
                priority_dirty = true;
                force_save = true;
            } else if (command.type == CommandType::reset_today) {
                current.steps_today = 0;
                current.last_step_us = 0;
                update_derived(current);
                dirty = true;
                priority_dirty = true;
                force_save = true;
            }
            ++current.sequence;
            portEXIT_CRITICAL(&snapshot_mux);
        }

        const auto clock = clock_service().snapshot();
        const bool wall_valid = clock.time_valid && valid_civil(clock.local_time);
        const auto local_epoch = wall_valid ? civil_to_epoch(clock.local_time) : 0;
        const auto day = wall_valid ? local_epoch / 86'400 : 0;
        const auto second_of_day = wall_valid
                                       ? static_cast<std::uint32_t>(clock.local_time.hour) * 3600U +
                                             static_cast<std::uint32_t>(clock.local_time.minute) * 60U +
                                             clock.local_time.second
                                       : 0;
        const auto day_update = advance_activity_day(day_state, wall_valid, day,
                                                      second_of_day, now_us);
        if (day_update.reset_count) {
            portENTER_CRITICAL(&snapshot_mux);
            current.steps_today = 0;
            current.last_step_us = 0;
            update_derived(current);
            ++current.sequence;
            portEXIT_CRITICAL(&snapshot_mux);
            dirty = true;
            priority_dirty = true;
            force_save = true;
            ESP_LOGI(kTag, "Daily activity rollover");
        }
        portENTER_CRITICAL(&snapshot_mux);
        if (current.date_valid != day_update.date_valid ||
            current.local_day != day_update.local_day) {
            current.date_valid = day_update.date_valid;
            current.local_day = day_update.local_day;
            ++current.sequence;
        }
        portEXIT_CRITICAL(&snapshot_mux);

        const auto hardware = hardware_service().snapshot();
        const auto motion = hardware.motion;
        if (motion.valid && motion.sampled_at_us > last_gesture_motion_sample_us) {
            ActivitySettings settings{};
            portENTER_CRITICAL(&snapshot_mux);
            settings = current.settings;
            portEXIT_CRITICAL(&snapshot_mux);
            const auto power = power_service().snapshot();
            const bool recent_physical_input =
                now_us - power.last_physical_input_us < 1'000'000;
            const bool external_power = hardware.battery.charging ||
                                        usb_serial_jtag_is_connected();
            const bool screen_inactive = power.state != nightglass::core::PowerState::active;
            const auto gesture = gesture_processor.process({
                .accel_x_g = motion.accel_x_g,
                .accel_y_g = motion.accel_y_g,
                .accel_z_g = motion.accel_z_g,
                .gyro_x_dps = motion.gyro_calibrated ? motion.gyro_corrected_x_dps
                                                     : motion.gyro_raw_x_dps,
                .gyro_y_dps = motion.gyro_calibrated ? motion.gyro_corrected_y_dps
                                                     : motion.gyro_raw_y_dps,
                .gyro_z_dps = motion.gyro_calibrated ? motion.gyro_corrected_z_dps
                                                     : motion.gyro_raw_z_dps,
                .sampled_at_us = motion.sampled_at_us,
                .allow_raise = screen_inactive,
                .gyro_calibrated = motion.gyro_calibrated,
            });
            last_gesture_motion_sample_us = motion.sampled_at_us;
            publish_gesture(gesture, now_us, external_power, recent_physical_input,
                            screen_inactive);
        }
        if (motion.valid && motion.gyro_calibrated &&
            motion.sampled_at_us > last_motion_sample_us) {
            const auto output = processor.process({motion.accel_x_g, motion.accel_y_g,
                                                   motion.accel_z_g, motion.sampled_at_us});
            last_motion_sample_us = motion.sampled_at_us;
            publish_processor(output, motion, now_us);
        } else if (motion.present && motion.valid && !motion.gyro_calibrated) {
            portENTER_CRITICAL(&snapshot_mux);
            current.sensor_present = true;
            current.sample_valid = true;
            current.readiness = ActivityReadiness::warming_up;
            current.calibrated = false;
            current.sampled_at_us = motion.sampled_at_us;
            ++current.sequence;
            portEXIT_CRITICAL(&snapshot_mux);
        } else if (!motion.present || !motion.valid ||
                   now_us - motion.sampled_at_us > kStaleAfterUs) {
            portENTER_CRITICAL(&snapshot_mux);
            current.sensor_present = motion.present;
            current.sample_valid = false;
            current.readiness = motion.present ? ActivityReadiness::stale
                                               : ActivityReadiness::unavailable;
            ++current.sequence;
            portEXIT_CRITICAL(&snapshot_mux);
        }

        ActivitySnapshot copy{};
        portENTER_CRITICAL(&snapshot_mux);
        copy = current;
        portEXIT_CRITICAL(&snapshot_mux);
        if (copy.readiness == ActivityReadiness::ready && !readiness_announced) {
            readiness_announced = true;
            nightglass::core::health_registry().set(
                "activity", copy.persistence_ok ? nightglass::core::HealthState::ok
                                                : nightglass::core::HealthState::degraded,
                copy.persistence_ok ? "Activity calibrated and tracking steps"
                                    : "Activity tracking; NVS persistence unavailable");
            ESP_LOGI(kTag, "Activity calibration complete after %u samples",
                     copy.calibration_samples);
        }
        const bool batch_due = copy.steps_today >= persisted_steps + kSaveStepBatch;
        const bool periodic_due = dirty && now_us - last_save_us >= kPeriodicSaveUs;
        const bool save_due = force_save || priority_dirty || batch_due || periodic_due;
        if (save_due && now_us >= next_save_attempt_us) {
            const bool ok = save_state();
            if (ok) {
                persisted_steps = copy.steps_today;
                last_save_us = now_us;
                next_save_attempt_us = 0;
                dirty = false;
                priority_dirty = false;
            } else {
                // Retain dirty state, but never hammer flash/NVS on a failure.
                next_save_attempt_us = now_us + kSaveRetryBackoffUs;
            }
            set_persistence(ok);
        }
        vTaskDelayUntil(&wake, kPeriod);
    }
}

}  // namespace

nightglass::core::Status ActivityService::start() {
    if (worker_task) return nightglass::core::Status::Ok();
    load_state();
    day_state.fallback_rollover_us = esp_timer_get_time() + 86'400LL * 1'000'000LL;
    processor.reset();
    gesture_processor.reset();
    command_queue = xQueueCreate(4, sizeof(Command));
    if (!command_queue) {
        return {nightglass::core::StatusCode::no_memory, "activity command queue failed"};
    }
    if (xTaskCreatePinnedToCore(worker, "nightglass_activity", 6144, nullptr, 3,
                                &worker_task, 0) != pdPASS) {
        vQueueDelete(command_queue);
        command_queue = nullptr;
        return {nightglass::core::StatusCode::no_memory, "activity task creation failed"};
    }
    portENTER_CRITICAL(&snapshot_mux);
    current.service_started = true;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    nightglass::core::health_registry().set(
        "activity", current.persistence_ok ? nightglass::core::HealthState::degraded
                                            : nightglass::core::HealthState::failed,
        current.persistence_ok ? "Activity sensor warm-up pending"
                               : "Activity started; persisted state unavailable");
    ESP_LOGI(kTag, "Activity service active: step length=%u mm goal=%lu",
             current.settings.step_length_mm,
             static_cast<unsigned long>(current.settings.daily_goal_steps));
    return nightglass::core::Status::Ok();
}

ActivitySnapshot ActivityService::snapshot() const {
    ActivitySnapshot copy{};
    portENTER_CRITICAL(&snapshot_mux);
    copy = current;
    portEXIT_CRITICAL(&snapshot_mux);
    return copy;
}

bool ActivityService::update_settings(const ActivitySettings &settings) {
    return valid_settings(settings) && submit({CommandType::settings, settings});
}

bool ActivityService::reset_today() { return submit({CommandType::reset_today}); }

ActivityService &activity_service() { return instance; }

}  // namespace nightglass::services

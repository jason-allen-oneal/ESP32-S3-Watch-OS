#include "nightglass/services/activity.hpp"

#include <algorithm>
#include <limits>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nightglass/core/health.hpp"
#include "nightglass/services/activity_day.hpp"
#include "nightglass/services/activity_processor.hpp"
#include "nightglass/services/clock.hpp"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/time_math.hpp"

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
    if ((result = nvs_set_u8(handle, "version", 1)) == ESP_OK &&
        // Retain the legacy NVS key so existing calibrated values migrate in place.
        (result = nvs_set_u16(handle, "stride_mm", copy.settings.step_length_mm)) == ESP_OK &&
        (result = nvs_set_u8(handle, "units", static_cast<std::uint8_t>(copy.settings.units))) == ESP_OK &&
        (result = nvs_set_u32(handle, "goal", copy.settings.daily_goal_steps)) == ESP_OK &&
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

void worker(void *) {
    TickType_t wake = xTaskGetTickCount();
    std::int64_t last_motion_sample_us = 0;
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

        const auto motion = hardware_service().snapshot().motion;
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

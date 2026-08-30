#include "nightglass/services/clock.hpp"

#include <algorithm>
#include <limits>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nightglass/core/health.hpp"
#include "nightglass/core/service.hpp"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/power.hpp"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_clock";
constexpr char kNvsNamespace[] = "ng_clock";
constexpr TickType_t kPeriod = pdMS_TO_TICKS(100);
constexpr std::uint32_t kMinTimerSeconds = 60;
constexpr std::uint32_t kMaxTimerSeconds = 24 * 60 * 60;

enum class CommandType : std::uint8_t {
    clock_settings,
    alarm_settings,
    timer_duration,
    timer_toggle,
    timer_reset,
    stopwatch_toggle,
    stopwatch_reset,
    dismiss_alerts,
};

struct Command {
    CommandType type{};
    ClockSettings clock{};
    AlarmSettings alarm{};
    std::uint32_t value{0};
};

ClockService instance;
TaskHandle_t worker_task = nullptr;
QueueHandle_t command_queue = nullptr;
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
ClockSnapshot current{};
std::int64_t timer_deadline_utc = 0;
std::int64_t timer_deadline_mono_us = 0;
std::int64_t stopwatch_started_us = 0;
std::uint64_t stopwatch_base_ms = 0;
std::int64_t last_alarm_day = std::numeric_limits<std::int64_t>::min();

bool valid_settings(const ClockSettings &settings) {
    return settings.utc_offset_minutes >= -12 * 60 && settings.utc_offset_minutes <= 14 * 60 &&
           settings.utc_offset_minutes % 30 == 0;
}

bool valid_alarm(const AlarmSettings &alarm) {
    return alarm.hour <= 23 && alarm.minute <= 59;
}

bool save_state() {
    ClockSnapshot snapshot{};
    std::int64_t deadline{};
    std::int64_t fired_day{};
    portENTER_CRITICAL(&snapshot_mux);
    snapshot = current;
    deadline = timer_deadline_utc;
    fired_day = last_alarm_day;
    portEXIT_CRITICAL(&snapshot_mux);

    nvs_handle_t handle{};
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return false;
    if ((result = nvs_set_u8(handle, "version", 1)) == ESP_OK &&
        (result = nvs_set_u8(handle, "fmt24", snapshot.settings.use_24_hour)) == ESP_OK &&
        (result = nvs_set_u8(handle, "dst", snapshot.settings.daylight_saving)) == ESP_OK &&
        (result = nvs_set_i16(handle, "offset", snapshot.settings.utc_offset_minutes)) == ESP_OK &&
        (result = nvs_set_u8(handle, "alarm_en", snapshot.alarm.enabled)) == ESP_OK &&
        (result = nvs_set_u8(handle, "alarm_h", snapshot.alarm.hour)) == ESP_OK &&
        (result = nvs_set_u8(handle, "alarm_m", snapshot.alarm.minute)) == ESP_OK &&
        (result = nvs_set_i64(handle, "alarm_day", fired_day)) == ESP_OK &&
        (result = nvs_set_u32(handle, "timer_cfg", snapshot.timer_configured_seconds)) == ESP_OK &&
        (result = nvs_set_u32(handle, "timer_rem", snapshot.timer_remaining_seconds)) == ESP_OK &&
        (result = nvs_set_u8(handle, "timer_run", snapshot.timer_running)) == ESP_OK &&
        (result = nvs_set_i64(handle, "timer_due", deadline)) == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result == ESP_OK;
}

void load_state() {
    ClockSnapshot loaded{};
    std::int64_t loaded_deadline = 0;
    std::int64_t loaded_alarm_day = std::numeric_limits<std::int64_t>::min();
    nvs_handle_t handle{};
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        std::uint8_t value8{};
        std::uint32_t value32{};
        std::int16_t value16{};
        if (nvs_get_u8(handle, "fmt24", &value8) == ESP_OK) loaded.settings.use_24_hour = value8;
        if (nvs_get_u8(handle, "dst", &value8) == ESP_OK) loaded.settings.daylight_saving = value8;
        if (nvs_get_i16(handle, "offset", &value16) == ESP_OK) loaded.settings.utc_offset_minutes = value16;
        if (nvs_get_u8(handle, "alarm_en", &value8) == ESP_OK) loaded.alarm.enabled = value8;
        if (nvs_get_u8(handle, "alarm_h", &value8) == ESP_OK) loaded.alarm.hour = value8;
        if (nvs_get_u8(handle, "alarm_m", &value8) == ESP_OK) loaded.alarm.minute = value8;
        nvs_get_i64(handle, "alarm_day", &loaded_alarm_day);
        if (nvs_get_u32(handle, "timer_cfg", &value32) == ESP_OK) loaded.timer_configured_seconds = value32;
        if (nvs_get_u32(handle, "timer_rem", &value32) == ESP_OK) loaded.timer_remaining_seconds = value32;
        if (nvs_get_u8(handle, "timer_run", &value8) == ESP_OK) loaded.timer_running = value8;
        nvs_get_i64(handle, "timer_due", &loaded_deadline);
        nvs_close(handle);
    }
    if (!valid_settings(loaded.settings)) loaded.settings = {};
    if (!valid_alarm(loaded.alarm)) loaded.alarm = {};
    if (loaded.timer_configured_seconds < kMinTimerSeconds ||
        loaded.timer_configured_seconds > kMaxTimerSeconds) {
        loaded.timer_configured_seconds = 300;
    }
    if (loaded.timer_remaining_seconds == 0 ||
        loaded.timer_remaining_seconds > kMaxTimerSeconds) {
        loaded.timer_remaining_seconds = loaded.timer_configured_seconds;
    }
    if (loaded.timer_running && loaded_deadline <= 0) loaded.timer_running = false;
    loaded.persistence_ok = true;
    current = loaded;
    timer_deadline_utc = loaded_deadline;
    // The RTC service may not have delivered its first sample yet. Preserve a
    // restored running countdown on the monotonic clock until UTC becomes
    // available; once it does, the persisted UTC deadline remains authoritative.
    timer_deadline_mono_us = loaded.timer_running
                                 ? esp_timer_get_time() +
                                       static_cast<std::int64_t>(
                                           loaded.timer_remaining_seconds) *
                                           1'000'000
                                 : 0;
    last_alarm_day = loaded_alarm_day;
}

bool submit(const Command &command) {
    return command_queue && xQueueSend(command_queue, &command, 0) == pdTRUE;
}

void mark_persistence(bool ok) {
    portENTER_CRITICAL(&snapshot_mux);
    current.persistence_ok = ok;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    if (!ok) {
        nightglass::core::health_registry().set(
            "clock", nightglass::core::HealthState::degraded,
            "Clock state active but NVS persistence failed");
    }
}

void handle_command(const Command &command, std::int64_t now_us) {
    bool persist = false;
    portENTER_CRITICAL(&snapshot_mux);
    switch (command.type) {
        case CommandType::clock_settings:
            if (valid_settings(command.clock)) {
                current.settings = command.clock;
                persist = true;
            }
            break;
        case CommandType::alarm_settings:
            if (valid_alarm(command.alarm)) {
                current.alarm = command.alarm;
                current.alarm_ringing = false;
                persist = true;
            }
            break;
        case CommandType::timer_duration:
            if (command.value >= kMinTimerSeconds && command.value <= kMaxTimerSeconds) {
                current.timer_configured_seconds = command.value;
                current.timer_remaining_seconds = command.value;
                current.timer_running = false;
                current.timer_ringing = false;
                timer_deadline_utc = 0;
                timer_deadline_mono_us = 0;
                persist = true;
            }
            break;
        case CommandType::timer_toggle:
            if (current.timer_running) {
                current.timer_running = false;
                timer_deadline_utc = 0;
                timer_deadline_mono_us = 0;
            } else {
                if (current.timer_remaining_seconds == 0) {
                    current.timer_remaining_seconds = current.timer_configured_seconds;
                }
                current.timer_running = true;
                current.timer_ringing = false;
                if (current.time_valid) {
                    timer_deadline_utc = current.utc_epoch_seconds + current.timer_remaining_seconds;
                    timer_deadline_mono_us = 0;
                } else {
                    timer_deadline_utc = 0;
                    timer_deadline_mono_us = now_us +
                        static_cast<std::int64_t>(current.timer_remaining_seconds) * 1'000'000;
                }
            }
            persist = true;
            break;
        case CommandType::timer_reset:
            current.timer_running = false;
            current.timer_ringing = false;
            current.timer_remaining_seconds = current.timer_configured_seconds;
            timer_deadline_utc = 0;
            timer_deadline_mono_us = 0;
            persist = true;
            break;
        case CommandType::stopwatch_toggle:
            if (current.stopwatch_running) {
                stopwatch_base_ms = current.stopwatch_elapsed_ms;
                current.stopwatch_running = false;
                stopwatch_started_us = 0;
            } else {
                current.stopwatch_running = true;
                stopwatch_base_ms = current.stopwatch_elapsed_ms;
                stopwatch_started_us = now_us;
            }
            break;
        case CommandType::stopwatch_reset:
            current.stopwatch_running = false;
            current.stopwatch_elapsed_ms = 0;
            stopwatch_base_ms = 0;
            stopwatch_started_us = 0;
            break;
        case CommandType::dismiss_alerts:
            current.alarm_ringing = false;
            current.timer_ringing = false;
            break;
    }
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    if (persist) mark_persistence(save_state());
}

void worker(void *) {
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        Command command{};
        while (xQueueReceive(command_queue, &command, 0) == pdTRUE) {
            handle_command(command, esp_timer_get_time());
        }

        const auto now_us = esp_timer_get_time();
        const auto hardware = hardware_service().snapshot();
        const auto &rtc = hardware.rtc;
        const bool rtc_fresh = rtc.present && rtc.valid && rtc.sampled_at_us > 0 &&
                               now_us - rtc.sampled_at_us <= 5'000'000;
        bool alert_started = false;
        bool persist_after_tick = false;

        portENTER_CRITICAL(&snapshot_mux);
        if (rtc_fresh) {
            const CivilTime utc{static_cast<std::int32_t>(rtc.year), rtc.month, rtc.day,
                                rtc.weekday, rtc.hour, rtc.minute, rtc.second};
            if (valid_civil(utc)) {
                current.time_valid = true;
                current.utc_epoch_seconds = civil_to_epoch(utc) +
                    std::max<std::int64_t>(0, (now_us - rtc.sampled_at_us) / 1'000'000);
                const auto total_offset = current.settings.utc_offset_minutes +
                                          (current.settings.daylight_saving ? 60 : 0);
                current.local_time = epoch_to_civil(
                    current.utc_epoch_seconds + static_cast<std::int64_t>(total_offset) * 60);
            }
        } else {
            current.time_valid = false;
        }

        if (current.alarm.enabled && current.time_valid) {
            const auto local_epoch = civil_to_epoch(current.local_time);
            const auto day = local_epoch / 86400;
            if (current.local_time.hour == current.alarm.hour &&
                current.local_time.minute == current.alarm.minute &&
                current.local_time.second <= 1 && day != last_alarm_day) {
                current.alarm_ringing = true;
                last_alarm_day = day;
                alert_started = true;
                persist_after_tick = true;
            }
        }

        if (current.timer_running) {
            std::int64_t remaining_us = 0;
            bool deadline_available = false;
            if (timer_deadline_utc > 0 && current.time_valid) {
                remaining_us = (timer_deadline_utc - current.utc_epoch_seconds) * 1'000'000;
                deadline_available = true;
            } else if (timer_deadline_mono_us > 0) {
                remaining_us = timer_deadline_mono_us - now_us;
                deadline_available = true;
            }
            if (deadline_available && remaining_us <= 0) {
                current.timer_running = false;
                current.timer_remaining_seconds = 0;
                current.timer_ringing = true;
                timer_deadline_utc = 0;
                timer_deadline_mono_us = 0;
                alert_started = true;
                persist_after_tick = true;
            } else if (deadline_available && remaining_us > 0) {
                current.timer_remaining_seconds = static_cast<std::uint32_t>(
                    (remaining_us + 999'999) / 1'000'000);
            }
        }

        if (current.stopwatch_running) {
            current.stopwatch_elapsed_ms = stopwatch_base_ms +
                static_cast<std::uint64_t>((now_us - stopwatch_started_us) / 1000);
        }
        const bool alarm_ringing = current.alarm_ringing;
        const bool timer_ringing = current.timer_ringing;
        ++current.sequence;
        portEXIT_CRITICAL(&snapshot_mux);

        if (persist_after_tick) mark_persistence(save_state());
        if (alert_started) {
            power_service().note_activity(alarm_ringing
                                              ? nightglass::core::WakeReason::alarm
                                              : nightglass::core::WakeReason::timer);
            ESP_LOGI(kTag, "Visual alert started alarm=%d timer=%d",
                     alarm_ringing, timer_ringing);
        }
        vTaskDelayUntil(&wake, kPeriod);
    }
}

}  // namespace

nightglass::core::Status ClockService::start() {
    if (worker_task) return nightglass::core::Status::Ok();
    load_state();
    command_queue = xQueueCreate(8, sizeof(Command));
    if (!command_queue) {
        return {nightglass::core::StatusCode::no_memory, "clock command queue failed"};
    }
    if (xTaskCreatePinnedToCore(worker, "nightglass_clock", 6144, nullptr, 4,
                                &worker_task, 0) != pdPASS) {
        vQueueDelete(command_queue);
        command_queue = nullptr;
        return {nightglass::core::StatusCode::no_memory, "clock task creation failed"};
    }
    nightglass::core::health_registry().set(
        "clock", nightglass::core::HealthState::ok,
        "Clock settings, alarm, countdown, and stopwatch active");
    ESP_LOGI(kTag, "Clock service active; alerts are visual-only");
    return nightglass::core::Status::Ok();
}

ClockSnapshot ClockService::snapshot() const {
    ClockSnapshot copy{};
    portENTER_CRITICAL(&snapshot_mux);
    copy = current;
    portEXIT_CRITICAL(&snapshot_mux);
    return copy;
}

AlertKind ClockService::active_alert() const {
    const auto copy = snapshot();
    if (copy.alarm_ringing && copy.timer_ringing) return AlertKind::both;
    if (copy.alarm_ringing) return AlertKind::alarm;
    if (copy.timer_ringing) return AlertKind::countdown;
    return AlertKind::none;
}

std::int64_t ClockService::next_wake_delay_us() const {
    const auto copy = snapshot();
    std::int64_t delay_us = std::numeric_limits<std::int64_t>::max();
    portENTER_CRITICAL(&snapshot_mux);
    const auto utc_deadline = timer_deadline_utc;
    const auto mono_deadline = timer_deadline_mono_us;
    portEXIT_CRITICAL(&snapshot_mux);
    if (copy.timer_running) {
        if (copy.time_valid && utc_deadline > 0) {
            delay_us = std::min(
                delay_us,
                (utc_deadline - copy.utc_epoch_seconds) * 1'000'000);
        } else if (mono_deadline > 0) {
            delay_us = std::min(delay_us, mono_deadline - esp_timer_get_time());
        }
    }
    if (copy.alarm.enabled && copy.time_valid) {
        const auto offset = copy.settings.utc_offset_minutes +
                            (copy.settings.daylight_saving ? 60 : 0);
        const auto local_epoch = copy.utc_epoch_seconds + static_cast<std::int64_t>(offset) * 60;
        auto target = (local_epoch / 86400) * 86400 +
                      static_cast<std::int64_t>(copy.alarm.hour) * 3600 +
                      static_cast<std::int64_t>(copy.alarm.minute) * 60;
        if (target <= local_epoch) target += 86400;
        delay_us = std::min(delay_us, (target - local_epoch) * 1'000'000);
    }
    if (delay_us == std::numeric_limits<std::int64_t>::max()) return 0;
    return std::max<std::int64_t>(1'000, delay_us);
}

bool ClockService::update_clock_settings(const ClockSettings &settings) {
    return valid_settings(settings) && submit({CommandType::clock_settings, settings, {}, 0});
}

bool ClockService::update_alarm(const AlarmSettings &alarm) {
    return valid_alarm(alarm) && submit({CommandType::alarm_settings, {}, alarm, 0});
}

bool ClockService::set_timer_duration(std::uint32_t seconds) {
    return submit({CommandType::timer_duration, {}, {}, seconds});
}

bool ClockService::toggle_timer() { return submit({CommandType::timer_toggle}); }
bool ClockService::reset_timer() { return submit({CommandType::timer_reset}); }
bool ClockService::toggle_stopwatch() { return submit({CommandType::stopwatch_toggle}); }
bool ClockService::reset_stopwatch() { return submit({CommandType::stopwatch_reset}); }
bool ClockService::dismiss_alerts() { return submit({CommandType::dismiss_alerts}); }

ClockService &clock_service() { return instance; }

}  // namespace nightglass::services

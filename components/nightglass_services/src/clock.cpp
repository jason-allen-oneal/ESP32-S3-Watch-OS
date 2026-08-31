#include "nightglass/services/clock.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nightglass/core/health.hpp"
#include "nightglass/core/service.hpp"
#include "nightglass/services/audio.hpp"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/power.hpp"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_clock";
constexpr char kNvsNamespace[] = "ng_clock";
constexpr TickType_t kPeriod = pdMS_TO_TICKS(100);
constexpr std::int64_t kAlertAudioRepeatUs = 4'000'000;
constexpr std::int64_t kAlertAudioRetryUs = 1'000'000;
constexpr std::uint32_t kMinTimerSeconds = 60;
constexpr std::uint32_t kMaxTimerSeconds = 24 * 60 * 60;

enum class CommandType : std::uint8_t {
    clock_settings,
    alarm_settings,
    quiet_hours,
    alarm_snooze,
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
    QuietHoursSettings quiet{};
    std::uint8_t index{0};
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
std::array<std::int64_t, kAlarmCapacity> last_alarm_days{};
std::int64_t snooze_deadline_utc = 0;
std::uint8_t snoozed_alarm_index = kNoAlarmIndex;

bool valid_settings(const ClockSettings &settings) {
    return settings.utc_offset_minutes >= -12 * 60 && settings.utc_offset_minutes <= 14 * 60 &&
           settings.utc_offset_minutes % 30 == 0;
}

bool valid_alarm(const AlarmSettings &alarm) { return valid_alarm_settings(alarm); }

bool save_state() {
    ClockSnapshot snapshot{};
    std::int64_t deadline{};
    std::array<std::int64_t, kAlarmCapacity> fired_days{};
    std::int64_t snooze_deadline{};
    std::uint8_t snooze_index{};
    portENTER_CRITICAL(&snapshot_mux);
    snapshot = current;
    deadline = timer_deadline_utc;
    fired_days = last_alarm_days;
    snooze_deadline = snooze_deadline_utc;
    snooze_index = snoozed_alarm_index;
    portEXIT_CRITICAL(&snapshot_mux);

    nvs_handle_t handle{};
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return false;
    if ((result = nvs_set_u8(handle, "version", 2)) == ESP_OK &&
        (result = nvs_set_u8(handle, "fmt24", snapshot.settings.use_24_hour)) == ESP_OK &&
        (result = nvs_set_u8(handle, "dst", snapshot.settings.daylight_saving)) == ESP_OK &&
        (result = nvs_set_i16(handle, "offset", snapshot.settings.utc_offset_minutes)) == ESP_OK &&
        (result = nvs_set_blob(handle, "alarms", snapshot.alarms.data(),
                               sizeof(snapshot.alarms))) == ESP_OK &&
        (result = nvs_set_blob(handle, "alarm_days", fired_days.data(),
                               sizeof(fired_days))) == ESP_OK &&
        (result = nvs_set_blob(handle, "quiet", &snapshot.quiet_hours,
                               sizeof(snapshot.quiet_hours))) == ESP_OK &&
        (result = nvs_set_i64(handle, "snooze_due", snooze_deadline)) == ESP_OK &&
        (result = nvs_set_u8(handle, "snooze_idx", snooze_index)) == ESP_OK &&
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
    std::array<std::int64_t, kAlarmCapacity> loaded_alarm_days{};
    loaded_alarm_days.fill(std::numeric_limits<std::int64_t>::min());
    std::int64_t loaded_snooze_deadline = 0;
    std::uint8_t loaded_snooze_index = kNoAlarmIndex;
    constexpr std::array<const char *, kAlarmCapacity> labels{
        "Wake up", "Work", "Medication", "Exercise"};
    for (std::size_t index = 0; index < loaded.alarms.size(); ++index) {
        loaded.alarms[index].hour = static_cast<std::uint8_t>(7 + index);
        loaded.alarms[index].repeat_days = kEveryDayMask;
        std::strncpy(loaded.alarms[index].label.data(), labels[index],
                     loaded.alarms[index].label.size() - 1);
    }
    nvs_handle_t handle{};
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        std::uint8_t value8{};
        std::uint32_t value32{};
        std::int16_t value16{};
        if (nvs_get_u8(handle, "fmt24", &value8) == ESP_OK) loaded.settings.use_24_hour = value8;
        if (nvs_get_u8(handle, "dst", &value8) == ESP_OK) loaded.settings.daylight_saving = value8;
        if (nvs_get_i16(handle, "offset", &value16) == ESP_OK) loaded.settings.utc_offset_minutes = value16;
        std::size_t alarm_size = sizeof(loaded.alarms);
        const bool alarms_loaded = nvs_get_blob(handle, "alarms", loaded.alarms.data(),
                                                &alarm_size) == ESP_OK &&
                                   alarm_size == sizeof(loaded.alarms);
        if (!alarms_loaded) {
            if (nvs_get_u8(handle, "alarm_en", &value8) == ESP_OK)
                loaded.alarms[0].enabled = value8;
            if (nvs_get_u8(handle, "alarm_h", &value8) == ESP_OK)
                loaded.alarms[0].hour = value8;
            if (nvs_get_u8(handle, "alarm_m", &value8) == ESP_OK)
                loaded.alarms[0].minute = value8;
            nvs_get_i64(handle, "alarm_day", &loaded_alarm_days[0]);
        } else {
            std::size_t days_size = sizeof(loaded_alarm_days);
            nvs_get_blob(handle, "alarm_days", loaded_alarm_days.data(), &days_size);
        }
        std::size_t quiet_size = sizeof(loaded.quiet_hours);
        nvs_get_blob(handle, "quiet", &loaded.quiet_hours, &quiet_size);
        nvs_get_i64(handle, "snooze_due", &loaded_snooze_deadline);
        nvs_get_u8(handle, "snooze_idx", &loaded_snooze_index);
        if (nvs_get_u32(handle, "timer_cfg", &value32) == ESP_OK) loaded.timer_configured_seconds = value32;
        if (nvs_get_u32(handle, "timer_rem", &value32) == ESP_OK) loaded.timer_remaining_seconds = value32;
        if (nvs_get_u8(handle, "timer_run", &value8) == ESP_OK) loaded.timer_running = value8;
        nvs_get_i64(handle, "timer_due", &loaded_deadline);
        nvs_close(handle);
    }
    if (!valid_settings(loaded.settings)) loaded.settings = {};
    for (auto &alarm : loaded.alarms) {
        if (!valid_alarm(alarm)) alarm = {};
    }
    if (!valid_quiet_hours(loaded.quiet_hours)) loaded.quiet_hours = {};
    loaded.alarm = loaded.alarms[0];
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
    last_alarm_days = loaded_alarm_days;
    snooze_deadline_utc = loaded_snooze_deadline;
    snoozed_alarm_index = loaded_snooze_index < kAlarmCapacity
                              ? loaded_snooze_index : kNoAlarmIndex;
    loaded.alarm_snoozed = snooze_deadline_utc > 0 &&
                           snoozed_alarm_index != kNoAlarmIndex;
    current = loaded;
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
            if (command.index < current.alarms.size() && valid_alarm(command.alarm)) {
                current.alarms[command.index] = command.alarm;
                current.alarm = current.alarms[0];
                if (current.ringing_alarm_index == command.index) {
                    current.alarm_ringing = false;
                    current.ringing_alarm_index = kNoAlarmIndex;
                }
                persist = true;
            }
            break;
        case CommandType::quiet_hours:
            if (valid_quiet_hours(command.quiet)) {
                current.quiet_hours = command.quiet;
                persist = true;
            }
            break;
        case CommandType::alarm_snooze:
            if (current.alarm_ringing && current.time_valid && command.value >= 1 &&
                command.value <= 60 && current.ringing_alarm_index < kAlarmCapacity) {
                snoozed_alarm_index = current.ringing_alarm_index;
                snooze_deadline_utc = current.utc_epoch_seconds +
                                      static_cast<std::int64_t>(command.value) * 60;
                current.snooze_minutes = static_cast<std::uint16_t>(command.value);
                current.alarm_snoozed = true;
                current.alarm_ringing = false;
                current.ringing_alarm_index = kNoAlarmIndex;
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
            current.ringing_alarm_index = kNoAlarmIndex;
            current.alarm_snoozed = false;
            snooze_deadline_utc = 0;
            snoozed_alarm_index = kNoAlarmIndex;
            current.timer_ringing = false;
            persist = true;
            break;
    }
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    if (persist) mark_persistence(save_state());
}

void worker(void *) {
    TickType_t wake = xTaskGetTickCount();
    std::int64_t next_alert_audio_us = 0;
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

        if (current.time_valid) {
            const auto minute_of_day = static_cast<std::uint16_t>(
                current.local_time.hour * 60U + current.local_time.minute);
            current.quiet_hours_active =
                quiet_hours_active(current.quiet_hours, minute_of_day);
            const auto local_epoch = civil_to_epoch(current.local_time);
            const auto day = local_epoch / 86400;
            for (std::size_t index = 0; index < current.alarms.size(); ++index) {
                const auto &alarm = current.alarms[index];
                if (alarm.enabled && alarm_runs_on_weekday(alarm, current.local_time.weekday) &&
                    current.local_time.hour == alarm.hour &&
                    current.local_time.minute == alarm.minute &&
                    current.local_time.second <= 1 && day != last_alarm_days[index]) {
                    current.alarm_ringing = true;
                    current.ringing_alarm_index = static_cast<std::uint8_t>(index);
                    last_alarm_days[index] = day;
                    alert_started = true;
                    persist_after_tick = true;
                    break;
                }
            }
            if (snooze_deadline_utc > 0 && current.utc_epoch_seconds >= snooze_deadline_utc &&
                snoozed_alarm_index < current.alarms.size()) {
                current.alarm_ringing = true;
                current.ringing_alarm_index = snoozed_alarm_index;
                current.alarm_snoozed = false;
                snooze_deadline_utc = 0;
                snoozed_alarm_index = kNoAlarmIndex;
                alert_started = true;
                persist_after_tick = true;
            }
        } else {
            current.quiet_hours_active = false;
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
        const bool scheduled_dnd = current.quiet_hours_active;
        ++current.sequence;
        portEXIT_CRITICAL(&snapshot_mux);

        audio_service().set_scheduled_dnd(scheduled_dnd);

        if (persist_after_tick) mark_persistence(save_state());
        if (alert_started) {
            power_service().note_activity(alarm_ringing
                                              ? nightglass::core::WakeReason::alarm
                                              : nightglass::core::WakeReason::timer);
            ESP_LOGI(kTag, "Alert started alarm=%d timer=%d",
                     alarm_ringing, timer_ringing);
        }
        if ((alarm_ringing || timer_ringing) &&
            (alert_started || now_us >= next_alert_audio_us)) {
            const auto audio_status = audio_service().request_sound(
                alarm_ringing ? SoundCue::alarm : SoundCue::timer);
            if (audio_status.is_ok()) {
                next_alert_audio_us = now_us + kAlertAudioRepeatUs;
            } else {
                next_alert_audio_us = now_us + kAlertAudioRetryUs;
                nightglass::core::health_registry().set(
                    "clock", nightglass::core::HealthState::degraded,
                    "visual alert active; audio cue unavailable");
                ESP_LOGW(kTag, "Alert audio enqueue failed status=%u",
                         static_cast<unsigned>(audio_status.code));
            }
        } else if (!alarm_ringing && !timer_ringing) {
            next_alert_audio_us = 0;
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
        "Clock settings, alarm, countdown, stopwatch, and audio cues active");
    ESP_LOGI(kTag, "Clock service active; alarm/timer cues enabled");
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
    const auto alarm_snooze_deadline = snooze_deadline_utc;
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
    if (copy.time_valid) {
        const auto offset = copy.settings.utc_offset_minutes +
                            (copy.settings.daylight_saving ? 60 : 0);
        const auto local_epoch = copy.utc_epoch_seconds + static_cast<std::int64_t>(offset) * 60;
        for (const auto &alarm : copy.alarms) {
            if (!alarm.enabled) continue;
            for (std::uint8_t day_offset = 0; day_offset < 8; ++day_offset) {
                const auto day_epoch = (local_epoch / 86400 + day_offset) * 86400;
                const auto weekday = epoch_to_civil(day_epoch).weekday;
                if (!alarm_runs_on_weekday(alarm, weekday)) continue;
                const auto target = day_epoch + static_cast<std::int64_t>(alarm.hour) * 3600 +
                                    static_cast<std::int64_t>(alarm.minute) * 60;
                if (target > local_epoch) {
                    delay_us = std::min(delay_us, (target - local_epoch) * 1'000'000);
                    break;
                }
            }
        }
        if (alarm_snooze_deadline > copy.utc_epoch_seconds) {
            delay_us = std::min(delay_us,
                                (alarm_snooze_deadline - copy.utc_epoch_seconds) * 1'000'000);
        }
    }
    if (delay_us == std::numeric_limits<std::int64_t>::max()) return 0;
    return std::max<std::int64_t>(1'000, delay_us);
}

bool ClockService::update_clock_settings(const ClockSettings &settings) {
    Command command{};
    command.type = CommandType::clock_settings;
    command.clock = settings;
    return valid_settings(settings) && submit(command);
}

bool ClockService::update_alarm(const AlarmSettings &alarm) {
    return update_alarm(0, alarm);
}

bool ClockService::update_alarm(std::size_t index, const AlarmSettings &alarm) {
    Command command{};
    command.type = CommandType::alarm_settings;
    command.alarm = alarm;
    command.index = static_cast<std::uint8_t>(index);
    return index < kAlarmCapacity && valid_alarm(alarm) && submit(command);
}

bool ClockService::update_quiet_hours(const QuietHoursSettings &settings) {
    Command command{};
    command.type = CommandType::quiet_hours;
    command.quiet = settings;
    return valid_quiet_hours(settings) && submit(command);
}

bool ClockService::snooze_alarm(std::uint16_t minutes) {
    Command command{};
    command.type = CommandType::alarm_snooze;
    command.value = minutes;
    return minutes >= 1 && minutes <= 60 && submit(command);
}

bool ClockService::set_timer_duration(std::uint32_t seconds) {
    Command command{};
    command.type = CommandType::timer_duration;
    command.value = seconds;
    return submit(command);
}

bool ClockService::toggle_timer() { return submit({CommandType::timer_toggle}); }
bool ClockService::reset_timer() { return submit({CommandType::timer_reset}); }
bool ClockService::toggle_stopwatch() { return submit({CommandType::stopwatch_toggle}); }
bool ClockService::reset_stopwatch() { return submit({CommandType::stopwatch_reset}); }
bool ClockService::dismiss_alerts() { return submit({CommandType::dismiss_alerts}); }

ClockService &clock_service() { return instance; }

}  // namespace nightglass::services

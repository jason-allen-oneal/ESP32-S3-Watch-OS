#!/usr/bin/env python3
"""Exercise actual hardware-task bounded recovery with hardware/RTOS doubles.

Compiles the production register helpers, diagnostic, and full hardware_task
body. The task exits only through its doubled notification wait, after normal
polling has resumed. BSP sampling is a boundary double; its separate regression
must verify the real I2C/lock implementation. This is hardware_task_only coverage,
not proof of global bus exclusion, controller recovery, or physical stability.

Optionally compare --baseline hardware.cpp to prove that the task body outside
the single diagnostic invocation is unchanged. No device is accessed.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def definition(source: str, signature: str) -> str:
    """Take a real C++ definition, ignoring braces in strings and comments."""
    match = re.search(re.escape(signature) + r"[^;{}]*\{", source)
    if match is None:
        raise AssertionError(f"missing production definition: {signature}")
    token = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[0-9]+(?:\'[0-9]+)*|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for item in token.finditer(source, match.end() - 1):
        if item[0] == "{":
            depth += 1
        elif item[0] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():item.end()]
    raise AssertionError(f"unterminated production definition: {signature}")


def declaration(source: str, name: str) -> str:
    match = re.search(r"^constexpr[^\n;]*\b" + re.escape(name) + r"\b[^;]*;", source, re.M)
    if match is None:
        raise AssertionError(f"missing production constant: {name}")
    return match[0]


PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "nightglass/bsp/touch_bus_diagnostic.hpp"

using TickType_t = unsigned int;
using i2c_master_dev_handle_t = void *;
using i2c_master_bus_handle_t = void *;
constexpr int pdTRUE = 1, GPIO_NUM_18 = 18;
using gpio_num_t = int;
std::int64_t clock_us = 0;
bool lvgl_locked = false;
std::vector<std::string> events, logs;
int checks = 0;
void require(bool condition, const std::string &message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::int64_t esp_timer_get_time() { return clock_us; }
const char *esp_err_to_name(esp_err_t result) {
    switch (result) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        default: return "UNKNOWN_ERROR";
    }
}
void record_log(const char *tag, const char *format, ...) {
    require(!lvgl_locked, "hardware diagnostic logged while LVGL lock was held");
    require(std::string(tag) == "nightglass_hw", "unexpected log tag");
    char buffer[1536];
    va_list arguments;
    va_start(arguments, format);
    const auto count = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    require(count >= 0 && std::size_t(count) < sizeof(buffer), "diagnostic log truncated");
    logs.emplace_back(buffer);
    if (logs.back().find("TOUCH_BUS_RECOVERY_BEGIN") == 0) events.emplace_back("quiet_begin");
    if (logs.back().find("TOUCH_BUS_RECOVERY_END") == 0) events.emplace_back("quiet_end");
}
#define ESP_LOGI(tag, ...) record_log(tag, __VA_ARGS__)

constexpr TickType_t tick_ms = 10;
constexpr TickType_t pdMS_TO_TICKS(std::uint32_t milliseconds) { return milliseconds / tick_ms; }
int delay_calls = 0;
void vTaskDelay(TickType_t ticks) {
    require(!lvgl_locked, "hardware task slept with LVGL lock held");
    require(ticks > 0, "zero-tick diagnostic busy loop");
    ++delay_calls;
    require(delay_calls < 1000, "diagnostic window failed to terminate");
    events.emplace_back("quiet_delay");
    clock_us += std::int64_t(ticks) * tick_ms * 1000;
}

int rtc_storage, pmic_storage, imu_storage, unknown_storage, bus_storage;
i2c_master_dev_handle_t rtc_device = &rtc_storage;
i2c_master_dev_handle_t pmic_device = &pmic_storage;
i2c_master_dev_handle_t imu_device = &imu_storage;
esp_err_t transfer_result = ESP_OK;
int reads = 0, writes = 0;
i2c_master_dev_handle_t observed_device = nullptr;
std::vector<std::uint8_t> observed_tx;
std::size_t observed_rx_size = 0;
int observed_timeout = 0;
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
    const std::uint8_t *tx, std::size_t tx_size, std::uint8_t *rx,
    std::size_t rx_size, int timeout) {
    ++reads;
    observed_device = device;
    observed_tx.assign(tx, tx + tx_size);
    observed_rx_size = rx_size;
    observed_timeout = timeout;
    if (transfer_result == ESP_OK) std::fill(rx, rx + rx_size, 0xa6);
    clock_us += 731;
    return transfer_result;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
    const std::uint8_t *tx, std::size_t tx_size, int timeout) {
    ++writes;
    observed_device = device;
    observed_tx.assign(tx, tx + tx_size);
    observed_rx_size = 0;
    observed_timeout = timeout;
    clock_us += 977;
    return transfer_result;
}

struct AudioSnapshot { bool hardware_active = false, operation_pending = false; };
int audio_calls = 0, audio_change_on_call = 0;
AudioSnapshot initial_audio{}, changed_audio{};
struct AudioDouble {
    AudioSnapshot snapshot() {
        ++audio_calls;
        return audio_change_on_call && audio_calls >= audio_change_on_call
            ? changed_audio : initial_audio;
    }
};
AudioDouble &audio_service() { static AudioDouble audio; return audio; }

bool request_pending = false;
int take_calls = 0, irq_calls = 0, irq_change_on_call = 0, sample_calls = 0;
std::uint32_t irq_value = 77;
bool irq_during_sample = false;
std::int64_t sample_duration_us = 0;
nightglass::bsp::TouchBusQuietSample sample_template;
int activation_calls = 0, request_read_calls = 0;
esp_err_t activation_result = ESP_OK, ordinary_read_result = ESP_OK;
bool ordinary_read_completes = true;
nightglass::bsp::TouchReadEvidence ordinary_evidence{};
int motion_polls = 0, rtc_polls = 0, battery_polls = 0;
namespace nightglass::bsp {
esp_err_t activate_touch_from_monitor() {
    ++activation_calls;
    require(!lvgl_locked, "hardware caller held LVGL lock before activation");
    if (activation_result == ESP_OK) sample_template.power_mode = 0;
    return activation_result;
}
TouchReadEvidence touch_read_evidence() { return ordinary_evidence; }
void request_touch_read() {
    ++request_read_calls;
    require(!lvgl_locked, "hardware caller held LVGL lock while posting normal read");
    // Model completion of the separate ordinary LVGL callback. The real BSP
    // regression checks this event and the production callback's health owner.
    if (ordinary_read_completes) {
        ++ordinary_evidence.sequence;
        ordinary_evidence.result = ordinary_read_result;
        ordinary_evidence.completed_us = clock_us;
    }
}
bool take_touch_bus_quiet_request() {
    ++take_calls;
    const bool answer = request_pending;
    request_pending = false;
    events.emplace_back("take_request");
    return answer;
}
std::uint32_t touch_irq_sequence() {
    ++irq_calls;
    if (irq_change_on_call && irq_calls == irq_change_on_call) ++irq_value;
    return irq_value;
}
TouchBusQuietSample sample_touch_bus_quiet_diagnostic() {
    ++sample_calls;
    require(!lvgl_locked, "caller already held LVGL lock at sample boundary");
    require(motion_polls == 0 && rtc_polls == 0 && battery_polls == 0,
            "normal hardware polling ran before the quiet diagnostic finished");
    require(sample_calls < 1000, "diagnostic sampled without a finite time bound");
    events.emplace_back("quiet_sample");
    auto result = sample_template;
    // The real BSP boundary has its own regression. Model a bounded call that
    // releases its lock before returning, without changing any health state.
    lvgl_locked = result.locked;
    clock_us += sample_duration_us;
    if (irq_during_sample) ++irq_value;
    result.irq_sequence = irq_value;
    result.sampled_at_us = clock_us;
    lvgl_locked = false;
    return result;
}
}
'''


TASK_BOUNDARIES = r'''
namespace nightglass::core {
enum class HealthState { degraded };
enum class PowerState { active, screen_blank, light_sleep };
struct HealthDouble {
    void set(const char *, HealthState, const char *) {
        throw std::runtime_error("unexpected health mutation in diagnostic task scenario");
    }
};
HealthDouble &health_registry() { static HealthDouble health; return health; }
}
struct PowerSnapshot { nightglass::core::PowerState state; };
nightglass::core::PowerState power_state = nightglass::core::PowerState::active;
struct PowerDouble { PowerSnapshot snapshot() { return {power_state}; } };
PowerDouble &power_service() { static PowerDouble power; return power; }
std::atomic_bool diagnostics_gyro_requested{false}, gyro_enabled{false};
std::int64_t last_gyro_policy_attempt_us = -1'000'000;
bool imu_ready = false;
void *haptic_queue = nullptr, *haptic_timer = nullptr;
int probe_calls = 0, notify_calls = 0, task_iterations = 2;
std::vector<TickType_t> notification_ticks;
struct StopTask {};
void probe_devices(i2c_master_bus_handle_t bus) {
    require(bus == &bus_storage, "hardware task lost its configured I2C bus");
    ++probe_calls;
    events.emplace_back("probe_devices");
}
bool gestures_require_gyro() { return false; }
bool apply_gyro_policy(bool) { throw std::runtime_error("unexpected gyro policy change"); }
int xQueueReceive(void *, HapticCommand *, int) { return 0; }
esp_err_t esp_timer_stop(void *) { throw std::runtime_error("diagnostic changed a timer"); }
esp_err_t esp_timer_start_once(void *, std::uint64_t) {
    throw std::runtime_error("diagnostic changed a timer");
}
esp_err_t gpio_set_level(gpio_num_t, int) { throw std::runtime_error("diagnostic changed GPIO"); }
void set_haptic_state(bool, std::int64_t, bool) { throw std::runtime_error("haptic mutation"); }
void publish_haptic_failure() { throw std::runtime_error("haptic failure mutation"); }
void publish_motion() { ++motion_polls; events.emplace_back("motion"); }
void publish_rtc() { ++rtc_polls; events.emplace_back("rtc"); }
void publish_battery() { ++battery_polls; events.emplace_back("battery"); }
std::uint32_t ulTaskNotifyTake(int clear, TickType_t ticks) {
    require(clear == pdTRUE, "normal hardware notification semantics changed");
    require(!lvgl_locked, "hardware task blocked with LVGL lock held");
    notification_ticks.push_back(ticks);
    events.emplace_back("notify");
    if (++notify_calls == task_iterations) throw StopTask{};
    clock_us += std::int64_t(ticks) * tick_ms * 1000;
    return 0;
}
'''


TESTS = r'''
bool has_log(const std::string &fragment) {
    return std::any_of(logs.begin(), logs.end(), [&](const auto &line) {
        return line.find(fragment) != std::string::npos;
    });
}
std::string matching_log(const std::string &fragment) {
    for (const auto &line : logs) if (line.find(fragment) != std::string::npos) return line;
    throw std::runtime_error("missing log: " + fragment);
}
void reset() {
    clock_us = 0;
    lvgl_locked = false;
    events.clear(); logs.clear(); notification_ticks.clear();
    delay_calls = reads = writes = audio_calls = sample_calls = take_calls = irq_calls = 0;
    motion_polls = rtc_polls = battery_polls = probe_calls = notify_calls = 0;
    audio_change_on_call = irq_change_on_call = 0;
    initial_audio = {}; changed_audio = {};
    irq_during_sample = false;
    irq_value = 77;
    request_pending = false;
    sample_duration_us = 0;
    sample_template = {};
    activation_calls = request_read_calls = 0;
    activation_result = ordinary_read_result = ESP_OK;
    ordinary_read_completes = true;
    ordinary_evidence = {};
    sample_template.locked = true;
    sample_template.count_result = ESP_ERR_INVALID_STATE;
    sample_template.mode_result = ESP_ERR_INVALID_STATE;
    sample_template.probe_result = ESP_ERR_NOT_FOUND;
    sample_template.reset_level = 1;
    sample_template.irq_level = 1;
    hardware_bus_observation = {42, 0x6b, ESP_ERR_TIMEOUT, -1234};
    transfer_result = ESP_OK;
    power_state = nightglass::core::PowerState::active;
    task_iterations = 2;
    diagnostics_gyro_requested.store(false);
    gyro_enabled.store(false);
    last_gyro_policy_attempt_us = -1'000'000;
}
void assert_no_quiet_mutation() {
    require(reads == 0 && writes == 0, "quiet diagnostic issued a non-touch I2C transaction");
    require(hardware_bus_observation.sequence == 42 &&
            hardware_bus_observation.address == 0x6b &&
            hardware_bus_observation.result == ESP_ERR_TIMEOUT &&
            hardware_bus_observation.completed_us == -1234,
            "quiet diagnostic altered last hardware-bus evidence");
    require(!lvgl_locked, "quiet diagnostic leaked LVGL lock");
}
void assert_window_logs(const char *reason) {
    require(has_log("coverage=hardware_task_only"), "quiet log overstates/omits its task-only scope");
    require(has_log("last_address=0x6b last_result=ESP_ERR_TIMEOUT last_completed_us=-1234"),
            "quiet start lost the last non-touch transaction evidence");
    require(has_log(std::string("TOUCH_BUS_RECOVERY_END reason=") + reason), "wrong quiet-window exit reason");
    require(has_log("hw_sequence_before=42 hw_sequence_after=42"), "quiet end lost traffic sequence evidence");
    require(has_log("resume_hardware_polling=1"), "quiet end does not identify polling resumption");
    assert_no_quiet_mutation();
}
void run_task() {
    try { hardware_task(&bus_storage); }
    catch (const StopTask &) { return; }
    throw std::runtime_error("production hardware task unexpectedly returned");
}
void assert_polling_resumed(bool had_window) {
    require(probe_calls == 1, "hardware task startup probe changed");
    require(notify_calls == task_iterations, "normal hardware task did not resume");
    require(motion_polls == task_iterations && rtc_polls >= 1 && battery_polls >= 1,
            "IMU/RTC/PMIC normal polling failed to resume");
    require(take_calls == task_iterations, "request not checked once per ordinary hardware iteration");
    const auto motion = std::find(events.begin(), events.end(), "motion");
    require(motion != events.end() && motion + 3 < events.end(), "missing resumed polling order");
    require(*(motion + 1) == "rtc" && *(motion + 2) == "battery" && *(motion + 3) == "notify",
            "normal resumed IMU/RTC/PMIC/notification ordering changed");
    if (had_window) {
        const auto ended = std::find(events.begin(), events.end(), "quiet_end");
        require(ended != events.end() && ended < motion, "normal polling preceded quiet-window end");
        require(std::count(events.begin(), events.end(), "quiet_begin") == 1,
                "consumed one-shot request triggered another quiet window");
    }
    assert_no_quiet_mutation();
}

void check_register_helpers() {
    reset();
    const i2c_master_dev_handle_t devices[] = {rtc_device, pmic_device, imu_device, &unknown_storage};
    const unsigned addresses[] = {0x51, 0x34, 0x6b, 0};
    const esp_err_t results[] = {ESP_OK, ESP_FAIL, ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT};
    hardware_bus_observation = {};
    std::uint32_t expected_sequence = 0;
    for (unsigned d = 0; d < 4; ++d) {
        for (const auto result : results) {
            transfer_result = result;
            std::uint8_t data[] = {0x19, 0x19, 0x19};
            require(read_register(devices[d], 0x35, data, sizeof(data)) == result,
                    "read result changed while recording evidence");
            require(observed_device == devices[d] && observed_tx == std::vector<std::uint8_t>{0x35} &&
                    observed_rx_size == sizeof(data) && observed_timeout == 20,
                    "read wrapper changed I2C transaction fields or timeout");
            require(std::all_of(std::begin(data), std::end(data), [&](auto value) {
                return value == (result == ESP_OK ? 0xa6 : 0x19);
            }), "telemetry changed read data or manufactured a successful read");
            require(hardware_bus_observation.sequence == ++expected_sequence &&
                    hardware_bus_observation.address == addresses[d] &&
                    hardware_bus_observation.result == result &&
                    hardware_bus_observation.completed_us == clock_us,
                    "read telemetry not recorded at actual completion");
            require(write_register(devices[d], 0x90, 0x5a) == result,
                    "write result changed while recording evidence");
            require(observed_device == devices[d] && observed_tx == std::vector<std::uint8_t>{0x90, 0x5a} &&
                    observed_timeout == 20, "write wrapper changed I2C payload or timeout");
            require(hardware_bus_observation.sequence == ++expected_sequence &&
                    hardware_bus_observation.address == addresses[d] &&
                    hardware_bus_observation.result == result &&
                    hardware_bus_observation.completed_us == clock_us,
                    "write telemetry not recorded at actual completion");
        }
    }
    require(reads == 16 && writes == 16, "register wrapper retried or added traffic");
    hardware_bus_observation.sequence = UINT32_MAX;
    std::uint8_t value;
    read_register(rtc_device, 0x04, &value, 1);
    require(hardware_bus_observation.sequence == 0, "transaction counter must wrap as uint32_t");
    require(logs.empty(), "register telemetry unexpectedly logs each transfer");
}

void check_no_request() {
    reset();
    run_touch_bus_recovery();
    require(take_calls == 1 && audio_calls == 0 && irq_calls == 0 && sample_calls == 0 &&
            delay_calls == 0 && clock_us == 0 && logs.empty(),
            "no request changed normal task timing/traffic/diagnostics");
    assert_no_quiet_mutation();
    for (const auto state : {nightglass::core::PowerState::active,
                            nightglass::core::PowerState::screen_blank,
                            nightglass::core::PowerState::light_sleep}) {
        reset();
        power_state = state;
        task_iterations = 3;
        run_task();
        assert_polling_resumed(false);
        require(audio_calls == 0 && irq_calls == 0 && sample_calls == 0 && logs.empty(),
                "ordinary hardware task unexpectedly invoked diagnostic work");
        const TickType_t expected_ticks = state == nightglass::core::PowerState::active ? 4 : 10;
        require(notification_ticks == std::vector<TickType_t>(3, expected_ticks),
                "ordinary 40-ms active / 100-ms blank polling changed");
        require(rtc_polls == 1 && battery_polls == 1, "ordinary RTC/PMIC cadence changed");
    }
}

void check_audio_skip() {
    for (const bool active : {false, true}) {
        reset();
        request_pending = true;
        initial_audio = {active, !active};
        run_task();
        require(sample_calls == 0 && delay_calls == 0 && irq_calls == 0,
                "initial audio activity failed to skip quiet window");
        require(has_log("TOUCH_BUS_RECOVERY_SKIPPED reason=audio_activity") &&
                !has_log("TOUCH_BUS_RECOVERY_BEGIN"), "skipped diagnostic reported a quiet interval");
        assert_polling_resumed(false);
    }
}

void check_deadlines() {
    // A bounded sample may cross the deadline. Do not claim an interrupt-exact
    // 4 seconds: allow one injected sample duration plus tick rounding.
    for (const auto duration : {0LL, 1000LL, 99'900LL, 173'500LL, 600'123LL}) {
        reset();
        request_pending = true;
        sample_duration_us = duration;
        run_touch_bus_recovery();
        require(clock_us >= 4'000'000 && clock_us < 4'000'000 + duration + 10'000,
                "quiet window outside deadline plus one bounded sample/tick");
        require(sample_calls > 0 && sample_calls <= 41, "unexpected diagnostic cadence or busy loop");
        require(activation_calls == 0 && request_read_calls == 0,
                "failed mode reads triggered activation or normal read");
        assert_window_logs("deadline");
    }
    reset();
    request_pending = true;
    run_task();
    assert_window_logs("deadline");
    assert_polling_resumed(true);
}

void check_abort_paths() {
    for (const bool active : {false, true}) {
        for (const int change_at : {2, 3}) {
            reset();
            request_pending = true;
            audio_change_on_call = change_at;
            changed_audio = {active, !active};
            run_task();
            require(sample_calls == change_at - 2, "audio transition allowed another diagnostic sample");
            assert_window_logs("audio_activity");
            assert_polling_resumed(true);
        }
    }
    for (const int change_at : {2, 3}) {
        reset();
        request_pending = true;
        irq_change_on_call = change_at;
        run_task();
        require(sample_calls == change_at - 2, "pre-sample IRQ transition failed to abort");
        assert_window_logs("irq_activity");
        assert_polling_resumed(true);
    }
    reset();
    request_pending = true;
    irq_during_sample = true;
    run_task();
    require(sample_calls == 1 && delay_calls == 0, "in-sample IRQ transition did not abort before waiting");
    assert_window_logs("irq_or_reported_contact");
    assert_polling_resumed(true);

    reset();
    request_pending = true;
    sample_template.count_result = ESP_OK;
    sample_template.point_count = 1;
    run_task();
    require(sample_calls == 1 && delay_calls == 0, "successful raw contact did not abort before waiting");
    assert_window_logs("irq_or_reported_contact");
    assert_polling_resumed(true);
}

void check_sample_evidence() {
    for (const auto mode : {0, 1}) {
        reset();
        request_pending = true;
        sample_template.count_result = sample_template.mode_result = ESP_OK;
        sample_template.point_count = 0;
        sample_template.power_mode = mode;
        run_task();
        require(activation_calls == 1 && request_read_calls == 1 && sample_calls == 2,
                "readable ACTIVE/MONITOR did not verify mode then post one ordinary read");
        require(clock_us >= 20'000 && has_log("source=lvgl_normal_read"),
                "missing settling or actual normal-read evidence");
        assert_window_logs("normal_read_ok");
        assert_polling_resumed(true);
    }
    for (const auto mode : {2, 3, 255}) {
        reset();
        request_pending = true;
        sample_template.mode_result = ESP_OK;
        sample_template.power_mode = mode;
        run_touch_bus_recovery();
        require(activation_calls == 0 && request_read_calls == 0,
                "unknown/sleep mode activated or manufactured normal-read evidence");
        assert_window_logs("deadline");
    }
    for (const bool completes : {false, true}) {
        reset();
        request_pending = true;
        sample_template.mode_result = ESP_OK;
        sample_template.power_mode = 1;
        ordinary_read_completes = completes;
        ordinary_read_result = ESP_FAIL;
        run_task();
        require(activation_calls == 1 && request_read_calls == 1,
                "normal-read failure changed recovery request count");
        assert_window_logs(completes ? "normal_read_failed" : "normal_read_timeout");
        assert_polling_resumed(true);
    }
    reset();
    request_pending = true;
    sample_template.mode_result = ESP_OK;
    sample_template.power_mode = 1;
    activation_result = ESP_FAIL;
    run_task();
    require(activation_calls == 1 && request_read_calls == 0,
            "failed activation proceeded to ordinary read");
    assert_window_logs("activation_failed");
    assert_polling_resumed(true);
}

int main() {
    try {
        check_register_helpers();
        check_no_request();
        check_audio_skip();
        check_deadlines();
        check_abort_paths();
        check_sample_evidence();
        std::cout << "Hardware touch recovery passed: actual register helpers, "
                     "recovery loop, and hardware_task; " << checks << " assertions; "
                     "coverage=hardware_task_only (no physical recovery claim)\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=ROOT / "components/nightglass_services/src/hardware.cpp")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    source = args.source.read_text()
    task = definition(source, "void hardware_task(")
    if args.baseline is not None:
        invocation = "        run_touch_bus_recovery();\n"
        assert task.count(invocation) == 1, "expected one diagnostic invocation in actual hardware task"
        baseline_task = definition(args.baseline.read_text(), "void hardware_task(")
        for prior_invocation in (invocation, "        run_touch_bus_quiet_diagnostic();\n"):
            baseline_task = baseline_task.replace(prior_invocation, "")
        assert task.replace(invocation, "") == baseline_task, \
            "ordinary hardware task changed beyond recovery invocation"
    constants = "\n".join(declaration(source, name) for name in (
        "kTag", "kRtcAddress", "kPmicAddress", "kImuAddress", "kI2cTimeoutMs",
        "kActivePollTicks", "kBlankPollTicks", "kHapticGpio", "kHapticSupplyMv"))
    production = "\n".join(definition(source, signature) for signature in (
        "void record_hardware_bus_transaction(", "esp_err_t read_register(",
        "esp_err_t write_register(", "void run_touch_bus_recovery("))
    observation = definition(source, "struct HardwareBusObservation") + ";\n"
    haptic = definition(source, "struct HapticCommand") + ";\n"
    program = "\n".join((PRELUDE, constants, observation,
                         "HardwareBusObservation hardware_bus_observation{};",
                         production, haptic, TASK_BOUNDARIES, task, TESTS))
    with tempfile.TemporaryDirectory(prefix="nightglass-hardware-quiet-") as directory:
        temporary = Path(directory)
        (temporary / "esp_err.h").write_text(
            "#pragma once\nusing esp_err_t = int;\n"
            "constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=0x103, "
            "ESP_ERR_NOT_FOUND=0x105, ESP_ERR_TIMEOUT=0x107;\n")
        cpp = temporary / "hardware_quiet.cpp"
        cpp.write_text(program)
        binary = temporary / "hardware_quiet"
        compiler = os.environ.get("CXX", "c++")
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-I", str(temporary), "-I", str(ROOT / "components/nightglass_bsp/include"),
                        str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
    print("Reviewed hardware.cpp SHA256=" + hashlib.sha256(args.source.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()

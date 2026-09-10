#!/usr/bin/env python3
"""Compile production touch diagnostics/callbacks with real health bookkeeping.

Only I2C, GPIO, LVGL, clock, logging, and critical-section boundaries are
doubled. Host mutexes exercise the production cross-thread/ISR locking paths;
they do not establish physical IRQ behavior or an ESP timing guarantee.
An optional --baseline board.cpp verifies the normal sample/gesture/health
bodies remain identical apart from diagnostic metadata publication.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--baseline", type=Path)
parser.add_argument("--recovery-only", action="store_true")
args = parser.parse_args()
source = (ROOT / "components/nightglass_bsp/src/board.cpp").read_text()


def function(text: str, signature: str) -> str:
    match = re.search(re.escape(signature) + r"[^;{}]*\{", text)
    if match is None:
        raise AssertionError(f"Missing production function: {signature}")
    opening = match.end() - 1
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


unchanged = (
    "void touch_probe_callback(", "void record_touch_read_success(",
    "void handle_touch_error(", "void touchpad_read(",
)
if args.baseline:
    baseline = args.baseline.read_text()
    for signature in unchanged:
        assert function(source, signature) == function(baseline, signature), signature
    current_read = function(source, "esp_err_t read_touch_controller(").replace(
        "            request_touch_bus_quiet_diagnostic(probe_result);\n", "")
    current_read = re.sub(
        r"    // Publish only after the genuine sample.*?    portEXIT_CRITICAL\(&touch_diagnostic_mux\);\n",
        "", current_read, flags=re.S)
    baseline_read = function(baseline, "esp_err_t read_touch_controller(").replace(
        "            request_touch_bus_quiet_diagnostic(probe_result);\n", "")
    assert current_read == baseline_read
    print("Normal sample, gesture, probe, and health bodies match baseline")

constants = "\n".join(re.search(r"^constexpr [^\n]*\b" + name + r" = [^\n]*;", source, re.M)[0]
                      for name in ("kTouchProbePeriodUs", "kTouchLivenessLogPeriodUs",
                                   "kTouchTransactionTimeoutMs"))
state_and_request = source[source.index("std::int64_t last_touch_error_log_us"):
                           source.index("esp_err_t create_touch_controller()")]
production = "\n".join(function(source, signature) for signature in (
    "void IRAM_ATTR touch_interrupt(", "void touch_probe_callback(",
    "esp_err_t read_touch_controller(", "void record_touch_read_success(",
    "void handle_touch_error(", "void touchpad_read(",
    "bool take_touch_bus_quiet_request(", "std::uint32_t touch_irq_sequence(",
    "TouchBusQuietSample sample_touch_bus_quiet_diagnostic(",
    "TouchReadEvidence touch_read_evidence(", "void request_touch_read(",
    "esp_err_t activate_touch_from_monitor(",
    "bool Board::lock_display(", "void Board::unlock_display(",
))

FREERTOS = r'''
#pragma once
#include <mutex>
struct portMUX_TYPE { std::mutex value; };
#define portMUX_INITIALIZER_UNLOCKED {}
void test_enter_critical(portMUX_TYPE *, bool);
void test_exit_critical(portMUX_TYPE *, bool);
#define portENTER_CRITICAL(mux) test_enter_critical(mux, false)
#define portEXIT_CRITICAL(mux) test_exit_critical(mux, false)
#define portENTER_CRITICAL_ISR(mux) test_enter_critical(mux, true)
#define portEXIT_CRITICAL_ISR(mux) test_exit_critical(mux, true)
'''

PRELUDE = r'''
#include <atomic>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "nightglass/bsp/board.hpp"
#include "nightglass/bsp/ft3168.hpp"
#include "nightglass/bsp/touch_bus_diagnostic.hpp"
#include "nightglass/core/health.hpp"

void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}
thread_local bool isr_context = false;
thread_local int critical_depth = 0;
std::atomic<unsigned> task_entries{0}, isr_entries{0};
void test_enter_critical(portMUX_TYPE *mux, bool isr) {
    require(isr == isr_context, "wrong ISR/task critical-section primitive");
    require(critical_depth == 0, "nested critical section");
    mux->value.lock();
    ++critical_depth;
    if (isr) ++isr_entries; else ++task_entries;
}
void test_exit_critical(portMUX_TYPE *mux, bool isr) {
    require(isr == isr_context && critical_depth == 1, "unbalanced critical section");
    --critical_depth;
    mux->value.unlock();
}
std::int64_t fake_time = 1'000'000;
std::int64_t esp_timer_get_time() { return fake_time; }
#define IRAM_ATTR
constexpr int BSP_LCD_TOUCH_RST = 9, BSP_LCD_TOUCH_INT = 38;
constexpr int ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS = 0x38;
constexpr int LVGL_PORT_EVENT_TOUCH = 1, MALLOC_CAP_INTERNAL = 1;
enum lv_indev_state_t { LV_INDEV_STATE_RELEASED, LV_INDEV_STATE_PRESSED };
struct lv_point_t { std::int32_t x = 0, y = 0; };
struct lv_indev_t { int identity; } input_storage{1};
struct lv_indev_data_t { lv_indev_state_t state{}; lv_point_t point{}; };
struct esp_lcd_touch_point_data_t { std::uint16_t x = 0, y = 0; };
struct Touch { void *io; } touch_storage{&touch_storage};
using esp_lcd_touch_handle_t = Touch *;
int bus_storage;
i2c_master_bus_handle_t bus_handle = &bus_storage;
std::vector<std::string> operations;
struct Read { int reg; esp_err_t result; std::uint8_t value; };
std::deque<Read> raw_reads;
esp_err_t address_result = ESP_ERR_NOT_FOUND;
esp_err_t driver_read_result = ESP_OK, driver_get_result = ESP_OK;
esp_lcd_touch_point_data_t driver_point{100, 200};
std::uint8_t driver_count = 0;
int driver_reads = 0, driver_gets = 0, reset_calls = 0;
int raw_calls = 0, probe_calls = 0, lock_calls = 0, unlock_calls = 0;
int write_calls = 0;
esp_err_t write_result = ESP_OK;
std::int64_t raw_advance_us = 0;
int lock_timeout = -1, reset_level = 1, irq_level = 1;
bool lock_allowed = true, lock_held = false;
std::atomic<unsigned> wake_calls{0};

const char *esp_err_to_name(esp_err_t result) {
    return result == ESP_OK ? "ESP_OK" : "ERROR";
}
template<typename... Args> void log_line(const char *, const char *, Args...) {
    require(critical_depth == 0, "log inside critical section");
}
#define ESP_LOGI(...) log_line(__VA_ARGS__)
#define ESP_LOGW(...) log_line(__VA_ARGS__)
std::size_t heap_caps_get_free_size(int) { return 20000; }
std::size_t heap_caps_get_largest_free_block(int) { return 10000; }
int gpio_get_level(int pin) {
    require(critical_depth == 0, "GPIO read inside metadata critical section");
    require(pin == 9 || pin == 38, "diagnostic observed an unrelated GPIO");
    operations.push_back("gpio:" + std::to_string(pin));
    return pin == 9 ? reset_level : irq_level;
}
i2c_master_bus_handle_t bsp_i2c_get_handle() { return bus_handle; }
esp_err_t esp_lcd_panel_io_rx_param(void *io, int reg, void *data, std::size_t length) {
    require(critical_depth == 0 && lock_held, "raw I2C lacked LVGL ownership or held spinlock");
    require(io == &touch_storage && data != nullptr && length == 1, "wrong raw-read boundary");
    require(!raw_reads.empty(), "unexpected extra raw read/retry");
    const auto next = raw_reads.front(); raw_reads.pop_front();
    require(reg == next.reg, "wrong register/order");
    ++raw_calls; operations.push_back("read:" + std::to_string(reg));
    fake_time += raw_advance_us;
    *static_cast<std::uint8_t *>(data) = next.value;
    return next.result;
}
esp_err_t esp_lcd_panel_io_tx_param(void *io, int reg, const void *data, std::size_t length) {
    require(critical_depth == 0 && lock_held, "mode write lacked LVGL ownership or held spinlock");
    require(io == &touch_storage && data != nullptr && length == 1 && reg == 0xa5 &&
            *static_cast<const std::uint8_t *>(data) == 0, "undocumented/wrong mode write");
    ++write_calls; operations.push_back("write:165:0");
    return write_result;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, int address, int timeout) {
    require(critical_depth == 0 && lock_held, "address probe lacked LVGL ownership or held spinlock");
    require(bus == &bus_storage && address == 0x38 && timeout == 50, "wrong address/probe bound");
    ++probe_calls; operations.push_back("probe");
    return address_result;
}
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t handle) {
    require(handle == &touch_storage && lock_held, "wrong genuine-read boundary");
    ++driver_reads;
    return driver_read_result;
}
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t handle,
                               esp_lcd_touch_point_data_t *point, std::uint8_t *count,
                               int maximum) {
    require(handle == &touch_storage && maximum == 1 && lock_held, "wrong genuine-extract boundary");
    ++driver_gets;
    if (driver_get_result == ESP_OK) { *point = driver_point; *count = driver_count; }
    return driver_get_result;
}
void lv_indev_reset(lv_indev_t *input, void *object) {
    require(input == &input_storage && object == nullptr, "gesture cancellation changed");
    ++reset_calls;
}
void lvgl_port_task_wake(int event, lv_indev_t *input) {
    require(critical_depth == 0, "LVGL wake inside ISR metadata critical section");
    require(event == LVGL_PORT_EVENT_TOUCH && input == &input_storage, "wrong LVGL wake");
    ++wake_calls;
}
bool bsp_display_lock(int timeout) {
    require(critical_depth == 0 && !lock_held, "nested/critical-section display lock");
    ++lock_calls; lock_timeout = timeout;
    operations.push_back("lock");
    if (lock_allowed) lock_held = true;
    return lock_allowed;
}
void bsp_display_unlock() {
    require(lock_held && critical_depth == 0, "unowned/critical-section display unlock");
    ++unlock_calls; lock_held = false;
    operations.push_back("unlock");
}

namespace nightglass::bsp {
Board instance;
constexpr char kTag[] = "nightglass_bsp";
lv_indev_t *touch_input = &input_storage;
esp_lcd_touch_handle_t touch_handle = &touch_storage;
'''

CHECKS = r'''
}  // namespace nightglass::bsp
using namespace nightglass::bsp;
using nightglass::core::HealthRecord;
using nightglass::core::HealthState;
using nightglass::core::health_registry;
static_assert(kTouchProbePeriodUs == 10'000'000, "genuine probe frequency changed");
static_assert(kTouchTransactionTimeoutMs == 50, "per-transfer timeout changed");

std::string health_state() {
    std::string result;
    for (const auto &record : health_registry().snapshot()) {
        if (!record.occupied) continue;
        result += std::string(record.component.data()) + ":" + record.detail.data() + ":" +
            std::to_string(static_cast<int>(record.state)) + ":" +
            std::to_string(record.failure_count) + ":" + std::to_string(record.last_success_us) + "\n";
    }
    return result;
}
HealthRecord touch_health() {
    HealthRecord record;
    require(health_registry().copy("touch", record), "touch record missing");
    return record;
}
void reset_boundaries() {
    require(!lock_held && critical_depth == 0, "previous case leaked lock");
    touch_handle = &touch_storage; touch_storage.io = &touch_storage;
    touch_input = &input_storage; bus_handle = &bus_storage;
    raw_reads.clear(); operations.clear();
    driver_read_result = driver_get_result = ESP_OK;
    driver_count = 0; address_result = ESP_ERR_NOT_FOUND;
    driver_reads = driver_gets = reset_calls = raw_calls = probe_calls = 0;
    lock_calls = unlock_calls = write_calls = 0; lock_timeout = -1;
    write_result = ESP_OK; raw_advance_us = 0;
    lock_allowed = true; reset_level = irq_level = 1;
    fake_time += 3'000'000;
}
void new_boot_metadata() {
    // Test isolation only; production never resets these monotonic metadata.
    portENTER_CRITICAL(&touch_diagnostic_mux);
    touch_bus_quiet_requested = touch_bus_quiet_request_issued = false;
    last_touch_bus_quiet_request_us = 0;
    last_touch_read_evidence = {};
    touch_diagnostic_irq_sequence = 0;
    portEXIT_CRITICAL(&touch_diagnostic_mux);
}
lv_indev_data_t normal_sample(esp_err_t read, esp_err_t extract, esp_err_t mode,
                            esp_err_t address = ESP_ERR_NOT_FOUND) {
    driver_read_result = read; driver_get_result = extract; address_result = address;
    fake_time += 3'000'000;  // Let the existing log throttle choose its normal diagnostic.
    raw_reads.push_back({0xa5, mode, 0});
    lv_indev_data_t data;
    lock_held = true; touchpad_read(&input_storage, &data); lock_held = false;
    require(raw_reads.empty(), "normal diagnostic unexpectedly skipped or changed");
    return data;
}
void physical_irq() {
    isr_context = true;
    touch_interrupt(&touch_storage);
    isr_context = false;
}
void expect_inert(const std::string &health_before, unsigned irq_before) {
    require(health_state() == health_before, "diagnostic manufactured health evidence");
    require(driver_reads == 0 && driver_gets == 0 && reset_calls == 0,
            "diagnostic modified normal driver/gesture state");
    require(touch_irq_sequence() == irq_before, "diagnostic manufactured IRQ");
    require(!lock_held && critical_depth == 0, "diagnostic leaked lock");
}
void quiet_read_checks() {
    new_boot_metadata();
    health_registry().set("touch", HealthState::ok, "prior genuine sample");
    const auto health_before = health_state();
    for (int missing = 0; missing < 3; ++missing) {
        reset_boundaries();
        if (missing == 0) lock_allowed = false;
        if (missing == 1) touch_handle = nullptr;
        if (missing == 2) touch_storage.io = nullptr;
        const auto sample = sample_touch_bus_quiet_diagnostic();
        require(sample.locked == (missing != 0), "lock result was misrepresented");
        require(sample.count_result == ESP_ERR_INVALID_STATE && sample.mode_result == ESP_ERR_INVALID_STATE &&
                sample.probe_result == ESP_ERR_INVALID_STATE, "unattempted IO reported success");
        require(sample.point_count == 0xff && sample.power_mode == 0xff && raw_calls == 0 && probe_calls == 0,
                "invalid boundary issued IO or manufactured raw data");
        require(lock_calls == 1 && lock_timeout == 20 && unlock_calls == (missing != 0), "bad lock bound/cleanup");
        require(sample.reset_level == -1 && sample.irq_level == 1 && sample.sampled_at_us == fake_time,
                "failure omitted GPIO/time observations");
        expect_inert(health_before, 0);
    }
    for (unsigned failures = 0; failures < 4; ++failures) {
        reset_boundaries();
        const bool count_fails = (failures & 1) != 0, mode_fails = (failures & 2) != 0;
        raw_reads = {{0x02, count_fails ? ESP_ERR_TIMEOUT : ESP_OK, 3},
                     {0xa5, mode_fails ? ESP_ERR_INVALID_STATE : ESP_OK, 1}};
        address_result = failures == 1 ? ESP_OK : failures == 2 ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_FOUND;
        const auto sample = sample_touch_bus_quiet_diagnostic();
        require(sample.locked && lock_calls == 1 && lock_timeout == 20 && unlock_calls == 1, "raw-read lock lost");
        require(sample.count_result == (count_fails ? ESP_ERR_TIMEOUT : ESP_OK) &&
                sample.mode_result == (mode_fails ? ESP_ERR_INVALID_STATE : ESP_OK), "raw IO result changed");
        require(sample.point_count == (count_fails ? 0xff : 3) && sample.power_mode == (mode_fails ? 0xff : 1),
                "failed read leaked modified output or successful raw value changed");
        require(sample.probe_result == (failures ? address_result : ESP_ERR_INVALID_STATE), "skipped probe manufactured ACK");
        std::vector<std::string> expected{"lock", "read:2", "read:165"};
        if (failures) expected.push_back("probe");
        expected.insert(expected.end(), {"gpio:38", "unlock"});
        require(operations == expected && raw_reads.empty() && raw_calls == 2 && probe_calls == (failures != 0),
                "raw diagnostic did extra/undocumented IO or changed cleanup order");
        require(!take_touch_bus_quiet_request(), "raw diagnostic armed a new quiet window");
        expect_inert(health_before, 0);
    }
    reset_boundaries(); bus_handle = nullptr;
    raw_reads = {{0x02, ESP_ERR_TIMEOUT, 9}, {0xa5, ESP_ERR_TIMEOUT, 9}};
    auto sample = sample_touch_bus_quiet_diagnostic();
    require(sample.locked && sample.probe_result == ESP_ERR_INVALID_STATE && probe_calls == 0 && unlock_calls == 1,
            "missing bus was probed/reported as an ACK or leaked display lock");
    expect_inert(health_before, 0);
}
void cooldown_and_health_checks() {
    reset_boundaries(); new_boot_metadata();
    touch_irq_registered = true; touch_degraded = false;
    consecutive_touch_errors = 0; last_touch_state = LV_INDEV_STATE_PRESSED; last_touch_point = {44, 55};
    health_registry().set("touch", HealthState::ok, "prior genuine sample");
    const auto success_time = touch_health().last_success_us;
    require(!take_touch_bus_quiet_request(), "boot began with a request");
    const esp_err_t non_nack[] = {ESP_OK, ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT};
    for (unsigned i = 0; i < 3; ++i) {
        const auto data = normal_sample(ESP_ERR_INVALID_STATE, ESP_OK, ESP_ERR_INVALID_STATE, non_nack[i]);
        require(!take_touch_bus_quiet_request(), "unconfirmed NACK requested quiet window");
        require(touch_health().last_success_us == success_time && driver_gets == 0,
                "failed sample gained health credit or extracted coordinates");
        if (i < 2) {
            require(data.state == LV_INDEV_STATE_PRESSED && data.point.x == 44 && data.point.y == 55 && reset_calls == 0,
                    "bounded missed sample changed held gesture");
        } else {
            require(data.state == LV_INDEV_STATE_RELEASED && reset_calls == 1 &&
                    touch_health().state == HealthState::degraded, "third failure failed safe gesture cancellation");
        }
    }
    normal_sample(ESP_ERR_TIMEOUT, ESP_OK, ESP_OK);
    require(!take_touch_bus_quiet_request() && probe_calls == 3, "successful mode read caused address probe/request");
    normal_sample(ESP_ERR_TIMEOUT, ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_NOT_FOUND);
    require(take_touch_bus_quiet_request(), "confirmed normal diagnostic NACK did not request window");
    require(!take_touch_bus_quiet_request(), "request was consumed twice");
    normal_sample(ESP_ERR_TIMEOUT, ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_NOT_FOUND);
    require(!take_touch_bus_quiet_request(), "consumption bypassed the request cooldown");
    require(touch_health().last_success_us == success_time, "request/NACK gained health credit");
    const auto before_extract_failure = touch_health().last_success_us;
    normal_sample(ESP_OK, ESP_ERR_TIMEOUT, ESP_OK);
    require(touch_health().last_success_us == before_extract_failure, "failed extraction gained health credit");
    driver_count = 1;
    const auto contact = normal_sample(ESP_OK, ESP_OK, ESP_OK);
    require(contact.state == LV_INDEV_STATE_PRESSED && contact.point.x == 100 && contact.point.y == 200 &&
            touch_health().state == HealthState::ok && touch_health().last_success_us == fake_time &&
            consecutive_touch_errors == 0 && !touch_degraded,
            "genuine registered controller sample lost health/coordinate behavior");
    const auto registered_success_time = touch_health().last_success_us;
    touch_irq_registered = false;
    normal_sample(ESP_OK, ESP_OK, ESP_OK);
    require(touch_health().state == HealthState::degraded && touch_health().last_success_us == registered_success_time,
            "unregistered IRQ gained full touch health");
    new_boot_metadata(); touch_irq_registered = true;
    normal_sample(ESP_OK, ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_NOT_FOUND);
    require(take_touch_bus_quiet_request() && touch_health().last_success_us == fake_time,
            "independent diagnostic failure replaced genuine successful-sample evidence");
    require(touch_irq_sequence() == 0, "normal read/contact manufactured physical IRQ");
}
void irq_and_concurrency_checks() {
    reset_boundaries(); new_boot_metadata();
    const auto health_before = health_state();
    const auto wakes = wake_calls.load();
    touch_probe_callback(nullptr);
    require(touch_irq_sequence() == 0 && wake_calls == wakes + 1, "timer probe manufactured IRQ or lost wake");
    const auto prior_isr_entries = isr_entries.load();
    physical_irq();
    require(touch_irq_sequence() == 1 && wake_calls == wakes + 2 && isr_entries == prior_isr_entries + 1,
            "real GPIO callback lost count, ISR protection, or existing wake");
    touch_input = nullptr; physical_irq();
    require(touch_irq_sequence() == 2 && wake_calls == wakes + 2, "IRQ without input was lost or fabricated wake");
    require(!take_touch_bus_quiet_request() && health_state() == health_before, "IRQ manufactured request/health");
    portENTER_CRITICAL(&touch_diagnostic_mux);
    touch_diagnostic_irq_sequence = std::numeric_limits<std::uint32_t>::max();
    portEXIT_CRITICAL(&touch_diagnostic_mux);
    physical_irq();
    require(touch_irq_sequence() == 0, "IRQ count failed unsigned wrap");
    new_boot_metadata();
    constexpr unsigned iterations = 10'000;
    std::atomic<unsigned> consumed{0};
    auto irq_writer = [] {
        for (unsigned i = 0; i < iterations; ++i) physical_irq();
    };
    auto requester = [] {
        for (unsigned i = 0; i < iterations; ++i) request_touch_bus_quiet_diagnostic(ESP_ERR_NOT_FOUND);
    };
    std::thread irq_a(irq_writer), irq_b(irq_writer), request_a(requester), request_b(requester);
    std::thread reader([&] {
        for (unsigned i = 0; i < iterations; ++i) {
            if (take_touch_bus_quiet_request()) ++consumed;
            require(touch_irq_sequence() <= 2 * iterations, "corrupt concurrent IRQ snapshot");
        }
    });
    irq_a.join(); irq_b.join(); request_a.join(); request_b.join(); reader.join();
    if (take_touch_bus_quiet_request()) ++consumed;
    require(consumed == 1 && touch_irq_sequence() == 2 * iterations,
            "cross-thread request duplicated or ISR counter lost updates");
    require(health_state() == health_before && !lock_held, "metadata concurrency changed health/display ownership");
}

void recovery_helper_checks() {
    reset_boundaries(); new_boot_metadata();
    const auto health_before = health_state();
    require(touch_read_evidence().sequence == 0 &&
            touch_read_evidence().result == ESP_ERR_INVALID_STATE, "manufactured initial read evidence");
    for (int scenario = 0; scenario < 11; ++scenario) {
        reset_boundaries();
        esp_err_t expected = ESP_ERR_INVALID_STATE;
        int expected_reads = 0, expected_writes = 0;
        if (scenario == 0) { lock_allowed = false; expected = ESP_ERR_TIMEOUT; }
        else if (scenario == 1) touch_handle = nullptr;
        else if (scenario == 2) touch_storage.io = nullptr;
        else {
            ++expected_reads;
            const std::uint8_t modes[] = {0, 1, 2, 3, 0xff, 1, 1, 0};
            const auto mode = modes[scenario - 3];
            const auto read_result = scenario == 8 ? ESP_ERR_TIMEOUT : ESP_OK;
            raw_reads.push_back({0xa5, read_result, mode});
            if (read_result != ESP_OK) expected = read_result;
            else if (mode == 0) expected = ESP_OK;
            else if (mode == 1) {
                expected_writes = 1;
                write_result = scenario == 9 ? ESP_ERR_TIMEOUT : ESP_OK;
                expected = write_result;
            }
        }
        const auto before_time = fake_time;
        require(activate_touch_from_monitor() == expected, "conditional activation result changed");
        require(raw_reads.empty() && raw_calls == expected_reads && write_calls == expected_writes,
                "activation wrote an unobserved mode or retried IO");
        require(lock_calls == 1 && lock_timeout == 20 && unlock_calls == (scenario != 0),
                "activation leaked lock or changed acquisition bound");
        require(fake_time == before_time && probe_calls == 0 && touch_read_evidence().sequence == 0 &&
                !take_touch_bus_quiet_request(), "activation manufactured evidence or additional recovery");
        expect_inert(health_before, 0);
    }
    reset_boundaries();
    const auto wakes = wake_calls.load();
    request_touch_read();
    require(wake_calls == wakes + 1 && driver_reads == 0 && raw_calls == 0 &&
            lock_calls == 0 && touch_read_evidence().sequence == 0,
            "request executed input/render work instead of posting the existing event");
    touch_input = nullptr; request_touch_read();
    require(wake_calls == wakes + 1, "absent input caused an event");
    expect_inert(health_before, 0);

    // Monotonic cooldown: coalesce, consume once, reject at 20 s minus 1 us,
    // allow exactly 20 s, and never create a request solely from elapsed time.
    reset_boundaries(); new_boot_metadata();
    request_touch_bus_quiet_diagnostic(ESP_ERR_TIMEOUT);
    require(!take_touch_bus_quiet_request(), "non-NACK initiated recovery");
    const auto requested_at = fake_time;
    request_touch_bus_quiet_diagnostic(ESP_ERR_NOT_FOUND);
    request_touch_bus_quiet_diagnostic(ESP_ERR_NOT_FOUND);
    require(take_touch_bus_quiet_request() && !take_touch_bus_quiet_request(), "request did not coalesce");
    fake_time = requested_at + 19'999'999;
    request_touch_bus_quiet_diagnostic(ESP_ERR_NOT_FOUND);
    require(!take_touch_bus_quiet_request(), "cooldown allowed early recovery");
    fake_time = requested_at + 20'000'000;
    require(!take_touch_bus_quiet_request(), "elapsed time manufactured a request");
    request_touch_bus_quiet_diagnostic(ESP_ERR_NOT_FOUND);
    require(take_touch_bus_quiet_request() && !take_touch_bus_quiet_request(), "cooldown did not reopen at 20 s");
    expect_inert(health_before, 0);

    // Publish the genuine result after diagnostics finish; raw recovery and
    // event posting above must not advance this completion sequence.
    for (const auto read_result : {ESP_OK, ESP_ERR_TIMEOUT}) {
        reset_boundaries();
        driver_read_result = read_result;
        raw_reads = {{0xa5, ESP_OK, 0}};
        raw_advance_us = 321;
        const auto before = touch_read_evidence();
        const auto started = fake_time;
        esp_lcd_touch_point_data_t point;
        std::uint8_t count = 0;
        lock_held = true;
        require(read_touch_controller(point, count) == read_result, "metadata replaced the genuine read result");
        lock_held = false;
        const auto evidence = touch_read_evidence();
        require(evidence.sequence == before.sequence + 1 && evidence.result == read_result &&
                evidence.completed_us == fake_time && evidence.completed_us == started + 321 && raw_reads.empty(),
                "completion evidence was early, stale, or manufactured");
        require(health_state() == health_before && critical_depth == 0,
                "read metadata changed health or leaked critical section");
    }
}

int main() {
    try {
        quiet_read_checks();
        cooldown_and_health_checks();
        recovery_helper_checks();
        irq_and_concurrency_checks();
        std::cout << "Nightglass touch-bus diagnostic production regression passed: "
                     "bounded recurring NACK, conditional MONITOR activation, raw reads, lock cleanup, health/gesture preservation, "
                     "IRQ/wake semantics, 20,000 concurrent IRQ callbacks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
'''

if args.recovery_only:
    CHECKS = CHECKS.replace(
        "        quiet_read_checks();\n        cooldown_and_health_checks();\n        recovery_helper_checks();\n        irq_and_concurrency_checks();",
        "        recovery_helper_checks();")
    CHECKS = CHECKS.replace(
        '"bounded recurring NACK, conditional MONITOR activation, raw reads, lock cleanup, health/gesture preservation, "\n                     "IRQ/wake semantics, 20,000 concurrent IRQ callbacks\\n"',
        '"focused conditional activation, 20-second cooldown, event-only request, genuine completion evidence\\n"')

with tempfile.TemporaryDirectory(prefix="nightglass-touch-bus-") as temporary:
    directory = Path(temporary)
    (directory / "driver").mkdir()
    (directory / "driver/i2c_master.h").write_text(
        "#pragma once\nusing i2c_master_bus_handle_t = void *;\n")
    (directory / "freertos").mkdir()
    (directory / "freertos/FreeRTOS.h").write_text(FREERTOS)
    (directory / "esp_timer.h").write_text(
        "#pragma once\n#include <cstdint>\nstd::int64_t esp_timer_get_time();\n")
    (directory / "esp_err.h").write_text(
        "#pragma once\nusing esp_err_t = int;\n"
        "constexpr esp_err_t ESP_OK = 0, ESP_ERR_INVALID_STATE = 0x103, "
        "ESP_ERR_NOT_FOUND = 0x105, ESP_ERR_TIMEOUT = 0x107;\n")
    unit = directory / "touch_bus_diagnostic_test.cpp"
    unit.write_text(PRELUDE + "\n" + constants + "\n" + state_and_request + "\n" + production + "\n" + CHECKS)
    binary = directory / "touch_bus_diagnostic_test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(directory), "-I", str(ROOT / "components/nightglass_bsp/include"),
                    "-I", str(ROOT / "components/nightglass_core/include"), str(unit),
                    str(ROOT / "components/nightglass_core/src/health.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

#!/usr/bin/env python3
"""Run production health checks/task with the real registry and boot policy.

Only clock, FreeRTOS, logging, persistence, and OTA device boundaries are doubled.
Use --baseline path/to/service.cpp to additionally compare all acceptance results
and non-log task traces with a prior source, and prove its diagnostics fail the
new observability assertions. This does not simulate physical I2C or OTA writes.
"""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


FREERTOS = r'''
#pragma once
#include <cstdint>
using portMUX_TYPE = int;
using TickType_t = std::uint32_t;
using TaskHandle_t = void *;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_enter_critical(portMUX_TYPE *);
void test_exit_critical(portMUX_TYPE *);
#define portENTER_CRITICAL(mux) test_enter_critical(mux)
#define portEXIT_CRITICAL(mux) test_exit_critical(mux)
#define pdMS_TO_TICKS(ms) ((ms) / 10U)
'''

PRELUDE = r'''
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "nightglass/core/health.hpp"
#include "nightglass/update/policy.hpp"
#include "nightglass/update/service.hpp"

using nightglass::core::HealthRecord;
using nightglass::core::HealthRegistry;
using nightglass::core::HealthState;
using nightglass::core::health_registry;
using namespace nightglass::update;
constexpr std::int64_t kNow = 100'000'000;
std::int64_t fake_time = kNow;
int critical_depth = 0, critical_entries = 0, timer_calls = 0, snapshot_calls = 0;
int store_calls = 0, accept_calls = 0, rollback_calls = 0, delete_calls = 0;
std::uint32_t stored_count = 99;
bool store_ok = true, track = false, observe = true;
constexpr int ESP_OK = 0;
int accept_result = ESP_OK, rollback_result = ESP_OK;
std::vector<std::string> events, logs;
int checker_scenarios = 0, task_scenarios = 0;

void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}
void event(std::string text) { if (track) events.push_back(std::move(text)); }
void test_enter_critical(portMUX_TYPE *) {
    require(critical_depth == 0, "nested critical section");
    ++critical_depth;
    if (track) ++critical_entries;
    event("registry_enter");
}
void test_exit_critical(portMUX_TYPE *) {
    require(critical_depth == 1, "unbalanced critical section");
    --critical_depth;
    event("registry_exit");
}
std::int64_t esp_timer_get_time() {
    if (track) ++timer_calls;
    event("time");
    return fake_time;
}
void record_log(const char *, const char *, ...) __attribute__((format(printf, 2, 3)));
void record_log(const char *, const char *format, ...) {
    require(critical_depth == 0, "logging inside registry critical section");
    std::array<char, 512> text{};
    va_list args;
    va_start(args, format);
    const int size = std::vsnprintf(text.data(), text.size(), format, args);
    va_end(args);
    require(size >= 0 && size < 256, "diagnostic line was not bounded");
    const std::string line(text.data());
    require(line.find("private-health-detail") == std::string::npos,
            "private health detail escaped into diagnostics");
    logs.push_back(line);
    event("log:" + line);
}
#define ESP_LOGE(...) record_log(__VA_ARGS__)
#define ESP_LOGI(...) record_log(__VA_ARGS__)
#define CONFIG_NIGHTGLASS_OTA_HIL_FORCE_HEALTH_FAILURE 0
void vTaskDelay(TickType_t ticks) { event("delay:" + std::to_string(ticks)); }
void vTaskDelete(void *task) {
    require(task == nullptr, "health task deleted another task");
    ++delete_calls;
    event("delete");
}
int esp_ota_mark_app_valid_cancel_rollback() {
    ++accept_calls; event("accept"); return accept_result;
}
int esp_ota_mark_app_invalid_rollback_and_reboot() {
    ++rollback_calls; event("rollback"); return rollback_result;
}

namespace nightglass::update {
constexpr char kTag[] = "nightglass_update";
UpdateService instance;
UpdateSnapshot current{};
TaskHandle_t health_task_handle = nullptr;
UpdateSnapshot UpdateService::snapshot() const {
    if (track) ++snapshot_calls;
    event("snapshot");
    return current;
}
void publish(const UpdateSnapshot &snapshot) {
    current = snapshot;
    event("publish:" + std::to_string(current.pending_verification) + ":" +
          std::to_string(current.health_gate_armed));
}
bool store_unhealthy_boots(std::uint32_t count) {
    ++store_calls; stored_count = count;
    event("store:" + std::to_string(count));
    return store_ok;
}
'''

SCENARIOS = r'''
}  // namespace nightglass::update

constexpr std::array<const char *, 6> required{"nvs", "display", "touch", "power", "clock", "ui"};
constexpr std::array<HealthState, 5> states{
    HealthState::unknown, HealthState::ok, HealthState::degraded,
    HealthState::absent, HealthState::failed};
// Independent acceptance table: rows nvs/display/touch/power/clock/ui, columns above.
constexpr bool allowed[6][5] = {
    {false, true, false, false, false}, {false, true, true, false, false},
    {false, true, false, false, false}, {false, true, true, false, false},
    {false, true, true, false, false}, {false, true, false, false, false},
};

void reset(bool pending = false, bool safe = false) {
    track = false;
    require(critical_depth == 0, "prior case left registry locked");
    health_registry() = HealthRegistry{};
    current = {};
    current.pending_verification = pending;
    current.safe_mode = safe;
    current.unhealthy_boots = 7;
    current.health_gate_armed = true;
    health_task_handle = &current;
    fake_time = kNow;
    store_ok = true;
    accept_result = rollback_result = ESP_OK;
    stored_count = 99;
    store_calls = accept_calls = rollback_calls = delete_calls = 0;
    critical_entries = timer_calls = snapshot_calls = 0;
    logs.clear(); events.clear();
}

void set_record(const char *name, HealthState state = HealthState::ok,
                std::int64_t timestamp = kNow) {
    fake_time = timestamp;
    // Exercise a nonzero failure count and retained previous-success timestamp.
    require(health_registry().set(name, HealthState::ok, "private-health-detail"), "set failed");
    require(health_registry().set(name, HealthState::failed, "private-health-detail"), "set failed");
    require(health_registry().set(name, HealthState::failed, "private-health-detail"), "set failed");
    require(health_registry().set(name, state, "private-health-detail"), "set failed");
    fake_time = kNow;
}

void fill(const char *omit = nullptr, bool usb = true) {
    for (const auto *name : required) {
        if (omit == nullptr || std::strcmp(omit, name) != 0) set_record(name);
    }
    if (usb && (omit == nullptr || std::strcmp(omit, "usb_update") != 0)) set_record("usb_update");
}

bool same_record(const HealthRecord &a, const HealthRecord &b) {
    return a.occupied == b.occupied && a.component == b.component && a.state == b.state &&
           a.detail == b.detail && a.failure_count == b.failure_count &&
           a.last_success_us == b.last_success_us;
}

void check_case(const std::string &name, bool expected, int expected_copies,
                const char *rejected = nullptr, const char *reason = nullptr) {
    const auto before = health_registry().snapshot();
    HealthRecord record{};
    const bool found = rejected && health_registry().copy(rejected, record);
    track = true;
    const bool actual = required_health_is_ok();
    track = false;
    require(actual == expected, name + ": acceptance changed");
    require(timer_calls == 1, name + ": checker clock snapshot changed");
    require(critical_entries == expected_copies, name + ": registry not copied once per checked component");
    const auto after = health_registry().snapshot();
    require(std::equal(before.begin(), before.end(), after.begin(), same_record),
            name + ": checker mutated the registry");
    require(critical_depth == 0, name + ": checker left registry locked");
    if (observe) {
        if (expected) {
            require(logs.empty(), name + ": healthy check emitted a rejection");
        } else {
            require(logs.size() == 1 && logs.front().starts_with("OTA_HEALTH_REJECT "),
                    name + ": missing rejection diagnostics");
            std::string expected_log = "OTA_HEALTH_REJECT component=" + std::string(rejected) +
                " reason=" + reason;
            if (found) {
                expected_log += " state=" + std::to_string(static_cast<unsigned>(record.state)) +
                    " failure_count=" + std::to_string(record.failure_count) +
                    " last_success_us=" + std::to_string(record.last_success_us);
            }
            expected_log += " now_us=" + std::to_string(kNow);
            if (found) expected_log += " max_age_us=" +
                std::to_string(std::strcmp(rejected, "touch") == 0 ? 25'000'000 : 0);
            require(logs.front() == expected_log, name + ": rejection evidence mismatch");
        }
    }
    std::cout << name << ':' << actual << ':' << critical_entries << ':' << snapshot_calls << '\n';
    ++checker_scenarios;
}

void checker_tests() {
    for (int pending = 0; pending != 2; ++pending) {
        for (std::size_t component = 0; component < required.size(); ++component) {
            for (std::size_t state = 0; state < states.size(); ++state) {
                reset(pending); fill(required[component]); set_record(required[component], states[state]);
                const bool expected = allowed[component][state];
                check_case(std::string(required[component]) + "-state-" + std::to_string(state) +
                           "-pending-" + std::to_string(pending), expected,
                           expected ? 6 + pending : static_cast<int>(component) + 1,
                           expected ? nullptr : required[component], "disallowed_state");
            }
            reset(pending); fill(required[component]);
            check_case(std::string(required[component]) + "-missing-pending-" + std::to_string(pending),
                       false, static_cast<int>(component) + 1, required[component], "missing_record");
        }
        for (std::size_t state = 0; state < states.size(); ++state) {
            reset(pending); fill("usb_update"); set_record("usb_update", states[state]);
            const bool expected = !pending || state == 1;
            check_case("usb-state-" + std::to_string(state) + "-pending-" + std::to_string(pending),
                       expected, 6 + pending, expected ? nullptr : "usb_update", "disallowed_state");
        }
        reset(pending); fill("usb_update");
        check_case("usb-missing-pending-" + std::to_string(pending), !pending, 6 + pending,
                   "usb_update", "missing_record");
    }
    struct FreshnessCase { const char *name; std::int64_t timestamp; bool accepted; };
    constexpr FreshnessCase freshness[] = {
        {"touch-zero", 0, false}, {"touch-future", kNow + 1, false},
        {"touch-exact25s", kNow - 25'000'000, true},
        {"touch-25s-plus1us", kNow - 25'000'001, false},
    };
    for (const auto &item : freshness) {
        reset(); fill("touch"); set_record("touch", HealthState::ok, item.timestamp);
        check_case(item.name, item.accepted, item.accepted ? 6 : 3, "touch", "stale_touch_evidence");
    }
    // Every suffix is simultaneously absent: the first failure must win.
    for (std::size_t index = 0; index < required.size(); ++index) {
        reset(true);
        for (std::size_t earlier = 0; earlier < index; ++earlier) set_record(required[earlier]);
        check_case("first-failure-" + std::to_string(index), false, static_cast<int>(index) + 1,
                   required[index], "missing_record");
    }
    reset(true); fill("touch"); set_record("touch", HealthState::failed, 0);
    check_case("state-before-stale", false, 3, "touch", "disallowed_state");
    // Optional radio/update failures must not become essential or reorder checks.
    reset(); fill(); set_record("radio", HealthState::failed); set_record("update", HealthState::failed);
    check_case("optional-failures", true, 6);
}

void task_case(const char *name, bool pending, bool safe, bool healthy,
               HealthGateAction expected_action, int expected_store, int expected_accept,
               int expected_rollback, HealthState expected_recovery, bool expected_pending,
               bool persist_ok = true, int accept_status = ESP_OK) {
    reset(pending, safe); fill();
    if (!healthy) set_record("touch", HealthState::failed);
    store_ok = persist_ok; accept_result = accept_status;
    track = true;
    health_gate_task(nullptr);
    track = false;
    require(store_calls == expected_store && accept_calls == expected_accept &&
            rollback_calls == expected_rollback, std::string(name) + ": task action changed");
    require(!store_calls || stored_count == 0, std::string(name) + ": persisted nonzero healthy count");
    require(current.pending_verification == expected_pending && !current.health_gate_armed &&
            health_task_handle == nullptr && delete_calls == 1,
            std::string(name) + ": task completion/pending state changed");
    require(current.safe_mode == safe && current.unhealthy_boots == 7,
            std::string(name) + ": task changed unrelated boot state");
    HealthRecord recovery{};
    const bool recovery_found = health_registry().copy("recovery", recovery);
    require((expected_recovery == HealthState::unknown && !recovery_found) ||
            (recovery_found && recovery.state == expected_recovery),
            std::string(name) + ": recovery registry outcome changed");
    require(events.size() > 3 && events[0] == "delay:6000" && events[1] == "snapshot" &&
            events[2] == "time" && events[3] == "registry_enter",
            std::string(name) + ": 60-second wait/snapshot/check order changed");
    if (observe) {
        const std::string summary = "OTA_HEALTH_GATE essential_ok=" + std::to_string(healthy) +
            " pending=" + std::to_string(pending) + " safe_mode=" + std::to_string(safe) +
            " bad_boots=7 action=" + std::to_string(static_cast<unsigned>(expected_action));
        require(std::count(logs.begin(), logs.end(), summary) == 1,
                std::string(name) + ": missing/duplicate/incorrect gate summary");
        const auto summary_event = std::find(events.begin(), events.end(), "log:" + summary);
        require(std::count(events.begin(), summary_event, "registry_enter") ==
                    (healthy ? 6 + static_cast<int>(pending) : 3) &&
                std::count(events.begin(), summary_event, "snapshot") == (healthy ? 2 : 1),
                std::string(name) + ": decision log moved before checks or after registry action");
        const auto first_effect = std::find_if(events.begin(), events.end(), [](const auto &item) {
            return item.starts_with("store:") || item == "accept" || item == "rollback" ||
                   item.starts_with("publish:");
        });
        require(summary_event != events.end() && summary_event < first_effect,
                std::string(name) + ": decision logged after a gate effect");
        if (!healthy) {
            const auto rejection = std::find_if(events.begin(), events.end(), [](const auto &item) {
                return item.starts_with("log:OTA_HEALTH_REJECT ");
            });
            require(rejection < summary_event, std::string(name) + ": decision logged before check");
        }
    }
    std::cout << "task-" << name << ':';
    for (const auto &item : events) if (!item.starts_with("log:")) std::cout << item << ',';
    std::cout << "recovery=" << static_cast<unsigned>(recovery.state) << '\n';
    ++task_scenarios;
}

int main(int argc, char **argv) {
    try {
        observe = argc < 2 || std::string_view(argv[1]) != "--semantics-only";
        checker_tests();
        task_case("pending-healthy", true, false, true, HealthGateAction::accept_pending_image,
                  1, 1, 0, HealthState::ok, false);
        task_case("pending-safe-healthy", true, true, true, HealthGateAction::rollback_pending_image,
                  0, 0, 1, HealthState::failed, true);
        task_case("pending-unhealthy", true, false, false, HealthGateAction::rollback_pending_image,
                  0, 0, 1, HealthState::failed, true);
        task_case("pending-safe-unhealthy", true, true, false, HealthGateAction::rollback_pending_image,
                  0, 0, 1, HealthState::failed, true);
        task_case("valid-healthy", false, false, true, HealthGateAction::record_healthy,
                  1, 0, 0, HealthState::ok, false);
        task_case("valid-safe-healthy", false, true, true, HealthGateAction::record_healthy,
                  1, 0, 0, HealthState::ok, false);
        task_case("valid-unhealthy", false, false, false, HealthGateAction::retain_unhealthy,
                  0, 0, 0, HealthState::failed, false);
        task_case("valid-safe-unhealthy", false, true, false, HealthGateAction::retain_unhealthy,
                  0, 0, 0, HealthState::failed, false);
        task_case("pending-persist-failure", true, false, true, HealthGateAction::accept_pending_image,
                  1, 0, 1, HealthState::failed, true, false);
        task_case("pending-accept-failure", true, false, true, HealthGateAction::accept_pending_image,
                  1, 1, 1, HealthState::failed, true, true, 1);
        task_case("valid-persist-failure", false, false, true, HealthGateAction::record_healthy,
                  1, 0, 0, HealthState::unknown, false, false);
        std::cout << "cases=" << checker_scenarios << '+' << task_scenarios << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
'''


def build(directory: Path, source_path: Path, name: str) -> Path:
    source = source_path.read_text()
    cpp, binary = directory / f"{name}.cpp", directory / name
    cpp.write_text(PRELUDE + "\n\n".join(function(source, signature) for signature in (
        "bool required_health_is_ok()", "void health_gate_task(void *)",
    )) + SCENARIOS)
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-I", str(directory), "-I", str(ROOT / "components/nightglass_core/include"),
        "-I", str(ROOT / "components/nightglass_update/include"), str(cpp),
        str(ROOT / "components/nightglass_core/src/health.cpp"),
        str(ROOT / "components/nightglass_update/src/policy.cpp"), "-o", str(binary),
    ], check=True)
    return binary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "components/nightglass_update/src/service.cpp")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nightglass-health-gate-test-") as temporary:
        directory = Path(temporary)
        (directory / "freertos").mkdir()
        (directory / "freertos/FreeRTOS.h").write_text(FREERTOS)
        (directory / "esp_timer.h").write_text("#pragma once\n#include <cstdint>\nstd::int64_t esp_timer_get_time();\n")
        candidate = build(directory, args.source, "candidate")
        result = subprocess.run([str(candidate)], text=True, capture_output=True)
        if result.returncode:
            raise SystemExit(result.stderr)
        digest = hashlib.sha256(result.stdout.encode()).hexdigest()
        print(f"Health gate diagnostics passed ({result.stdout.splitlines()[-1]}; semantic SHA256 {digest})")
        if args.baseline:
            baseline = build(directory, args.baseline, "baseline")
            previous = subprocess.run([str(baseline), "--semantics-only"], text=True, capture_output=True)
            if previous.returncode:
                raise SystemExit(previous.stderr)
            if previous.stdout != result.stdout:
                raise SystemExit("Candidate acceptance/control-flow differs from baseline")
            negative = subprocess.run([str(baseline)], text=True, capture_output=True)
            if negative.returncode == 0 or "missing rejection diagnostics" not in negative.stderr:
                raise SystemExit("Baseline did not fail the expected new observability assertion")
            print("Baseline comparison passed: identical semantics; baseline intentionally fails new diagnostics")


if __name__ == "__main__":
    main()

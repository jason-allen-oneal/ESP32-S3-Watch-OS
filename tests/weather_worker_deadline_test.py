#!/usr/bin/env python3
"""Execute the production idle scheduler against standby and retry boundaries."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "components/nightglass_services/src/network_weather.cpp").read_text()
start = source.index("TickType_t worker_wait_ticks() {")
end = source.index("\nvoid worker(void *)", start)
definition = source[start:end]
stub = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
using TickType_t = std::uint32_t;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr TickType_t pdMS_TO_TICKS(std::uint64_t ms) { return ms / 10; }
void portENTER_CRITICAL(void *) {}
void portEXIT_CRITICAL(void *) {}
int state_mux;
namespace nightglass::core {
enum class PowerState { active, dim, ambient, screen_blank, light_sleep };
}
enum class WeatherSource { none, phone, direct };
struct NetworkWeatherSnapshot {
    struct Settings { std::uint16_t refresh_minutes = 30;
        bool enabled = false, location_configured = false; } settings;
    bool data_valid = false, stale = false, credentials_configured = false;
    std::uint32_t age_seconds = 0;
    WeatherSource source = WeatherSource::none;
} current;
std::atomic_bool sleep_suspended{false};
std::atomic<std::int64_t> next_fetch_us{0};
std::int64_t now_us = 1'000'000;
std::int64_t esp_timer_get_time() { return now_us; }
void update_age(std::int64_t) {}
struct PowerDouble {
    nightglass::core::PowerState state = nightglass::core::PowerState::active;
    PowerDouble snapshot() { return *this; }
} power;
PowerDouble &power_service() { return power; }
'''
checks = r'''
int main() {
    assert(worker_wait_ticks() == portMAX_DELAY); // Disabled, no cache.
    current.settings.enabled = current.settings.location_configured = true;
    current.credentials_configured = true;
    assert(worker_wait_ticks() == 100); // Wi-Fi startup bounded to one second.
    next_fetch_us = now_us + 30'000'000;
    assert(worker_wait_ticks() == 3000); // Retry deadline, not 30 polls.
    current.data_valid = true;
    current.source = WeatherSource::phone;
    current.age_seconds = 3600;
    assert(worker_wait_ticks() == 100); // Strict >3600 stale boundary.
    current.age_seconds = 3590;
    assert(worker_wait_ticks() == 1100);
    current.source = WeatherSource::direct;
    assert(worker_wait_ticks() == 1100); // Stale deadline beats fetch deadline.
    current.stale = true;
    power.state = nightglass::core::PowerState::screen_blank;
    assert(worker_wait_ticks() == portMAX_DELAY); // Wake via power notification.
    power.state = nightglass::core::PowerState::active;
    assert(worker_wait_ticks() == 3000);
    next_fetch_us = now_us - 1;
    assert(worker_wait_ticks() == 100);
    sleep_suspended = true;
    assert(worker_wait_ticks() == portMAX_DELAY);
    sleep_suspended = false;
    current.stale = false;
    current.age_seconds = 0;
    current.settings.refresh_minutes = UINT16_MAX;
    current.source = WeatherSource::phone;
    assert(worker_wait_ticks() == 786420100); // No millisecond overflow.
}
'''
with tempfile.TemporaryDirectory(prefix="nightglass-weather-deadline-") as temp:
    cpp = Path(temp) / "test.cpp"
    binary = Path(temp) / "test"
    cpp.write_text(stub + definition + checks)
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Weather idle/deadline scheduler passed")

#!/usr/bin/env python3
"""Exercise the production progressive reader with host mutex/task adapters.

This checks producer/consumer fencing, not physical I2S or BLE throughput.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/nightglass_services/src/voice.cpp').read_text()
start = source.index('bool read_spoken_stream(')
end = source.index('\n}\n', start) + 2
reader = source[start:end]
harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
using namespace std::chrono_literals;
struct VoicePlaybackContext {
    std::uint32_t session_id{};
    std::uint8_t *buffer{};
    std::size_t bytes{};
    std::int64_t stream_deadline_us{};
};
std::timed_mutex response_mutex_value;
auto *response_mutex = &response_mutex_value;
constexpr bool pdTRUE = true;
int pdMS_TO_TICKS(int x) { return x; }
bool xSemaphoreTake(std::timed_mutex *m, int ms) {
    return m->try_lock_for(std::chrono::milliseconds(ms));
}
void xSemaphoreGive(std::timed_mutex *m) { m->unlock(); }
void vTaskDelay(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
std::int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic_bool cancel_requested{false};
bool audio_stream_failed{}, audio_response_streaming{true}, audio_stream_ended{};
std::uint8_t *audio_response_buffer{};
std::size_t audio_response_received{};
'''
tests = r'''
int main() {
    std::uint8_t samples[4]{1,2,3,4}, output[4]{};
    audio_response_buffer = samples;
    VoicePlaybackContext context{7, samples, 4, esp_timer_get_time() + 2'000'000};
    std::atomic_bool completed{false};
    std::thread consumer([&] {
        assert(read_spoken_stream(&context, 0, output));
        completed.store(true);
    });
    std::this_thread::sleep_for(20ms);
    assert(!completed.load()); // No read from unpublished samples.
    { std::lock_guard guard(response_mutex_value); audio_response_received = 2; }
    std::this_thread::sleep_for(20ms);
    assert(!completed.load()); // Exact requested range must be ready.
    { std::lock_guard guard(response_mutex_value); audio_response_received = 4; }
    consumer.join();
    assert(std::equal(output, output + 4, samples));
    assert(!read_spoken_stream(&context, 3, std::span(output, 2)));
    completed = false;
    std::thread finish([&] {
        assert(read_spoken_stream(&context, 4, {}));
        completed = true;
    });
    std::this_thread::sleep_for(20ms);
    assert(!completed.load()); // Samples alone don't substitute for verified END.
    { std::lock_guard guard(response_mutex_value); audio_stream_ended = true; }
    finish.join();
    cancel_requested = true;
    assert(!read_spoken_stream(&context, 0, output));
    cancel_requested = false;
    audio_stream_failed = true;
    assert(!read_spoken_stream(&context, 0, output));
    audio_stream_failed = false;
    audio_response_buffer = nullptr;
    assert(!read_spoken_stream(&context, 0, output)); // Ownership revoked.
    audio_response_buffer = samples;
    audio_response_received = 0;
    context.stream_deadline_us = esp_timer_get_time();
    assert(!read_spoken_stream(&context, 0, output)); // Bounded starvation.
}
'''
with tempfile.TemporaryDirectory(prefix='nightglass-stream-test-') as directory:
    code = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    code.write_text(harness + reader + tests)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('Progressive voice reader passed: unpublished ranges, end, ownership, cancel, starvation')

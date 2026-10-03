#!/usr/bin/env python3
"""Exercise production GPIO hand-off callbacks with failure and race injection."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "components/nightglass_services/src/power.cpp").read_text()
start = source.index("std::atomic_bool automatic_sleep_ready")
end = source.index("void IRAM_ATTR side_key_interrupt", start)
production = source[start:end]
stub = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#define IRAM_ATTR
using esp_err_t = int;
using BaseType_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, pdFALSE = 0;
constexpr int kTouchInterruptGpio = 38, kSideKeyGpio = 10;
constexpr int GPIO_INTR_NEGEDGE = 2, GPIO_INTR_LOW_LEVEL = 4;
constexpr int ESP_SLEEP_WAKEUP_GPIO = 1;
void *supervisor_task = reinterpret_cast<void *>(1);
int touch_level = 1, button_level = 0, cause = 0, notifications = 0;
int fail_at = 0, operation = 0, mode = GPIO_INTR_NEGEDGE;
bool irq = true, wake = false, late_touch = false;
std::vector<int> actions;
int result(int action) { actions.push_back(action); return ++operation == fail_at ? ESP_FAIL : ESP_OK; }
int gpio_get_level(int pin) { return pin == kTouchInterruptGpio ? touch_level : button_level; }
int gpio_intr_disable(int) { int r=result(1); if (!r) irq=false; return r; }
int gpio_wakeup_enable(int, int requested) {
    assert(!irq); int r=result(2); if (!r) {wake=true; mode=requested;}
    return r;
}
int gpio_wakeup_disable(int) { int r=result(3); if (!r) wake=false; return r; }
int gpio_set_intr_type(int, int requested) { int r=result(4); if (!r) mode=requested; return r; }
int gpio_intr_enable(int) {
    int r=result(5); if (!r) irq=true;
    if (late_touch) touch_level=0;
    return r;
}
int esp_sleep_get_wakeup_cause() { return cause; }
void vTaskNotifyGiveFromISR(void *, BaseType_t *) { ++notifications; }
'''
checks = r'''
void reset() {
    automatic_sleep_ready = false; automatic_sleep_fault = false;
    automatic_touch_pending = false; automatic_sleep_count = 0; automatic_sleep_ms = 0;
    automatic_touch_armed = false; touch_level=1; button_level=0; cause=0;
    notifications=0; fail_at=0; operation=0; mode=GPIO_INTR_NEGEDGE;
    irq=true; wake=false; late_touch=false; actions.clear();
}
int main() {
    reset(); assert(skip_automatic_sleep()); // Active/AOD/USB: readiness veto.
    automatic_sleep_ready=true; assert(!skip_automatic_sleep());
    touch_level=0; assert(skip_automatic_sleep());
    assert(automatic_touch_pending && notifications==1 && !automatic_sleep_ready);
    reset(); automatic_sleep_ready=true; button_level=1; assert(skip_automatic_sleep());
    reset(); assert(automatic_sleep_enter(100000,nullptr)==ESP_OK);
    assert(!irq && wake && mode==GPIO_INTR_LOW_LEVEL);
    assert(automatic_sleep_exit(50000,nullptr)==ESP_OK);
    assert(irq && !wake && mode==GPIO_INTR_NEGEDGE && !automatic_touch_armed);
    assert(automatic_sleep_count==1 && automatic_sleep_ms==50 && !automatic_touch_pending);
    assert(actions==std::vector<int>({1,2,3,4,5}));
    for (int race=0; race<3; ++race) {
        reset(); automatic_sleep_ready=true;
        if (race==0) touch_level=0; // Touch after veto, before entry.
        automatic_sleep_enter(100000,nullptr);
        if (race==1) cause=ESP_SLEEP_WAKEUP_GPIO; // Pulse released before exit.
        if (race==2) late_touch=true; // Assertion while restoring IRQ.
        automatic_sleep_exit(40000,nullptr);
        assert(automatic_touch_pending && notifications>=1 && !automatic_sleep_ready);
        assert(irq && !wake && mode==GPIO_INTR_NEGEDGE);
    }
    for (int failure=1; failure<=5; ++failure) {
        reset(); fail_at=failure;
        automatic_sleep_enter(100000,nullptr);
        automatic_sleep_exit(0,nullptr);
        assert(automatic_sleep_fault && skip_automatic_sleep());
        assert(!automatic_touch_armed && notifications>=1);
        assert(automatic_sleep_count==0); // Rejected/skipped window isn't evidence.
    }
    reset(); cause=ESP_SLEEP_WAKEUP_GPIO; // Old cause after a skipped short window.
    automatic_sleep_enter(1,nullptr); automatic_sleep_exit(0,nullptr);
    assert(!automatic_touch_pending && automatic_sleep_count==0);
    for (int i=0;i<1000;++i) {
        automatic_sleep_enter(100000,nullptr); automatic_sleep_exit(50000,nullptr);
        assert(irq && !wake && mode==GPIO_INTR_NEGEDGE);
    }
    assert(automatic_sleep_count==1000 && automatic_sleep_ms==50000);
}
'''
with tempfile.TemporaryDirectory(prefix="nightglass-auto-sleep-") as temp:
    cpp = Path(temp) / "test.cpp"
    binary = Path(temp) / "test"
    cpp.write_text(stub + production + checks)
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Automatic sleep GPIO hand-off passed: races, faults, 1000 windows")

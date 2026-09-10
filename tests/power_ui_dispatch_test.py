#!/usr/bin/env python3
"""Execute production power/UI callbacks against a task-aware LVGL test double.

This checks callback ownership and timer lifecycle, not source-string contracts.
The real callback bodies are compiled unchanged; only the hardware, LVGL and
rendering boundaries are doubled. It is not a physical wake or stack-usage test.
Pass --source to run the same regression against an earlier shell.cpp.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, default=ROOT / "components/nightglass_ui/src/shell.cpp")
args = parser.parse_args()
source = args.source.read_text()


def method(name: str, optional: bool = False) -> str:
    start = source.find(f"void Shell::{name}(")
    if start < 0:
        if optional:
            return ""
        raise AssertionError(f"Missing production method: {name}")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

enum class Task { power, lvgl };
Task task = Task::power;
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
void rendering() {
    require(task == Task::lvgl, "rendering escaped onto power-supervisor task");
}
namespace nightglass::core {
enum class PowerState { active, dim, ambient, screen_blank, light_sleep, deep_sleep };
}
using State = nightglass::core::PowerState;
namespace nightglass::services {
struct Power { struct Snapshot { State state; }; State state = State::active;
    Snapshot snapshot() { return {state}; } } power;
struct Voice { bool quiescent() { return true; } void cancel() {} } voice;
struct Premium { int profile() { return 0; } } premium;
Power &power_service() { return power; }
Voice &voice_service() { return voice; }
Premium &premium_service() { return premium; }
}
struct lv_timer_t { bool paused = false; bool ready = false; void *data = nullptr; };
void *lv_timer_get_user_data(lv_timer_t *timer) { return timer->data; }
void lv_timer_pause(lv_timer_t *timer) { timer->paused = true; }
void lv_timer_resume(lv_timer_t *timer) { timer->paused = false; }
void lv_timer_ready(lv_timer_t *timer) { timer->ready = true; }
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1;
constexpr int LVGL_PORT_EVENT_USER = 1;
bool tick_enabled = true;
int resume_result = ESP_OK;
int user_wakes = 0;
bool lvgl_port_lock(int) { return true; }
void lvgl_port_unlock() {}
int lvgl_port_stop() { tick_enabled = false; return ESP_OK; }
int lvgl_port_resume() {
    if (resume_result == ESP_OK) tick_enabled = true;
    return resume_result;
}
void lvgl_port_task_wake(int, void *) { ++user_wakes; }
const char *esp_err_to_name(int) { return "test failure"; }
#define ESP_LOGE(...) do {} while (0)
struct lv_obj_t {} object;
constexpr int kPanelWidth = 410, kPanelHeight = 502;
constexpr int LV_OPA_COVER = 255, LV_OBJ_FLAG_SCROLLABLE = 1, LV_OBJ_FLAG_HIDDEN = 2;
constexpr int lv_font_montserrat_48 = 48, lv_font_montserrat_20 = 20;
int lv_color_black() { return 0; }
lv_obj_t *lv_obj_create(lv_obj_t *) { rendering(); return &object; }
void lv_obj_remove_style_all(lv_obj_t *) { rendering(); }
void lv_obj_set_size(lv_obj_t *, int, int) { rendering(); }
void lv_obj_set_style_bg_color(lv_obj_t *, int, int) { rendering(); }
void lv_obj_set_style_bg_opa(lv_obj_t *, int, int) { rendering(); }
void lv_obj_set_style_opa(lv_obj_t *, int, int) { rendering(); }
void lv_obj_remove_flag(lv_obj_t *, int) { rendering(); }
void lv_obj_add_flag(lv_obj_t *, int) { rendering(); }
void lv_obj_move_foreground(lv_obj_t *) { rendering(); }
void lv_anim_delete(lv_obj_t *, void *) { rendering(); }
lv_obj_t *label(lv_obj_t *, const char *, const int *, int) { rendering(); return &object; }

class Shell {
public:
    std::atomic<State> power_state_{State::active};
    std::atomic_bool ui_pipeline_suspended_{false};
    lv_timer_t route_timer{false, false, this}, system_timer{false, false, this};
    lv_timer_t *timer_ = &route_timer, *system_timer_ = &system_timer;
    bool ambient_visible_ = false;
    lv_obj_t *ambient_layer_ = nullptr, *ambient_time_ = nullptr, *ambient_date_ = nullptr;
    lv_obj_t *screen_ = &object, *content_host_ = &object;
    std::uint32_t ambient_minute_ = 0;
    int rendered_profile_ = 0, route_draws = 0, overlay_draws = 0, ambient_draws = 0;
    void refresh_active_route() { rendering(); if (!ambient_visible_) ++route_draws; }
    void refresh_system_overlay() { rendering(); ++overlay_draws; }
    void refresh_ambient() { rendering(); ++ambient_draws; }
    void render_route() { rendering(); }
    void install_touch_callbacks(lv_obj_t *) { rendering(); }
    void handle_side_key(bool) {}
    void handle_gesture() {}
    void apply_power_state(State);
    void apply_ambient_state(State);
    static void power_state_callback(State, void *);
    static void timer_callback(lv_timer_t *);
    static void system_timer_callback(lv_timer_t *);
};
'''

SCENARIOS = r'''
void transition(Shell &shell, State state) {
    task = Task::power;
    nightglass::services::power.state = state;
    Shell::power_state_callback(state, &shell);
}
void dispatch(Shell &shell) {
    require(tick_enabled, "LVGL tick remained stopped on wake");
    require(user_wakes > 0, "wake never notified the LVGL event loop");
    user_wakes = 0;
    task = Task::lvgl;
    if (shell.system_timer_ && !shell.system_timer_->paused && shell.system_timer_->ready) {
        shell.system_timer_->ready = false;
        Shell::system_timer_callback(shell.system_timer_);
    }
    if (shell.timer_ && !shell.timer_->paused && shell.timer_->ready) {
        shell.timer_->ready = false;
        Shell::timer_callback(shell.timer_);
    }
    require(user_wakes == 0, "system timer rescheduled itself without a state change");
}
void blank(Shell &shell) {
    transition(shell, State::screen_blank);
    require(shell.ui_pipeline_suspended_, "blank did not suspend the pipeline");
    require(!tick_enabled && shell.system_timer_->paused, "blank left system timer active");
    if (shell.timer_) require(shell.timer_->paused, "blank left route timer active");
}
int main() {
    try {
        Shell shell;
        // Repeated dim/blank wakes preserve ownership and promptly refresh both layers.
        for (int i = 0; i < 100; ++i) {
            transition(shell, State::dim);
            blank(shell);
            const int routes = shell.route_draws, overlays = shell.overlay_draws;
            transition(shell, State::active);
            require(shell.route_draws == routes && shell.overlay_draws == overlays,
                    "power observer rendered synchronously");
            require(!shell.ui_pipeline_suspended_, "wake stayed suspended");
            dispatch(shell);
            require(shell.route_draws > routes && shell.overlay_draws > overlays,
                    "wake did not refresh route and overlay");
        }
        // Static screens may have no route timer; the system timer must still recover.
        shell.timer_ = nullptr;
        blank(shell);
        transition(shell, State::active);
        const int overlays = shell.overlay_draws;
        dispatch(shell);
        require(shell.overlay_draws > overlays, "timerless route lost system wake");
        shell.timer_ = &shell.route_timer;

        // Ambient UI object creation/hiding belongs to LVGL, including wake from blank.
        blank(shell);
        transition(shell, State::ambient);
        dispatch(shell);
        require(shell.ambient_visible_ && shell.timer_->paused, "ambient left normal refresh active");
        blank(shell);
        transition(shell, State::ambient);
        dispatch(shell);
        require(shell.ambient_visible_ && shell.timer_->paused, "ambient resume unpaused normal refresh");
        blank(shell);
        transition(shell, State::active);
        const int routes = shell.route_draws;
        dispatch(shell);
        require(!shell.ambient_visible_ && shell.route_draws > routes,
                "ambient exit stranded the normal route");

        // Failed tick resume remains suspended and can be retried in the same state.
        blank(shell);
        resume_result = -1;
        transition(shell, State::active);
        require(shell.ui_pipeline_suspended_ && shell.system_timer_->paused,
                "failed tick resume reported a working UI");
        resume_result = ESP_OK;
        transition(shell, State::active);
        dispatch(shell);
        std::cout << "Nightglass power/UI dispatch regression passed (100 wake cycles)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
'''

bodies = "\n\n".join(method(name, optional=name == "apply_ambient_state") for name in (
    "timer_callback", "system_timer_callback", "power_state_callback",
    "apply_power_state", "apply_ambient_state",
))
with tempfile.TemporaryDirectory(prefix="nightglass-power-ui-test-") as directory:
    cpp = Path(directory) / "callbacks.cpp"
    binary = Path(directory) / "callbacks"
    cpp.write_text(PRELUDE + bodies + SCENARIOS)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra",
                    "-Werror", "-pedantic", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

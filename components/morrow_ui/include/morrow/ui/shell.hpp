#pragma once

#include <cstdint>

#include "lvgl.h"
#include "morrow/core/status.hpp"

namespace morrow::ui {

class Shell {
public:
    morrow::core::Status start();
    void stop();

private:
    static void timer_callback(lv_timer_t *timer);
    static void haptic_callback(lv_event_t *event);
    void refresh();

    lv_obj_t *screen_{nullptr};
    lv_timer_t *timer_{nullptr};
    lv_obj_t *rtc_time_{nullptr};
    lv_obj_t *rtc_status_{nullptr};
    lv_obj_t *rtc_detail_{nullptr};
    lv_obj_t *battery_percent_{nullptr};
    lv_obj_t *battery_voltage_{nullptr};
    lv_obj_t *battery_status_{nullptr};
    lv_obj_t *battery_detail_{nullptr};
    lv_obj_t *battery_bar_{nullptr};
    lv_obj_t *motion_state_{nullptr};
    lv_obj_t *motion_status_{nullptr};
    lv_obj_t *motion_accel_{nullptr};
    lv_obj_t *motion_gyro_{nullptr};
    lv_obj_t *haptic_button_{nullptr};
    lv_obj_t *haptic_primary_{nullptr};
    lv_obj_t *haptic_result_{nullptr};
    std::uint32_t observed_accepted_pulses_{0};
    std::uint32_t observed_failed_pulses_{0};
    bool haptic_waiting_{false};
};

Shell &shell();

}  // namespace morrow::ui

#pragma once

#include <cstdint>

#include "lvgl.h"
#include "nightglass/core/navigation.hpp"
#include "nightglass/core/status.hpp"

namespace nightglass::ui {

class Shell {
public:
    nightglass::core::Status start();
    void stop();

private:
    static void timer_callback(lv_timer_t *timer);
    static void input_callback(lv_event_t *event);
    static void back_callback(lv_event_t *event);
    static void launcher_callback(lv_event_t *event);
    static void settings_callback(lv_event_t *event);
    static void active_brightness_callback(lv_event_t *event);
    static void dim_brightness_callback(lv_event_t *event);
    static void dim_after_callback(lv_event_t *event);
    static void blank_after_callback(lv_event_t *event);
    static void sleep_after_callback(lv_event_t *event);
    static void diagnostics_callback(lv_event_t *event);
    static void about_callback(lv_event_t *event);

    void navigate(nightglass::core::NavigationAction action);
    void render_route();
    void render_home();
    void render_launcher();
    void render_settings();
    void render_diagnostics();
    void render_about();
    void configure_refresh_timer(std::uint32_t period_ms);
    void refresh_active_route();
    void refresh_home();
    void refresh_diagnostics();
    void clear_route_objects();
    void refresh_settings_labels();

    nightglass::core::NavigationState navigation_{};
    lv_obj_t *screen_{nullptr};
    lv_obj_t *content_host_{nullptr};
    lv_obj_t *overlay_layer_{nullptr};
    lv_indev_t *touch_input_{nullptr};
    lv_timer_t *timer_{nullptr};

    // Settings route widgets.
    lv_obj_t *setting_active_{nullptr};
    lv_obj_t *setting_dim_{nullptr};
    lv_obj_t *setting_dim_after_{nullptr};
    lv_obj_t *setting_blank_after_{nullptr};
    lv_obj_t *setting_sleep_after_{nullptr};

    // Home route widgets.
    lv_obj_t *home_time_{nullptr};
    lv_obj_t *home_time_state_{nullptr};
    lv_obj_t *home_date_{nullptr};
    lv_obj_t *home_battery_{nullptr};
    lv_obj_t *home_battery_detail_{nullptr};
    lv_obj_t *home_motion_{nullptr};

    // Diagnostics widgets exist only while that route is active.
    lv_obj_t *diagnostics_rtc_state_{nullptr};
    lv_obj_t *diagnostics_rtc_detail_{nullptr};
    lv_obj_t *diagnostics_battery_state_{nullptr};
    lv_obj_t *diagnostics_battery_detail_{nullptr};
    lv_obj_t *diagnostics_motion_state_{nullptr};
    lv_obj_t *diagnostics_motion_detail_{nullptr};
    lv_obj_t *diagnostics_haptic_state_{nullptr};
    lv_obj_t *diagnostics_haptic_detail_{nullptr};
};

Shell &shell();

}  // namespace nightglass::ui

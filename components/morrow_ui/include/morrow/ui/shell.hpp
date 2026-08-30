#pragma once

#include <cstdint>

#include "lvgl.h"
#include "morrow/core/navigation.hpp"
#include "morrow/core/status.hpp"

namespace morrow::ui {

class Shell {
public:
    morrow::core::Status start();
    void stop();

private:
    static void timer_callback(lv_timer_t *timer);
    static void back_callback(lv_event_t *event);
    static void launcher_callback(lv_event_t *event);
    static void diagnostics_callback(lv_event_t *event);
    static void about_callback(lv_event_t *event);

    void navigate(morrow::core::NavigationAction action);
    void render_route();
    void render_home();
    void render_launcher();
    void render_diagnostics();
    void render_about();
    void configure_refresh_timer(std::uint32_t period_ms);
    void refresh_active_route();
    void refresh_home();
    void refresh_diagnostics();
    void clear_route_objects();

    morrow::core::NavigationState navigation_{};
    lv_obj_t *screen_{nullptr};
    lv_obj_t *content_host_{nullptr};
    lv_obj_t *overlay_layer_{nullptr};
    lv_timer_t *timer_{nullptr};

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

}  // namespace morrow::ui

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
    static void system_timer_callback(lv_timer_t *timer);
    static void input_callback(lv_event_t *event);
    static void back_callback(lv_event_t *event);
    static void launcher_callback(lv_event_t *event);
    static void settings_callback(lv_event_t *event);
    static void power_settings_callback(lv_event_t *event);
    static void clock_settings_callback(lv_event_t *event);
    static void watchface_settings_callback(lv_event_t *event);
    static void watchface_next_callback(lv_event_t *event);
    static void activity_callback(lv_event_t *event);
    static void activity_stride_callback(lv_event_t *event);
    static void activity_goal_callback(lv_event_t *event);
    static void activity_reset_callback(lv_event_t *event);
    static void weather_callback(lv_event_t *event);
    static void weather_toggle_callback(lv_event_t *event);
    static void weather_units_callback(lv_event_t *event);
    static void weather_refresh_callback(lv_event_t *event);
    static void weather_latitude_callback(lv_event_t *event);
    static void weather_longitude_callback(lv_event_t *event);
    static void weather_clear_wifi_callback(lv_event_t *event);
    static void connectivity_callback(lv_event_t *event);
    static void connectivity_toggle_callback(lv_event_t *event);
    static void media_callback(lv_event_t *event);
    static void alarm_callback(lv_event_t *event);
    static void countdown_callback(lv_event_t *event);
    static void stopwatch_callback(lv_event_t *event);
    static void active_brightness_callback(lv_event_t *event);
    static void dim_brightness_callback(lv_event_t *event);
    static void dim_after_callback(lv_event_t *event);
    static void blank_after_callback(lv_event_t *event);
    static void sleep_after_callback(lv_event_t *event);
    static void time_format_callback(lv_event_t *event);
    static void utc_offset_callback(lv_event_t *event);
    static void dst_callback(lv_event_t *event);
    static void alarm_hour_callback(lv_event_t *event);
    static void alarm_minute_callback(lv_event_t *event);
    static void alarm_enabled_callback(lv_event_t *event);
    static void countdown_duration_callback(lv_event_t *event);
    static void countdown_toggle_callback(lv_event_t *event);
    static void countdown_reset_callback(lv_event_t *event);
    static void stopwatch_toggle_callback(lv_event_t *event);
    static void stopwatch_reset_callback(lv_event_t *event);
    static void dismiss_alert_callback(lv_event_t *event);
    static void diagnostics_callback(lv_event_t *event);
    static void about_callback(lv_event_t *event);

    void navigate(nightglass::core::NavigationAction action);
    void render_route();
    void render_home();
    void render_launcher();
    void render_settings();
    void render_power_settings();
    void render_clock_settings();
    void render_watchface_settings();
    void render_activity();
    void render_weather();
    void render_connectivity();
    void render_classic_home();
    void render_pack_home();
    void render_alarm();
    void render_countdown();
    void render_stopwatch();
    void render_diagnostics();
    void render_about();
    void configure_refresh_timer(std::uint32_t period_ms);
    void refresh_active_route();
    void refresh_home();
    void refresh_clock_settings();
    void refresh_watchface_settings();
    void refresh_activity();
    void refresh_weather();
    void refresh_connectivity();
    void refresh_alarm();
    void refresh_countdown();
    void refresh_stopwatch();
    void refresh_system_overlay();
    void refresh_diagnostics();
    void clear_route_objects();
    void refresh_settings_labels();

    nightglass::core::NavigationState navigation_{};
    lv_obj_t *screen_{nullptr};
    lv_obj_t *content_host_{nullptr};
    lv_obj_t *overlay_layer_{nullptr};
    lv_indev_t *touch_input_{nullptr};
    lv_timer_t *timer_{nullptr};
    lv_timer_t *system_timer_{nullptr};

    // Settings route widgets.
    lv_obj_t *setting_active_{nullptr};
    lv_obj_t *setting_dim_{nullptr};
    lv_obj_t *setting_dim_after_{nullptr};
    lv_obj_t *setting_blank_after_{nullptr};
    lv_obj_t *setting_sleep_after_{nullptr};
    lv_obj_t *setting_time_format_{nullptr};
    lv_obj_t *setting_utc_offset_{nullptr};
    lv_obj_t *setting_dst_{nullptr};
    lv_obj_t *clock_preview_{nullptr};
    lv_obj_t *watchface_name_{nullptr};
    lv_obj_t *activity_steps_{nullptr};
    lv_obj_t *activity_detail_{nullptr};
    lv_obj_t *activity_stride_{nullptr};
    lv_obj_t *activity_goal_{nullptr};
    lv_obj_t *weather_state_{nullptr};
    lv_obj_t *weather_detail_{nullptr};
    lv_obj_t *weather_toggle_{nullptr};
    lv_obj_t *weather_units_{nullptr};
    lv_obj_t *weather_refresh_{nullptr};
    lv_obj_t *weather_latitude_{nullptr};
    lv_obj_t *weather_longitude_{nullptr};
    lv_obj_t *connectivity_state_{nullptr};
    lv_obj_t *connectivity_detail_{nullptr};
    lv_obj_t *connectivity_toggle_{nullptr};

    // Core clock app widgets.
    lv_obj_t *alarm_time_{nullptr};
    lv_obj_t *alarm_state_{nullptr};
    lv_obj_t *alarm_toggle_{nullptr};
    lv_obj_t *countdown_time_{nullptr};
    lv_obj_t *countdown_duration_{nullptr};
    lv_obj_t *countdown_toggle_{nullptr};
    lv_obj_t *stopwatch_time_{nullptr};
    lv_obj_t *stopwatch_toggle_{nullptr};
    lv_obj_t *alert_card_{nullptr};
    std::uint8_t displayed_alert_kind_{0};

    // Home route widgets.
    lv_obj_t *home_time_{nullptr};
    lv_obj_t *home_time_state_{nullptr};
    lv_obj_t *home_day_{nullptr};
    lv_obj_t *home_date_{nullptr};
    lv_obj_t *home_battery_{nullptr};
    lv_obj_t *home_battery_detail_{nullptr};
    lv_obj_t *home_motion_{nullptr};
    lv_obj_t *home_steps_{nullptr};
    lv_obj_t *home_alarm_{nullptr};
    lv_obj_t *home_timer_{nullptr};
    lv_obj_t *home_distance_{nullptr};
    lv_obj_t *home_weather_{nullptr};
    lv_obj_t *home_notifications_{nullptr};

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

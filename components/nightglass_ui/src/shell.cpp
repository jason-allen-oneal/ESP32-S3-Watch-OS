#include "nightglass/ui/shell.hpp"

#include <cstdio>
#include <cstdint>

#include "esp_timer.h"
#include "lvgl.h"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/power.hpp"

namespace nightglass::ui {

namespace {
Shell instance;

constexpr std::uint32_t kVoid = 0x000000;
constexpr std::uint32_t kSurface = 0x0B0D12;
constexpr std::uint32_t kElevated = 0x141824;
constexpr std::uint32_t kDivider = 0x272D3A;
constexpr std::uint32_t kPrimary = 0xF4F6FA;
constexpr std::uint32_t kSecondary = 0xA2ABBA;
constexpr std::uint32_t kCyan = 0x63DDE4;
constexpr std::uint32_t kViolet = 0xA78BFA;
constexpr std::uint32_t kGreen = 0x50D890;
constexpr std::uint32_t kAmber = 0xFFB454;
constexpr std::uint32_t kRed = 0xFF6174;

constexpr int kPanelWidth = 410;
constexpr int kPanelHeight = 502;
constexpr int kSafeInset = 28;
constexpr int kSafeRight = kPanelWidth - kSafeInset;
constexpr int kSafeBottom = kPanelHeight - kSafeInset;
constexpr int kSafeContentWidth = kPanelWidth - (2 * kSafeInset);

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                std::uint32_t color) {
    auto *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

void set_state(lv_obj_t *target, const char *text, std::uint32_t color) {
    if (!target) return;
    lv_label_set_text(target, text);
    lv_obj_set_style_text_color(target, lv_color_hex(color), 0);
}

lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int width, int height,
                      const char *text, std::uint32_t background,
                      std::uint32_t foreground, lv_event_cb_t callback,
                      void *user_data) {
    auto *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(background == kCyan ? kViolet : kElevated),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    auto *button_label = label(button, text, &lv_font_montserrat_18, foreground);
    lv_obj_center(button_label);
    return button;
}

void add_header(lv_obj_t *parent, const char *title, lv_event_cb_t back_callback,
                void *user_data) {
    make_button(parent, kSafeInset, kSafeInset, 56, 56, "<", kSurface, kPrimary,
                back_callback, user_data);
    auto *heading = label(parent, title, &lv_font_montserrat_26, kPrimary);
    lv_obj_set_pos(heading, 100, 39);
    lv_obj_set_width(heading, kSafeRight - 100);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_DOTS);
}

lv_obj_t *make_route_card(lv_obj_t *parent, int y, int height) {
    auto *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, kSafeContentWidth, height);
    lv_obj_set_pos(obj, kSafeInset, y);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kSurface), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(kDivider), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, 16, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t *make_diagnostic_card(lv_obj_t *parent, int height, const char *title,
                               lv_obj_t **state, lv_obj_t **detail) {
    auto *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, kSafeContentWidth - 8, height);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kSurface), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(kDivider), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, 14, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    auto *heading = label(obj, title, &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(heading, 0, 0);
    *state = label(obj, "WAIT", &lv_font_montserrat_14, kAmber);
    lv_obj_align(*state, LV_ALIGN_TOP_RIGHT, 0, 0);
    *detail = label(obj, "Awaiting first sample", &lv_font_montserrat_16, kPrimary);
    lv_obj_set_pos(*detail, 0, 28);
    lv_obj_set_width(*detail, kSafeContentWidth - 44);
    lv_label_set_long_mode(*detail, LV_LABEL_LONG_MODE_WRAP);
    return obj;
}
}  // namespace

nightglass::core::Status Shell::start() {
    if (screen_) return nightglass::core::Status::Ok();

    screen_ = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen_);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(kVoid), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    content_host_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(content_host_);
    lv_obj_set_size(content_host_, kPanelWidth, kPanelHeight);
    lv_obj_set_pos(content_host_, 0, 0);
    lv_obj_remove_flag(content_host_, LV_OBJ_FLAG_SCROLLABLE);

    // Reserved system-owned layer. Future modals render here without replacing
    // the persistent screen or allowing applications to cover system chrome.
    overlay_layer_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(overlay_layer_);
    lv_obj_set_size(overlay_layer_, kPanelWidth, kPanelHeight);
    lv_obj_set_pos(overlay_layer_, 0, 0);
    lv_obj_add_flag(overlay_layer_, LV_OBJ_FLAG_HIDDEN);

    navigation_ = {};
    render_route();
    for (auto *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER) {
            touch_input_ = input;
            lv_indev_add_event_cb(touch_input_, input_callback, LV_EVENT_PRESSED, this);
            break;
        }
    }
    lv_screen_load(screen_);
    return nightglass::core::Status::Ok();
}

void Shell::stop() {
    configure_refresh_timer(0);
    if (touch_input_) {
        lv_indev_remove_event_cb_with_user_data(touch_input_, input_callback, this);
        touch_input_ = nullptr;
    }
    if (screen_) {
        lv_obj_delete(screen_);
    }
    screen_ = nullptr;
    content_host_ = nullptr;
    overlay_layer_ = nullptr;
    clear_route_objects();
    navigation_ = {};
}

void Shell::timer_callback(lv_timer_t *timer) {
    static_cast<Shell *>(lv_timer_get_user_data(timer))->refresh_active_route();
}

void Shell::input_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    const auto before = nightglass::services::power_service().snapshot();
    nightglass::services::power_service().note_activity(nightglass::core::WakeReason::touch);
    if (before.state == nightglass::core::PowerState::screen_blank && self->touch_input_) {
        // The first contact on a black screen is wake-only. LVGL sends indev
        // events before object events, so this prevents click-through and then
        // ignores the remainder of the same physical touch until release.
        lv_indev_stop_processing(self->touch_input_);
        lv_indev_wait_release(self->touch_input_);
    }
}

void Shell::back_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::back);
}

void Shell::launcher_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_launcher);
}

void Shell::diagnostics_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_diagnostics);
}

void Shell::about_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_about);
}

void Shell::navigate(nightglass::core::NavigationAction action) {
    const auto next = nightglass::core::reduce_navigation(navigation_, action);
    if (next == navigation_) return;
    navigation_ = next;
    render_route();
}

void Shell::render_route() {
    configure_refresh_timer(0);
    clear_route_objects();
    lv_obj_clean(content_host_);

    switch (navigation_.route) {
        case nightglass::core::Route::home:
            render_home();
            break;
        case nightglass::core::Route::launcher:
            render_launcher();
            break;
        case nightglass::core::Route::diagnostics:
            render_diagnostics();
            break;
        case nightglass::core::Route::about:
            render_about();
            break;
    }
}

void Shell::render_home() {
    auto *brand = label(content_host_, "NIGHTGLASS", &lv_font_montserrat_16, kCyan);
    lv_obj_set_pos(brand, kSafeInset, 36);

    home_battery_ = label(content_host_, "BATTERY --", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(home_battery_, 252, 36);
    lv_obj_set_width(home_battery_, kSafeRight - 252);
    lv_obj_set_style_text_align(home_battery_, LV_TEXT_ALIGN_RIGHT, 0);

    home_time_ = label(content_host_, "--:--", &lv_font_montserrat_48, kPrimary);
    lv_obj_set_pos(home_time_, kSafeInset, 100);
    home_time_state_ = label(content_host_, "TIME UNAVAILABLE", &lv_font_montserrat_14, kRed);
    lv_obj_set_pos(home_time_state_, kSafeInset, 166);
    home_date_ = label(content_host_, "DATE UNAVAILABLE", &lv_font_montserrat_20, kSecondary);
    lv_obj_set_pos(home_date_, kSafeInset, 194);

    auto *status_card = make_route_card(content_host_, 246, 116);
    auto *glance = label(status_card, "AT A GLANCE", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(glance, 0, 0);
    home_motion_ = label(status_card, "MOTION · UNAVAILABLE", &lv_font_montserrat_20, kPrimary);
    lv_obj_set_pos(home_motion_, 0, 29);
    home_battery_detail_ = label(status_card, "Battery data unavailable", &lv_font_montserrat_14,
                                 kSecondary);
    lv_obj_set_pos(home_battery_detail_, 0, 67);
    lv_obj_set_width(home_battery_detail_, kSafeContentWidth - 32);
    lv_label_set_long_mode(home_battery_detail_, LV_LABEL_LONG_MODE_DOTS);

    make_button(content_host_, kSafeInset, 390, kSafeContentWidth, 70, "APPS", kCyan, kVoid,
                launcher_callback, this);

    configure_refresh_timer(1000);
    refresh_home();
}

void Shell::render_launcher() {
    add_header(content_host_, "APPS", back_callback, this);

    make_button(content_host_, kSafeInset, 112, kSafeContentWidth, 76, "DIAGNOSTICS",
                kSurface, kPrimary, diagnostics_callback, this);
    auto *diagnostics_hint = label(content_host_, "Live board telemetry", &lv_font_montserrat_14,
                                   kSecondary);
    lv_obj_set_pos(diagnostics_hint, 50, 168);

    make_button(content_host_, kSafeInset, 212, kSafeContentWidth, 76, "ABOUT", kSurface,
                kPrimary, about_callback, this);
    auto *about_hint = label(content_host_, "System and hardware identity",
                             &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(about_hint, 50, 268);

    auto *note = label(content_host_, "Only installed features are listed.",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, kSafeInset, 424);
    lv_obj_set_width(note, kSafeContentWidth);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
}

void Shell::render_diagnostics() {
    add_header(content_host_, "DIAGNOSTICS", back_callback, this);

    auto *scroller = lv_obj_create(content_host_);
    lv_obj_set_size(scroller, kSafeContentWidth, kSafeBottom - 100);
    lv_obj_set_pos(scroller, kSafeInset, 96);
    lv_obj_set_style_bg_opa(scroller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroller, 0, 0);
    lv_obj_set_style_pad_all(scroller, 0, 0);
    lv_obj_set_style_pad_row(scroller, 10, 0);
    lv_obj_set_scroll_dir(scroller, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroller, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(scroller, lv_color_hex(kDivider), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroller, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(scroller, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroller, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    make_diagnostic_card(scroller, 106, "RTC · PCF85063", &diagnostics_rtc_state_,
                         &diagnostics_rtc_detail_);
    make_diagnostic_card(scroller, 126, "POWER · AXP2101", &diagnostics_battery_state_,
                         &diagnostics_battery_detail_);
    make_diagnostic_card(scroller, 152, "MOTION · QMI8658", &diagnostics_motion_state_,
                         &diagnostics_motion_detail_);
    make_diagnostic_card(scroller, 116, "HAPTIC · GPIO18", &diagnostics_haptic_state_,
                         &diagnostics_haptic_detail_);

    // High-rate board refresh belongs exclusively to this active route.
    configure_refresh_timer(100);
    refresh_diagnostics();
}

void Shell::render_about() {
    add_header(content_host_, "ABOUT", back_callback, this);

    auto *identity = make_route_card(content_host_, 112, 142);
    auto *name = label(identity, "Nightglass", &lv_font_montserrat_32, kPrimary);
    lv_obj_set_pos(name, 0, 0);
    auto *kind = label(identity, "Native watch system", &lv_font_montserrat_16, kCyan);
    lv_obj_set_pos(kind, 0, 44);
    auto *version = label(identity, "ESP-IDF 5.5.5 · LVGL 9.5", &lv_font_montserrat_14,
                          kSecondary);
    lv_obj_set_pos(version, 0, 78);

    auto *hardware = make_route_card(content_host_, 270, 154);
    auto *heading = label(hardware, "HARDWARE", &lv_font_montserrat_14, kViolet);
    lv_obj_set_pos(heading, 0, 0);
    auto *detail = label(hardware,
                         "ESP32-S3 · 8 MB PSRAM\n32 MB flash · 410 x 502 AMOLED\n"
                         "Waveshare Touch AMOLED 2.06",
                         &lv_font_montserrat_16, kPrimary);
    lv_obj_set_pos(detail, 0, 29);
    lv_obj_set_width(detail, kSafeContentWidth - 32);
    lv_obj_set_style_text_line_space(detail, 8, 0);
}

void Shell::configure_refresh_timer(std::uint32_t period_ms) {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (period_ms > 0) {
        timer_ = lv_timer_create(timer_callback, period_ms, this);
    }
}

void Shell::refresh_active_route() {
    switch (navigation_.route) {
        case nightglass::core::Route::home:
            refresh_home();
            break;
        case nightglass::core::Route::diagnostics:
            refresh_diagnostics();
            break;
        case nightglass::core::Route::launcher:
        case nightglass::core::Route::about:
            break;
    }
}

void Shell::refresh_home() {
    if (!home_time_) return;
    const auto snapshot = nightglass::services::hardware_service().snapshot();
    const auto now = esp_timer_get_time();
    char buffer[96]{};

    const auto &rtc = snapshot.rtc;
    const auto rtc_age = now - rtc.sampled_at_us;
    if (rtc.present && rtc.valid && rtc_age <= 5'000'000) {
        std::snprintf(buffer, sizeof(buffer), "%02u:%02u", rtc.hour, rtc.minute);
        lv_label_set_text(home_time_, buffer);
        std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u", rtc.year, rtc.month, rtc.day);
        lv_label_set_text(home_date_, buffer);
        set_state(home_time_state_, rtc_age > 2'000'000 ? "TIME STALE" : "RTC · LIVE",
                  rtc_age > 2'000'000 ? kAmber : kGreen);
    } else {
        lv_label_set_text(home_time_, "--:--");
        lv_label_set_text(home_date_, "DATE UNAVAILABLE");
        set_state(home_time_state_, "TIME UNAVAILABLE", kRed);
    }

    const auto &battery = snapshot.battery;
    const auto battery_age = now - battery.sampled_at_us;
    if (battery.pmic_present && battery.battery_present && battery.percent_valid &&
        battery_age <= 10'000'000) {
        std::snprintf(buffer, sizeof(buffer), "%s%u%%", battery.charging ? "CHG " : "",
                      battery.percent);
        lv_label_set_text(home_battery_, buffer);
        if (battery.voltage_valid) {
            std::snprintf(buffer, sizeof(buffer), "%u.%02u V · %s",
                          battery.voltage_mv / 1000, (battery.voltage_mv % 1000) / 10,
                          battery.charging ? "Charging" : battery.discharging ? "Battery"
                                                                             : "State partial");
            lv_label_set_text(home_battery_detail_, buffer);
        } else {
            lv_label_set_text(home_battery_detail_, battery.charging ? "Charging · voltage unavailable"
                                                                     : "Voltage unavailable");
        }
        lv_obj_set_style_text_color(home_battery_,
                                    lv_color_hex(battery.percent <= 10 ? kRed
                                                 : battery.percent <= 20 ? kAmber
                                                                         : kSecondary),
                                    0);
    } else if (battery.pmic_present && !battery.battery_present) {
        set_state(home_battery_, "NO BATTERY", kAmber);
        lv_label_set_text(home_battery_detail_, "Battery not detected");
    } else {
        set_state(home_battery_, "BATTERY --", kRed);
        lv_label_set_text(home_battery_detail_, "Battery data unavailable");
    }

    const auto &motion = snapshot.motion;
    const auto motion_age = now - motion.sampled_at_us;
    if (motion.present && motion.valid && motion_age <= 2'000'000) {
        if (!motion.gyro_calibrated) {
            set_state(home_motion_, "MOTION · CALIBRATING", kAmber);
        } else {
            set_state(home_motion_, motion.moving ? "MOTION · MOVING" : "MOTION · STILL",
                      motion_age > 500'000 ? kAmber : motion.moving ? kCyan : kPrimary);
        }
    } else {
        set_state(home_motion_, "MOTION · UNAVAILABLE", kRed);
    }
}

void Shell::refresh_diagnostics() {
    if (!diagnostics_rtc_state_) return;
    const auto snapshot = nightglass::services::hardware_service().snapshot();
    const auto now = esp_timer_get_time();
    char buffer[192]{};

    const auto &rtc = snapshot.rtc;
    const auto rtc_age = now - rtc.sampled_at_us;
    if (rtc.present && rtc.valid && rtc_age <= 5'000'000) {
        set_state(diagnostics_rtc_state_, rtc_age > 2'000'000 ? "STALE" : "LIVE",
                  rtc_age > 2'000'000 ? kAmber : kGreen);
        std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u\n%04u-%02u-%02u",
                      rtc.hour, rtc.minute, rtc.second, rtc.year, rtc.month, rtc.day);
        lv_label_set_text(diagnostics_rtc_detail_, buffer);
    } else {
        set_state(diagnostics_rtc_state_, "NO DATA", kRed);
        lv_label_set_text(diagnostics_rtc_detail_, rtc.present ? "RTC value invalid or stale"
                                                               : "RTC read failed");
    }

    const auto &battery = snapshot.battery;
    const auto battery_age = now - battery.sampled_at_us;
    if (!battery.pmic_present || battery_age > 10'000'000) {
        set_state(diagnostics_battery_state_, "NO DATA", kRed);
        lv_label_set_text(diagnostics_battery_detail_, "AXP2101 telemetry unavailable");
    } else if (!battery.battery_present) {
        set_state(diagnostics_battery_state_, "NO BATTERY", kAmber);
        lv_label_set_text(diagnostics_battery_detail_, "PMIC live · battery not detected");
    } else {
        set_state(diagnostics_battery_state_, battery_age > 5'000'000 ? "STALE" : "LIVE",
                  battery_age > 5'000'000 ? kAmber : kGreen);
        const char *charge = battery.charging ? "Charging"
                             : battery.discharging ? "Discharging"
                                                  : "Charge state partial";
        if (battery.percent_valid && battery.voltage_valid) {
            std::snprintf(buffer, sizeof(buffer), "%u%% · %u.%02u V\n%s", battery.percent,
                          battery.voltage_mv / 1000, (battery.voltage_mv % 1000) / 10, charge);
        } else {
            std::snprintf(buffer, sizeof(buffer), "Battery detected\n%s · metrics partial", charge);
        }
        lv_label_set_text(diagnostics_battery_detail_, buffer);
    }

    const auto &motion = snapshot.motion;
    const auto motion_age = now - motion.sampled_at_us;
    if (!motion.present || !motion.valid || motion_age > 2'000'000) {
        set_state(diagnostics_motion_state_, "NO DATA", kRed);
        lv_label_set_text(diagnostics_motion_detail_, "QMI8658 sample unavailable");
    } else if (!motion.gyro_calibrated) {
        set_state(diagnostics_motion_state_, "CALIBRATING", kAmber);
        const unsigned progress = motion.gyro_calibration_required == 0
                                      ? 0
                                      : 100U * motion.gyro_calibration_samples /
                                            motion.gyro_calibration_required;
        std::snprintf(buffer, sizeof(buffer),
                      "Keep watch stationary · %u%%\nGyro zero %u / %u samples\nRestarts %lu",
                      progress, motion.gyro_calibration_samples,
                      motion.gyro_calibration_required,
                      static_cast<unsigned long>(motion.gyro_calibration_restarts));
        lv_label_set_text(diagnostics_motion_detail_, buffer);
    } else {
        set_state(diagnostics_motion_state_, motion_age > 500'000 ? "STALE" : "LIVE",
                  motion_age > 500'000 ? kAmber : kGreen);
        std::snprintf(buffer, sizeof(buffer),
                      "%s · bias %+.1f %+.1f %+.1f\nACC %+.2f  %+.2f  %+.2f g\nGYR %+.1f  %+.1f  %+.1f d/s",
                      motion.moving ? "Moving" : "Still", motion.gyro_bias_x_dps,
                      motion.gyro_bias_y_dps, motion.gyro_bias_z_dps, motion.accel_x_g,
                      motion.accel_y_g, motion.accel_z_g, motion.gyro_x_dps,
                      motion.gyro_y_dps, motion.gyro_z_dps);
        lv_label_set_text(diagnostics_motion_detail_, buffer);
    }

    const auto &haptic = snapshot.haptic;
    if (!haptic.actuator_present) {
        set_state(diagnostics_haptic_state_, "DEFERRED", kAmber);
        if (haptic.supply_state_known && !haptic.supply_enabled) {
            lv_label_set_text(diagnostics_haptic_detail_,
                              "No fitted actuator detected\nALDO3 verified off");
        } else {
            lv_label_set_text(diagnostics_haptic_detail_,
                              "No fitted actuator detected\nALDO3 state unverified");
        }
    } else if (haptic.ready) {
        set_state(diagnostics_haptic_state_, "READY", kGreen);
        lv_label_set_text(diagnostics_haptic_detail_, "Actuator service available");
    } else {
        set_state(diagnostics_haptic_state_, "UNAVAILABLE", kRed);
        lv_label_set_text(diagnostics_haptic_detail_, "Actuator present · service unavailable");
    }
}

void Shell::clear_route_objects() {
    home_time_ = nullptr;
    home_time_state_ = nullptr;
    home_date_ = nullptr;
    home_battery_ = nullptr;
    home_battery_detail_ = nullptr;
    home_motion_ = nullptr;
    diagnostics_rtc_state_ = nullptr;
    diagnostics_rtc_detail_ = nullptr;
    diagnostics_battery_state_ = nullptr;
    diagnostics_battery_detail_ = nullptr;
    diagnostics_motion_state_ = nullptr;
    diagnostics_motion_detail_ = nullptr;
    diagnostics_haptic_state_ = nullptr;
    diagnostics_haptic_detail_ = nullptr;
}

Shell &shell() { return instance; }

}  // namespace nightglass::ui

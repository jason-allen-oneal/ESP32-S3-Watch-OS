#include "morrow/ui/shell.hpp"

#include <cstdio>
#include <cstdint>

#include "esp_timer.h"
#include "lvgl.h"
#include "morrow/services/hardware.hpp"

namespace morrow::ui {

namespace {
Shell instance;

constexpr std::uint32_t kVoid = 0x000000;
constexpr std::uint32_t kSurface = 0x0B0D12;
constexpr std::uint32_t kDivider = 0x272D3A;
constexpr std::uint32_t kPrimary = 0xF4F6FA;
constexpr std::uint32_t kSecondary = 0xA2ABBA;
constexpr std::uint32_t kCyan = 0x63DDE4;
constexpr std::uint32_t kGreen = 0x50D890;
constexpr std::uint32_t kAmber = 0xFFB454;
constexpr std::uint32_t kRed = 0xFF5C70;

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                std::uint32_t color) {
    auto *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t *card(lv_obj_t *parent, int y, int height) {
    auto *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, 370, height);
    lv_obj_set_pos(obj, 20, y);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kSurface), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(kDivider), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

void set_state(lv_obj_t *target, const char *text, std::uint32_t color) {
    lv_label_set_text(target, text);
    lv_obj_set_style_text_color(target, lv_color_hex(color), 0);
}
}  // namespace

morrow::core::Status Shell::start() {
    if (screen_) return morrow::core::Status::Ok();

    auto *screen = lv_obj_create(nullptr);
    screen_ = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kVoid), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    auto *title = label(screen, "HARDWARE", &lv_font_montserrat_26, kPrimary);
    lv_obj_set_pos(title, 20, 20);
    auto *subtitle = label(screen, "LIVE DIAGNOSTICS", &lv_font_montserrat_14, kCyan);
    lv_obj_align(subtitle, LV_ALIGN_TOP_RIGHT, -20, 27);

    auto *rtc = card(screen, 72, 76);
    auto *rtc_title = label(rtc, "RTC", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(rtc_title, 0, -2);
    rtc_status_ = label(rtc, "WAIT", &lv_font_montserrat_14, kAmber);
    lv_obj_align(rtc_status_, LV_ALIGN_TOP_RIGHT, 0, -2);
    rtc_time_ = label(rtc, "--:--:--", &lv_font_montserrat_26, kPrimary);
    lv_obj_set_pos(rtc_time_, 0, 18);
    rtc_detail_ = label(rtc, "PCF85063 · awaiting first read", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(rtc_detail_, 152, 24);
    lv_obj_set_width(rtc_detail_, 192);
    lv_label_set_long_mode(rtc_detail_, LV_LABEL_LONG_MODE_DOTS);

    auto *battery = card(screen, 156, 110);
    auto *battery_title = label(battery, "BATTERY", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(battery_title, 0, -2);
    battery_status_ = label(battery, "WAIT", &lv_font_montserrat_14, kAmber);
    lv_obj_align(battery_status_, LV_ALIGN_TOP_RIGHT, 0, -2);
    battery_percent_ = label(battery, "--%", &lv_font_montserrat_32, kPrimary);
    lv_obj_set_pos(battery_percent_, 0, 20);
    battery_voltage_ = label(battery, "-.-- V", &lv_font_montserrat_20, kPrimary);
    lv_obj_align(battery_voltage_, LV_ALIGN_TOP_RIGHT, 0, 28);
    battery_bar_ = lv_bar_create(battery);
    lv_obj_set_size(battery_bar_, 344, 7);
    lv_obj_set_pos(battery_bar_, 0, 58);
    lv_bar_set_range(battery_bar_, 0, 100);
    lv_bar_set_value(battery_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(battery_bar_, lv_color_hex(kDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_bar_, lv_color_hex(kCyan), LV_PART_INDICATOR);
    battery_detail_ = label(battery, "AXP2101 · awaiting first read", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(battery_detail_, 0, 72);

    auto *motion = card(screen, 274, 120);
    auto *motion_title = label(motion, "MOTION", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(motion_title, 0, -2);
    motion_status_ = label(motion, "WAIT", &lv_font_montserrat_14, kAmber);
    lv_obj_align(motion_status_, LV_ALIGN_TOP_RIGHT, 0, -2);
    motion_state_ = label(motion, "Unknown", &lv_font_montserrat_20, kPrimary);
    lv_obj_set_pos(motion_state_, 0, 20);
    motion_accel_ = label(motion, "ACC g  X --.--  Y --.--  Z --.--", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(motion_accel_, 0, 53);
    motion_gyro_ = label(motion, "GYR d/s X --.-   Y --.-   Z --.-", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(motion_gyro_, 0, 77);

    auto *button = lv_button_create(screen);
    haptic_button_ = button;
    lv_obj_set_size(button, 370, 64);
    lv_obj_set_pos(button, 20, 410);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kCyan), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(button, haptic_callback, LV_EVENT_CLICKED, this);
    haptic_primary_ = label(button, "TEST HAPTIC", &lv_font_montserrat_18, kVoid);
    lv_obj_set_pos(haptic_primary_, 12, 7);
    auto *haptic_secondary = label(button, "SHORT PULSE · 120 MS", &lv_font_montserrat_14, kVoid);
    lv_obj_set_pos(haptic_secondary, 12, 32);
    haptic_result_ = label(screen, "Physical response unverified", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(haptic_result_, 20, 481);

    lv_screen_load(screen);
    timer_ = lv_timer_create(timer_callback, 100, this);
    refresh();
    return morrow::core::Status::Ok();
}

void Shell::stop() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (screen_) {
        lv_obj_delete(screen_);
        screen_ = nullptr;
    }
    rtc_time_ = rtc_status_ = rtc_detail_ = nullptr;
    battery_percent_ = battery_voltage_ = battery_status_ = battery_detail_ = nullptr;
    battery_bar_ = nullptr;
    motion_state_ = motion_status_ = motion_accel_ = motion_gyro_ = nullptr;
    haptic_button_ = haptic_primary_ = haptic_result_ = nullptr;
    haptic_waiting_ = false;
}

void Shell::timer_callback(lv_timer_t *timer) {
    static_cast<Shell *>(lv_timer_get_user_data(timer))->refresh();
}

void Shell::haptic_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!self->haptic_button_ || lv_obj_has_state(self->haptic_button_, LV_STATE_DISABLED)) return;
    if (morrow::services::hardware_service().request_haptic(120)) {
        self->haptic_waiting_ = true;
        lv_obj_add_state(self->haptic_button_, LV_STATE_DISABLED);
        lv_label_set_text(self->haptic_primary_, "PULSE QUEUED");
        set_state(self->haptic_result_, "Waiting for hardware acceptance", kAmber);
    } else {
        set_state(self->haptic_result_, "Pulse unavailable or rate limited", kRed);
    }
}

void Shell::refresh() {
    const auto snapshot = morrow::services::hardware_service().snapshot();
    const auto now = esp_timer_get_time();
    char buffer[96]{};

    const auto &rtc = snapshot.rtc;
    if (rtc.present) {
        std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u", rtc.hour, rtc.minute, rtc.second);
        lv_label_set_text(rtc_time_, buffer);
        if (rtc.valid && now - rtc.sampled_at_us > 5'000'000) {
            lv_label_set_text(rtc_time_, "--:--:--");
            set_state(rtc_status_, "NO DATA", kRed);
            lv_label_set_text(rtc_detail_, "RTC updates stopped");
        } else if (rtc.valid) {
            set_state(rtc_status_, now - rtc.sampled_at_us > 2'000'000 ? "STALE" : "LIVE",
                      now - rtc.sampled_at_us > 2'000'000 ? kAmber : kGreen);
            std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u · PCF85063",
                          rtc.year, rtc.month, rtc.day);
            lv_label_set_text(rtc_detail_, buffer);
        } else {
            set_state(rtc_status_, "CHECK", kAmber);
            lv_label_set_text(rtc_detail_, "RTC oscillator or date invalid");
        }
    } else {
        lv_label_set_text(rtc_time_, "--:--:--");
        set_state(rtc_status_, "NO DATA", kRed);
        lv_label_set_text(rtc_detail_, "PCF85063 read failed");
    }

    const auto &battery = snapshot.battery;
    if (battery.pmic_present && now - battery.sampled_at_us > 10'000'000) {
        lv_label_set_text(battery_percent_, "--%");
        lv_label_set_text(battery_voltage_, "-.-- V");
        set_state(battery_status_, "NO DATA", kRed);
        lv_label_set_text(battery_detail_, "PMIC updates stopped");
        lv_bar_set_value(battery_bar_, 0, LV_ANIM_OFF);
    } else if (!battery.pmic_present) {
        lv_label_set_text(battery_percent_, "--%");
        lv_label_set_text(battery_voltage_, "-.-- V");
        set_state(battery_status_, "NO DATA", kRed);
        lv_label_set_text(battery_detail_, "AXP2101 read failed");
        lv_bar_set_value(battery_bar_, 0, LV_ANIM_OFF);
    } else if (!battery.battery_present) {
        lv_label_set_text(battery_percent_, "--%");
        lv_label_set_text(battery_voltage_, "-.-- V");
        set_state(battery_status_, "NO BATTERY", kAmber);
        lv_label_set_text(battery_detail_, "Battery not detected");
        lv_bar_set_value(battery_bar_, 0, LV_ANIM_OFF);
    } else {
        if (battery.percent_valid) {
            std::snprintf(buffer, sizeof(buffer), "%u%%", battery.percent);
            lv_label_set_text(battery_percent_, buffer);
            lv_bar_set_value(battery_bar_, battery.percent, LV_ANIM_OFF);
            lv_obj_set_style_text_color(battery_percent_, lv_color_hex(
                battery.percent <= 10 ? kRed : battery.percent <= 20 ? kAmber : kPrimary), 0);
        } else {
            lv_label_set_text(battery_percent_, "--%");
            lv_bar_set_value(battery_bar_, 0, LV_ANIM_OFF);
        }
        if (battery.voltage_valid) {
            std::snprintf(buffer, sizeof(buffer), "%u.%02u V", battery.voltage_mv / 1000,
                          (battery.voltage_mv % 1000) / 10);
            lv_label_set_text(battery_voltage_, buffer);
        } else {
            lv_label_set_text(battery_voltage_, "-.-- V");
        }
        if (now - battery.sampled_at_us > 5'000'000) {
            set_state(battery_status_, "STALE", kAmber);
            lv_label_set_text(battery_detail_, "Last PMIC read is stale");
        } else if (battery.charging) {
            set_state(battery_status_, "CHARGING", kAmber);
            lv_label_set_text(battery_detail_, "Charging · PMIC state");
        } else if (battery.discharging) {
            set_state(battery_status_, "BATTERY", kGreen);
            lv_label_set_text(battery_detail_, "Discharging");
        } else {
            set_state(battery_status_, "PARTIAL", kAmber);
            lv_label_set_text(battery_detail_, "Charge state unavailable");
        }
    }

    const auto &motion = snapshot.motion;
    if (!motion.present || !motion.valid || now - motion.sampled_at_us > 2'000'000) {
        set_state(motion_status_, "NO DATA", kRed);
        lv_label_set_text(motion_state_, "Unknown");
        lv_label_set_text(motion_accel_, "ACC g  X --.--  Y --.--  Z --.--");
        lv_label_set_text(motion_gyro_, "GYR d/s X --.-   Y --.-   Z --.-");
    } else {
        set_state(motion_status_, now - motion.sampled_at_us > 500'000 ? "STALE" : "LIVE",
                  now - motion.sampled_at_us > 500'000 ? kAmber : kGreen);
        lv_label_set_text(motion_state_, motion.moving ? "Moving" : "Still");
        std::snprintf(buffer, sizeof(buffer), "ACC g  X %+.2f  Y %+.2f  Z %+.2f",
                      motion.accel_x_g, motion.accel_y_g, motion.accel_z_g);
        lv_label_set_text(motion_accel_, buffer);
        std::snprintf(buffer, sizeof(buffer), "GYR d/s X %+.1f  Y %+.1f  Z %+.1f",
                      motion.gyro_x_dps, motion.gyro_y_dps, motion.gyro_z_dps);
        lv_label_set_text(motion_gyro_, buffer);
    }

    if (!snapshot.haptic.ready) {
        lv_obj_add_state(haptic_button_, LV_STATE_DISABLED);
        set_state(haptic_result_, "Haptic service unavailable", kRed);
    } else if (snapshot.haptic.failed_pulses > observed_failed_pulses_) {
        observed_failed_pulses_ = snapshot.haptic.failed_pulses;
        haptic_waiting_ = false;
        lv_obj_remove_state(haptic_button_, LV_STATE_DISABLED);
        lv_label_set_text(haptic_primary_, "TEST HAPTIC");
        set_state(haptic_result_, "Pulse activation failed", kRed);
    } else if (snapshot.haptic.pulse_active) {
        lv_label_set_text(haptic_primary_, "PULSE ACTIVE");
    } else if (snapshot.haptic.accepted_pulses > observed_accepted_pulses_) {
        observed_accepted_pulses_ = snapshot.haptic.accepted_pulses;
        haptic_waiting_ = false;
        lv_obj_remove_state(haptic_button_, LV_STATE_DISABLED);
        lv_label_set_text(haptic_primary_, "TEST HAPTIC");
        set_state(haptic_result_, "Pulse sent · confirm response by feel", kAmber);
    } else if (!haptic_waiting_) {
        lv_obj_remove_state(haptic_button_, LV_STATE_DISABLED);
    }
}

Shell &shell() { return instance; }

}  // namespace morrow::ui

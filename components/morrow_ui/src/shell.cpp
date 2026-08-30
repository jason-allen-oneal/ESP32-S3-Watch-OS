#include "morrow/ui/shell.hpp"

#include <cstdint>

#include "lvgl.h"

namespace morrow::ui {

namespace {
Shell instance;
lv_obj_t *touch_status = nullptr;

constexpr std::uint32_t kVoid = 0x000000;
constexpr std::uint32_t kSurface = 0x0B0D12;
constexpr std::uint32_t kDivider = 0x272D3A;
constexpr std::uint32_t kPrimary = 0xF4F6FA;
constexpr std::uint32_t kSecondary = 0xA2ABBA;
constexpr std::uint32_t kCyan = 0x63DDE4;
constexpr std::uint32_t kGreen = 0x50D890;
constexpr std::uint32_t kAmber = 0xFFB454;

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                std::uint32_t color) {
    auto *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t *status_row(lv_obj_t *parent, const char *name, const char *status,
                     std::uint32_t color, int y) {
    auto *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 370, 50);
    lv_obj_set_pos(row, 0, y);

    auto *name_label = label(row, name, &lv_font_montserrat_16, kPrimary);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 0, 0);

    auto *status_label = label(row, status, &lv_font_montserrat_14, color);
    lv_obj_align(status_label, LV_ALIGN_RIGHT_MID, 0, 0);
    return status_label;
}

void verify_touch(lv_event_t *) {
    if (touch_status) {
        lv_label_set_text(touch_status, "VERIFIED");
        lv_obj_set_style_text_color(touch_status, lv_color_hex(kGreen), 0);
    }
}
}  // namespace

morrow::core::Status Shell::start() {
    auto *screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kVoid), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    auto *brand = label(screen, "MORROW", &lv_font_montserrat_14, kCyan);
    lv_obj_set_pos(brand, 20, 18);

    auto *mode = label(screen, "FOUNDATION MODE", &lv_font_montserrat_14, kSecondary);
    lv_obj_align(mode, LV_ALIGN_TOP_RIGHT, -20, 18);

    auto *time = label(screen, "--:--", &lv_font_montserrat_48, kPrimary);
    lv_obj_set_pos(time, 20, 68);

    auto *time_state = label(screen, "RTC SERVICE PENDING", &lv_font_montserrat_14, kAmber);
    lv_obj_set_pos(time_state, 24, 122);

    auto *card = lv_obj_create(screen);
    lv_obj_set_size(card, 370, 208);
    lv_obj_set_pos(card, 20, 170);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kSurface), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kDivider), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    auto *title = label(card, "HARDWARE GATE", &lv_font_montserrat_18, kPrimary);
    lv_obj_set_pos(title, 0, 0);
    status_row(card, "Display", "ONLINE", kGreen, 38);
    touch_status = status_row(card, "Touch", "TAP TO VERIFY", kAmber, 82);
    status_row(card, "Recovery", "AVAILABLE", kGreen, 126);

    auto *button = lv_button_create(screen);
    lv_obj_set_size(button, 370, 64);
    lv_obj_set_pos(button, 20, 410);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kCyan), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(button, verify_touch, LV_EVENT_CLICKED, nullptr);

    auto *button_label = label(button, "VERIFY TOUCH", &lv_font_montserrat_18, kVoid);
    lv_obj_center(button_label);

    lv_screen_load(screen);
    return morrow::core::Status::Ok();
}

Shell &shell() { return instance; }

}  // namespace morrow::ui

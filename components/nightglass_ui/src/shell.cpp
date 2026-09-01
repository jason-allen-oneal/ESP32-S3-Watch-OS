#include "nightglass/ui/shell.hpp"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <span>

#include "esp_timer.h"
#include "lvgl.h"
#include "nightglass/services/clock.hpp"
#include "nightglass/services/activity.hpp"
#include "nightglass/services/activity_units.hpp"
#include "nightglass/services/audio.hpp"
#include "nightglass/services/connectivity.hpp"
#include "nightglass/services/network_weather.hpp"
#include "nightglass/services/weather_logic.hpp"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/power.hpp"
#include "nightglass/services/watchface.hpp"
#include "nightglass/services/update_transport.hpp"
#include "nightglass/ui/assets/revenant_grid_v2.hpp"

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

struct ChromePalette {
    std::uint32_t background;
    std::uint32_t surface;
    std::uint32_t elevated;
    std::uint32_t border;
    std::uint32_t primary;
    std::uint32_t secondary;
    std::uint32_t accent;
    std::uint32_t accent_alternate;
    std::uint32_t pressed;
};

constexpr ChromePalette kClassicChrome{
    kVoid, kSurface, kElevated, kDivider, kPrimary, kSecondary, kCyan, kViolet,
    kElevated,
};
constexpr ChromePalette kRevenantChrome{
    0x000000, 0x080B09, 0x111712, 0x355128, 0xF3F7F4, 0x9BA79F, 0xA8FF32,
    0x7ED321, 0x263C1B,
};

constexpr int kPanelWidth = 410;
constexpr int kPanelHeight = 502;
constexpr int kSafeInset = 28;
constexpr int kSafeRight = kPanelWidth - kSafeInset;
constexpr int kSafeBottom = kPanelHeight - kSafeInset;
constexpr int kSafeContentWidth = kPanelWidth - (2 * kSafeInset);

const ChromePalette &chrome_palette() {
    const auto &pack = nightglass::services::watchface_service().selected();
    if (nightglass::services::valid_face_pack(pack) &&
        pack.chrome.theme == nightglass::services::ChromeTheme::revenant) {
        return kRevenantChrome;
    }
    return kClassicChrome;
}

bool revenant_chrome() { return &chrome_palette() == &kRevenantChrome; }

std::uint32_t chrome_color(std::uint32_t color) {
    if (!revenant_chrome()) return color;
    if (color == kVoid) return kRevenantChrome.background;
    if (color == kSurface) return kRevenantChrome.surface;
    if (color == kElevated) return kRevenantChrome.elevated;
    if (color == kDivider) return kRevenantChrome.border;
    if (color == kPrimary) return kRevenantChrome.primary;
    if (color == kSecondary) return kRevenantChrome.secondary;
    if (color == kCyan) return kRevenantChrome.accent;
    if (color == kViolet) return kRevenantChrome.accent_alternate;
    return color;
}

template <typename T, std::size_t N>
T next_value(T current, const T (&values)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (values[i] == current) return values[(i + 1) % N];
    }
    return values[0];
}

void set_button_text(lv_obj_t *button, const char *text) {
    if (button && lv_obj_get_child_count(button) > 0) {
        lv_label_set_text(lv_obj_get_child(button, 0), text);
    }
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                std::uint32_t color) {
    auto *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(chrome_color(color)), 0);
    return obj;
}

void set_state(lv_obj_t *target, const char *text, std::uint32_t color) {
    if (!target) return;
    lv_label_set_text(target, text);
    lv_obj_set_style_text_color(target, lv_color_hex(color), 0);
}

lv_obj_t *weather_shape(lv_obj_t *parent, int x, int y, int width, int height,
                        std::uint32_t color, int radius = 0) {
    auto *shape = lv_obj_create(parent);
    lv_obj_remove_style_all(shape);
    lv_obj_set_pos(shape, x, y);
    lv_obj_set_size(shape, width, height);
    lv_obj_set_style_bg_color(shape, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(shape, radius, 0);
    lv_obj_remove_flag(shape, LV_OBJ_FLAG_CLICKABLE);
    return shape;
}

void draw_weather_icon(lv_obj_t *parent, nightglass::services::WeatherIcon icon,
                       std::uint32_t color) {
    if (!parent) return;
    lv_obj_clean(parent);
    constexpr std::uint32_t shadow = 0x101413;
    auto cloud = [parent, color]() {
        weather_shape(parent, 8, 13, 28, 10, color, 5);
        weather_shape(parent, 13, 8, 13, 13, color, 7);
        weather_shape(parent, 23, 10, 10, 10, color, 5);
    };
    using nightglass::services::WeatherIcon;
    switch (icon) {
        case WeatherIcon::clear_day:
            weather_shape(parent, 14, 4, 18, 18, color, 9);
            break;
        case WeatherIcon::clear_night:
            weather_shape(parent, 12, 3, 20, 20, color, 10);
            weather_shape(parent, 19, 0, 18, 18, shadow, 9);
            break;
        case WeatherIcon::partly_cloudy:
            weather_shape(parent, 22, 2, 13, 13, color, 7);
            cloud();
            break;
        case WeatherIcon::cloudy:
            cloud();
            break;
        case WeatherIcon::fog:
            weather_shape(parent, 7, 7, 30, 3, color, 2);
            weather_shape(parent, 11, 14, 26, 3, color, 2);
            weather_shape(parent, 7, 21, 30, 3, color, 2);
            break;
        case WeatherIcon::rain:
            cloud();
            weather_shape(parent, 12, 24, 3, 4, color, 1);
            weather_shape(parent, 22, 24, 3, 4, color, 1);
            weather_shape(parent, 32, 24, 3, 4, color, 1);
            break;
        case WeatherIcon::snow:
            cloud();
            weather_shape(parent, 12, 24, 4, 4, color, 2);
            weather_shape(parent, 22, 24, 4, 4, color, 2);
            weather_shape(parent, 32, 24, 4, 4, color, 2);
            break;
        case WeatherIcon::storm:
            cloud();
            weather_shape(parent, 19, 22, 5, 6, color, 1);
            weather_shape(parent, 24, 25, 5, 3, color, 1);
            break;
    }
}

lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int width, int height,
                      const char *text, std::uint32_t background,
                      std::uint32_t foreground, lv_event_cb_t callback,
                      void *user_data) {
    auto *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 14, 0);
    const auto &chrome = chrome_palette();
    lv_obj_set_style_bg_color(button, lv_color_hex(chrome_color(background)), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(chrome.border), 0);
    lv_obj_set_style_border_width(button, revenant_chrome() ? 1 : 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(
        button,
        lv_color_hex(revenant_chrome()
                         ? (background == kCyan ? chrome.accent_alternate : chrome.pressed)
                         : (background == kCyan ? kViolet : kElevated)),
        LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(chrome.accent), LV_STATE_PRESSED);
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
    if (revenant_chrome()) {
        auto *rail = lv_obj_create(parent);
        lv_obj_remove_style_all(rail);
        lv_obj_set_size(rail, kSafeContentWidth, 1);
        lv_obj_set_pos(rail, kSafeInset, 91);
        lv_obj_set_style_bg_color(rail, lv_color_hex(chrome_palette().accent), 0);
        lv_obj_set_style_bg_opa(rail, LV_OPA_70, 0);
    }
}

lv_obj_t *make_route_card(lv_obj_t *parent, int y, int height) {
    auto *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, kSafeContentWidth, height);
    lv_obj_set_pos(obj, kSafeInset, y);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(chrome_palette().surface), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(chrome_palette().border), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, 16, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t *make_scroller(lv_obj_t *parent) {
    auto *scroller = lv_obj_create(parent);
    lv_obj_set_size(scroller, kSafeContentWidth, kSafeBottom - 100);
    lv_obj_set_pos(scroller, kSafeInset, 96);
    lv_obj_set_style_bg_opa(scroller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroller, 0, 0);
    lv_obj_set_style_pad_all(scroller, 4, 0);
    lv_obj_set_scroll_dir(scroller, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroller, LV_SCROLLBAR_MODE_ACTIVE);
    if (revenant_chrome()) {
        lv_obj_set_style_bg_color(scroller, lv_color_hex(chrome_palette().border),
                                  LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(scroller, LV_OPA_70, LV_PART_SCROLLBAR);
    }
    return scroller;
}

void format_time(char *buffer, std::size_t size,
                 const nightglass::services::CivilTime &time, bool use_24_hour,
                 const char **period) {
    if (use_24_hour) {
        std::snprintf(buffer, size, "%02u:%02u", time.hour, time.minute);
        *period = "";
        return;
    }
    const auto hour = static_cast<unsigned>(time.hour % 12 == 0 ? 12 : time.hour % 12);
    std::snprintf(buffer, size, "%u:%02u", hour, time.minute);
    *period = time.hour < 12 ? "AM" : "PM";
}

void format_duration(char *buffer, std::size_t size, std::uint64_t total_ms,
                     bool tenths) {
    const auto total_seconds = total_ms / 1000;
    const auto hours = total_seconds / 3600;
    const auto minutes = (total_seconds % 3600) / 60;
    const auto seconds = total_seconds % 60;
    if (tenths) {
        std::snprintf(buffer, size, "%02llu:%02llu:%02llu.%llu",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds),
                      static_cast<unsigned long long>((total_ms / 100) % 10));
    } else {
        std::snprintf(buffer, size, "%02llu:%02llu:%02llu",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds));
    }
}

void format_media_time(char *buffer, std::size_t size, std::uint32_t milliseconds) {
    const auto seconds = milliseconds / 1000U;
    std::snprintf(buffer, size, "%lu:%02lu",
                  static_cast<unsigned long>(seconds / 60U),
                  static_cast<unsigned long>(seconds % 60U));
}

lv_obj_t *make_diagnostic_card(lv_obj_t *parent, int height, const char *title,
                               lv_obj_t **state, lv_obj_t **detail) {
    auto *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, kSafeContentWidth - 8, height);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(chrome_palette().surface), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(chrome_palette().border), 0);
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

const lv_font_t *face_font(nightglass::services::FaceTextStyle style) {
    using nightglass::services::FaceTextStyle;
    switch (style) {
        case FaceTextStyle::caption_14:
            return &lv_font_montserrat_14;
        case FaceTextStyle::body_16:
            return &lv_font_montserrat_16;
        case FaceTextStyle::value_20:
            return &lv_font_montserrat_20;
        case FaceTextStyle::time_48:
            return &lv_font_montserrat_48;
    }
    return &lv_font_montserrat_14;
}

std::uint32_t face_color(const nightglass::services::FacePalette &palette,
                         nightglass::services::FaceColorRole role) {
    using nightglass::services::FaceColorRole;
    switch (role) {
        case FaceColorRole::primary:
            return palette.primary;
        case FaceColorRole::secondary:
            return palette.secondary;
        case FaceColorRole::accent:
            return palette.accent;
        case FaceColorRole::accent_dim:
            return palette.accent_dim;
    }
    return palette.primary;
}

lv_text_align_t face_align(nightglass::services::FaceTextAlign align) {
    using nightglass::services::FaceTextAlign;
    switch (align) {
        case FaceTextAlign::left:
            return LV_TEXT_ALIGN_LEFT;
        case FaceTextAlign::center:
            return LV_TEXT_ALIGN_CENTER;
        case FaceTextAlign::right:
            return LV_TEXT_ALIGN_RIGHT;
    }
    return LV_TEXT_ALIGN_LEFT;
}

const lv_image_dsc_t *face_asset(nightglass::services::FaceAsset asset) {
    switch (asset) {
        case nightglass::services::FaceAsset::revenant_grid_v2:
            return &revenant_grid_v2_background;
        case nightglass::services::FaceAsset::none:
            return nullptr;
    }
    return nullptr;
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
    system_timer_ = lv_timer_create(system_timer_callback, 250, this);
    return nightglass::core::Status::Ok();
}

void Shell::stop() {
    configure_refresh_timer(0);
    if (system_timer_) {
        lv_timer_delete(system_timer_);
        system_timer_ = nullptr;
    }
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
    alert_card_ = nullptr;
    displayed_alert_kind_ = 0;
    displayed_alert_alarm_index_ = nightglass::services::kNoAlarmIndex;
    quick_settings_open_ = false;
    home_aod_active_ = false;
    clear_route_objects();
    navigation_ = {};
}

void Shell::timer_callback(lv_timer_t *timer) {
    static_cast<Shell *>(lv_timer_get_user_data(timer))->refresh_active_route();
}

void Shell::system_timer_callback(lv_timer_t *timer) {
    static_cast<Shell *>(lv_timer_get_user_data(timer))->refresh_system_overlay();
}

void Shell::input_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    const auto before = nightglass::services::power_service().snapshot();
    nightglass::services::power_service().note_activity(nightglass::core::WakeReason::touch);
    if ((before.state == nightglass::core::PowerState::dim ||
         before.state == nightglass::core::PowerState::ambient ||
         before.state == nightglass::core::PowerState::screen_blank ||
         before.wake_touch_pending) &&
        self->touch_input_) {
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

void Shell::settings_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_settings);
}

void Shell::power_settings_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_power_settings);
}

void Shell::clock_settings_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_clock_settings);
}

void Shell::watchface_settings_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_watchface_settings);
}

void Shell::watchface_next_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto &service = nightglass::services::watchface_service();
    const auto &selected = service.selected();
    const auto *packs = service.packs();
    const auto count = service.pack_count();
    bool changed = false;
    for (std::size_t index = 0; index < count; ++index) {
        if (packs[index].id == selected.id) {
            changed = service.select(packs[(index + 1) % count].id);
            break;
        }
    }
    if (changed) {
        // The pack owns both its home face and system chrome. Recreate the
        // active route and any visible system overlay from the new metadata.
        self->render_route();
        self->displayed_alert_kind_ = 0;
        self->refresh_system_overlay();
    } else {
        self->refresh_watchface_settings();
    }
}

void Shell::alarm_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_alarm);
}

void Shell::countdown_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_countdown);
}

void Shell::stopwatch_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_stopwatch);
}

void Shell::active_brightness_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::power_service().snapshot().settings;
    constexpr std::uint8_t values[]{20, 30, 50, 75, 100};
    settings.active_brightness = next_value(settings.active_brightness, values);
    if (settings.dim_brightness >= settings.active_brightness) settings.dim_brightness = 8;
    nightglass::services::power_service().update_settings(settings);
    self->refresh_settings_labels();
}

void Shell::dim_brightness_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::power_service().snapshot().settings;
    constexpr std::uint8_t values[]{0, 4, 8, 12, 20};
    settings.dim_brightness = next_value(settings.dim_brightness, values);
    if (settings.dim_brightness >= settings.active_brightness) settings.dim_brightness = 0;
    nightglass::services::power_service().update_settings(settings);
    self->refresh_settings_labels();
}

void Shell::dim_after_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::power_service().snapshot().settings;
    constexpr std::uint16_t values[]{10, 15, 30, 60, 120};
    settings.dim_after_seconds = next_value(settings.dim_after_seconds, values);
    if (settings.blank_after_seconds <= settings.dim_after_seconds) {
        settings.blank_after_seconds = settings.dim_after_seconds + 30;
    }
    nightglass::services::power_service().update_settings(settings);
    self->refresh_settings_labels();
}

void Shell::blank_after_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::power_service().snapshot().settings;
    constexpr std::uint16_t values[]{20, 30, 60, 120, 300};
    settings.blank_after_seconds = next_value(settings.blank_after_seconds, values);
    if (settings.blank_after_seconds <= settings.dim_after_seconds) {
        settings.blank_after_seconds = settings.dim_after_seconds + 15;
    }
    nightglass::services::power_service().update_settings(settings);
    self->refresh_settings_labels();
}

void Shell::sleep_after_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::power_service().snapshot().settings;
    constexpr std::uint16_t values[]{0, 5, 15, 30, 60};
    settings.sleep_after_blank_seconds = next_value(settings.sleep_after_blank_seconds, values);
    nightglass::services::power_service().update_settings(settings);
    self->refresh_settings_labels();
}

void Shell::time_format_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::clock_service().snapshot().settings;
    settings.use_24_hour = !settings.use_24_hour;
    if (!nightglass::services::clock_service().update_clock_settings(settings)) {
        lv_label_set_text(self->clock_preview_, "CLOCK QUEUE FULL");
    }
}

void Shell::utc_offset_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::clock_service().snapshot().settings;
    settings.utc_offset_minutes = static_cast<std::int16_t>(settings.utc_offset_minutes + 30);
    if (settings.utc_offset_minutes > 14 * 60) settings.utc_offset_minutes = -12 * 60;
    if (!nightglass::services::clock_service().update_clock_settings(settings)) {
        lv_label_set_text(self->clock_preview_, "CLOCK QUEUE FULL");
    }
}

void Shell::dst_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::clock_service().snapshot().settings;
    settings.daylight_saving = !settings.daylight_saving;
    if (!nightglass::services::clock_service().update_clock_settings(settings)) {
        lv_label_set_text(self->clock_preview_, "CLOCK QUEUE FULL");
    }
}

void Shell::alarm_hour_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().adjust_alarm(
            self->alarm_slot_index_, nightglass::services::AlarmAdjustment::hour_forward)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::alarm_hour_back_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().adjust_alarm(
            self->alarm_slot_index_, nightglass::services::AlarmAdjustment::hour_backward)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::alarm_minute_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().adjust_alarm(
            self->alarm_slot_index_, nightglass::services::AlarmAdjustment::minute_forward)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::alarm_minute_back_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().adjust_alarm(
            self->alarm_slot_index_, nightglass::services::AlarmAdjustment::minute_backward)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::alarm_enabled_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().adjust_alarm(
            self->alarm_slot_index_, nightglass::services::AlarmAdjustment::toggle_enabled)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::alarm_slot_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    self->alarm_slot_index_ = static_cast<std::uint8_t>(
        (self->alarm_slot_index_ + 1) % nightglass::services::kAlarmCapacity);
    self->refresh_alarm();
}

void Shell::alarm_repeat_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().adjust_alarm(
            self->alarm_slot_index_, nightglass::services::AlarmAdjustment::cycle_repeat)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::alarm_label_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().adjust_alarm(
            self->alarm_slot_index_, nightglass::services::AlarmAdjustment::cycle_label)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::alarm_snooze_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().snooze_alarm(10)) return;
    self->refresh_system_overlay();
}

void Shell::quiet_toggle_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto quiet = nightglass::services::clock_service().snapshot().quiet_hours;
    quiet.enabled = !quiet.enabled;
    if (!nightglass::services::clock_service().update_quiet_hours(quiet)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::quiet_start_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto quiet = nightglass::services::clock_service().snapshot().quiet_hours;
    quiet.start_minute = static_cast<std::uint16_t>((quiet.start_minute + 30) % (24 * 60));
    if (!nightglass::services::clock_service().update_quiet_hours(quiet)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::quiet_end_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto quiet = nightglass::services::clock_service().snapshot().quiet_hours;
    quiet.end_minute = static_cast<std::uint16_t>((quiet.end_minute + 30) % (24 * 60));
    if (!nightglass::services::clock_service().update_quiet_hours(quiet)) {
        set_state(self->alarm_state_, "QUEUE FULL", kRed);
    }
}

void Shell::quick_settings_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    self->quick_settings_open_ = true;
    self->render_route();
}

void Shell::quick_settings_back_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    self->quick_settings_open_ = false;
    self->render_route();
}

void Shell::quick_brightness_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::power_service().snapshot().settings;
    constexpr std::uint8_t values[]{20, 30, 50, 75, 100};
    settings.active_brightness = next_value(settings.active_brightness, values);
    if (settings.dim_brightness >= settings.active_brightness) settings.dim_brightness = 8;
    const auto status = nightglass::services::power_service().update_settings(settings);
    if (!status.is_ok()) {
        set_button_text(self->quick_brightness_, "BRIGHTNESS  SAVE FAILED");
        return;
    }
    self->refresh_quick_settings();
}

void Shell::quick_mute_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::audio_service().snapshot().settings;
    settings.muted = !settings.muted;
    const auto status = nightglass::services::audio_service().update_settings(settings);
    if (!status.is_ok()) {
        set_button_text(self->quick_mute_, "WATCH SOUND  SAVE FAILED");
        return;
    }
    self->refresh_quick_settings();
}

void Shell::quick_dnd_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::audio_service().snapshot().settings;
    settings.do_not_disturb = !settings.do_not_disturb;
    const auto status = nightglass::services::audio_service().update_settings(settings);
    if (!status.is_ok()) {
        set_button_text(self->quick_dnd_, "DND  SAVE FAILED");
        return;
    }
    self->refresh_quick_settings();
}

void Shell::quick_bluetooth_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::connectivity_service().snapshot().settings;
    settings.enabled = !settings.enabled;
    const auto status = nightglass::services::connectivity_service().update_settings(settings);
    if (!status.is_ok()) {
        set_button_text(self->quick_bluetooth_, "BLUETOOTH  SAVE FAILED");
        return;
    }
    self->refresh_quick_settings();
}

void Shell::phone_command_callback(lv_event_t *event) {
    const auto command = static_cast<nightglass::services::PhoneCommand>(
        reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event)));
    if (!nightglass::services::connectivity_service().send_phone(command) &&
        instance.connectivity_state_) {
        set_state(instance.connectivity_state_, "COMMAND FAILED", kRed);
    }
}

void Shell::call_command_callback(lv_event_t *event) {
    auto command = static_cast<nightglass::services::CallCommand>(
        reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event)));
    if (command == nightglass::services::CallCommand::mute &&
        nightglass::services::connectivity_service().snapshot().call.muted) {
        command = nightglass::services::CallCommand::unmute;
    }
    if (!nightglass::services::connectivity_service().send_call(command) &&
        instance.connectivity_state_) {
        set_state(instance.connectivity_state_, "COMMAND FAILED", kRed);
    }
}

void Shell::countdown_duration_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    const auto snapshot = nightglass::services::clock_service().snapshot();
    constexpr std::uint32_t values[]{60, 300, 600, 900, 1800, 3600};
    if (!nightglass::services::clock_service().set_timer_duration(
            next_value(snapshot.timer_configured_seconds, values))) {
        lv_label_set_text(self->countdown_time_, "QUEUE FULL");
    }
}

void Shell::countdown_toggle_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().toggle_timer()) {
        lv_label_set_text(self->countdown_time_, "QUEUE FULL");
    }
}

void Shell::countdown_reset_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().reset_timer()) {
        lv_label_set_text(self->countdown_time_, "QUEUE FULL");
    }
}

void Shell::stopwatch_toggle_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().toggle_stopwatch()) {
        lv_label_set_text(self->stopwatch_time_, "QUEUE FULL");
    }
}

void Shell::stopwatch_reset_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::clock_service().reset_stopwatch()) {
        lv_label_set_text(self->stopwatch_time_, "QUEUE FULL");
    }
}

void Shell::audio_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_audio);
}

void Shell::audio_play_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    const auto status = nightglass::services::audio_service().request_test_tone();
    if (status.is_ok()) {
        set_state(self->audio_state_, "QUEUED", kAmber);
        lv_label_set_text(self->audio_detail_, "Speaker test queued off the UI task");
    } else {
        set_state(self->audio_state_, "FAILED", kRed);
        lv_label_set_text(self->audio_detail_, status.detail);
    }
}

void Shell::audio_capture_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    const auto status = nightglass::services::audio_service().request_microphone_sample();
    if (!status.is_ok()) {
        set_state(self->audio_state_, "FAILED", kRed);
        lv_label_set_text(self->audio_detail_, status.detail);
        return;
    }
    set_state(self->audio_state_, "QUEUED", kAmber);
    lv_label_set_text(self->audio_detail_, "Microphone sample queued off the UI task");
}

void Shell::audio_volume_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::audio_service().snapshot().settings;
    settings.volume_percent = settings.volume_percent >= 100
                                  ? 0
                                  : static_cast<std::uint8_t>(settings.volume_percent + 10);
    const auto status = nightglass::services::audio_service().update_settings(settings);
    if (!status.is_ok()) {
        set_state(self->audio_state_, "SAVE FAILED", kRed);
        lv_label_set_text(self->audio_detail_, status.detail);
        return;
    }
    self->refresh_audio();
}

void Shell::audio_mute_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::audio_service().snapshot().settings;
    settings.muted = !settings.muted;
    const auto status = nightglass::services::audio_service().update_settings(settings);
    if (!status.is_ok()) {
        set_state(self->audio_state_, "SAVE FAILED", kRed);
        lv_label_set_text(self->audio_detail_, status.detail);
        return;
    }
    self->refresh_audio();
}

void Shell::audio_dnd_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::audio_service().snapshot().settings;
    settings.do_not_disturb = !settings.do_not_disturb;
    const auto status = nightglass::services::audio_service().update_settings(settings);
    if (!status.is_ok()) {
        set_state(self->audio_state_, "SAVE FAILED", kRed);
        lv_label_set_text(self->audio_detail_, status.detail);
        return;
    }
    self->refresh_audio();
}

void Shell::dismiss_alert_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (nightglass::services::clock_service().dismiss_alerts()) {
        self->refresh_system_overlay();
    }
}

void Shell::update_confirm_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    (void)nightglass::services::update_transport().confirm();
    self->refresh_system_overlay();
}

void Shell::update_abort_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    (void)nightglass::services::update_transport().abort();
    self->refresh_system_overlay();
}

void Shell::update_restart_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!nightglass::services::update_transport().restart()) {
        self->refresh_system_overlay();
    }
}

void Shell::diagnostics_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_diagnostics);
}

void Shell::about_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_about);
}

void Shell::activity_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_activity);
}

void Shell::activity_step_length_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::activity_service().snapshot().settings;
    settings.step_length_mm = nightglass::services::next_step_length(
        settings.step_length_mm, settings.units);
    nightglass::services::activity_service().update_settings(settings);
    self->refresh_activity();
}

void Shell::activity_units_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::activity_service().snapshot().settings;
    settings.units = settings.units == nightglass::services::ActivityUnits::imperial
                         ? nightglass::services::ActivityUnits::metric
                         : nightglass::services::ActivityUnits::imperial;
    nightglass::services::activity_service().update_settings(settings);
    self->refresh_activity();
}

void Shell::activity_goal_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::activity_service().snapshot().settings;
    settings.daily_goal_steps += 1000;
    if (settings.daily_goal_steps > 30000) settings.daily_goal_steps = 1000;
    nightglass::services::activity_service().update_settings(settings);
    self->refresh_activity();
}

void Shell::activity_reset_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    nightglass::services::activity_service().reset_today();
    self->refresh_activity();
}

void Shell::weather_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_weather);
}

void Shell::weather_toggle_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::network_weather_service().snapshot().settings;
    settings.enabled = !settings.enabled;
    nightglass::services::network_weather_service().update_settings(settings);
    self->refresh_weather();
}

void Shell::weather_units_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::network_weather_service().snapshot().settings;
    settings.units = settings.units == nightglass::services::WeatherUnits::metric
                         ? nightglass::services::WeatherUnits::imperial
                         : nightglass::services::WeatherUnits::metric;
    nightglass::services::network_weather_service().update_settings(settings);
    self->refresh_weather();
}

void Shell::weather_refresh_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::network_weather_service().snapshot().settings;
    constexpr std::uint16_t values[]{15, 30, 60, 120};
    settings.refresh_minutes = next_value(settings.refresh_minutes, values);
    nightglass::services::network_weather_service().update_settings(settings);
    nightglass::services::network_weather_service().request_refresh();
    self->refresh_weather();
}

void Shell::weather_latitude_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::network_weather_service().snapshot().settings;
    settings.location_configured = true;
    settings.latitude_e6 += 100'000;
    if (settings.latitude_e6 > 90'000'000) settings.latitude_e6 = -90'000'000;
    nightglass::services::network_weather_service().update_settings(settings);
    self->refresh_weather();
}

void Shell::weather_longitude_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::network_weather_service().snapshot().settings;
    settings.location_configured = true;
    settings.longitude_e6 += 100'000;
    if (settings.longitude_e6 > 180'000'000) settings.longitude_e6 = -180'000'000;
    nightglass::services::network_weather_service().update_settings(settings);
    self->refresh_weather();
}

void Shell::weather_clear_wifi_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    nightglass::services::network_weather_service().clear_credentials();
    self->refresh_weather();
}

void Shell::connectivity_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_connectivity);
}

void Shell::notifications_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    self->selected_notification_id_ = 0;
    self->navigate(nightglass::core::NavigationAction::open_notifications);
}

void Shell::media_app_callback(lv_event_t *event) {
    static_cast<Shell *>(lv_event_get_user_data(event))->navigate(
        nightglass::core::NavigationAction::open_media);
}

void Shell::notification_detail_callback(lv_event_t *event) {
    auto *context = static_cast<NotificationActionContext *>(lv_event_get_user_data(event));
    if (!context || !context->shell || context->id == 0) return;
    context->shell->selected_notification_id_ = context->id;
    context->shell->render_route();
}

void Shell::notification_list_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    self->selected_notification_id_ = 0;
    self->render_route();
}

void Shell::notification_reply_send_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!self || !self->notification_reply_box_ || self->selected_notification_id_ == 0) return;
    const char *text = lv_textarea_get_text(self->notification_reply_box_);
    const bool sent = text && text[0] != '\0' &&
                      nightglass::services::connectivity_service().reply_notification(
                          self->selected_notification_id_, text);
    set_button_text(lv_event_get_current_target_obj(event),
                    sent ? "SENDING..." : "FAILED");
    if (sent) lv_textarea_set_text(self->notification_reply_box_, "");
}

void Shell::notification_action_callback(lv_event_t *event) {
    auto *context = static_cast<NotificationActionContext *>(lv_event_get_user_data(event));
    if (!context || !context->shell || context->id == 0) return;
    if (context->reply != nullptr) {
        const bool sent = nightglass::services::connectivity_service().reply_notification(
            context->id, context->reply);
        set_button_text(lv_event_get_current_target_obj(event),
                        sent ? "SENDING..." : "FAILED");
        return;
    }
    const bool sent = nightglass::services::connectivity_service().mark_notification(
        context->id, context->dismiss);
    if (!context->dismiss || !sent) {
        set_button_text(lv_event_get_current_target_obj(event), sent ? "SENT" : "FAILED");
    }
}

void Shell::notification_privacy_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    if (!self) return;
    const auto snapshot = nightglass::services::connectivity_service().snapshot();
    const bool currently_hidden = snapshot.notification_privacy ==
                                  nightglass::services::NotificationPrivacyPolicy::always_redact;
    nightglass::services::connectivity_service().set_notification_privacy(
        currently_hidden
            ? nightglass::services::NotificationPrivacyPolicy::show_details
            : nightglass::services::NotificationPrivacyPolicy::always_redact,
        currently_hidden);
    self->render_route();
}

void Shell::connectivity_toggle_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::connectivity_service().snapshot().settings;
    settings.enabled = !settings.enabled;
    nightglass::services::connectivity_service().update_settings(settings);
    self->refresh_connectivity();
}

void Shell::media_callback(lv_event_t *event) {
    const auto command = static_cast<nightglass::services::MediaCommand>(
        reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event)));
    const bool sent = nightglass::services::connectivity_service().send_media(command);
    if (auto *self = &instance; self->media_state_) {
        set_state(self->media_state_, sent ? "COMMAND SENT" : "COMMAND FAILED",
                  sent ? kGreen : kRed);
    }
}

void Shell::face_action_callback(lv_event_t *event) {
    auto *target = lv_event_get_current_target_obj(event);
    auto *self = static_cast<Shell *>(lv_obj_get_user_data(target));
    if (!self) return;
    const auto action = static_cast<nightglass::services::FaceAction>(
        reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event)));
    using nightglass::core::NavigationAction;
    using nightglass::services::FaceAction;
    switch (action) {
        case FaceAction::open_apps: self->navigate(NavigationAction::open_launcher); break;
        case FaceAction::open_activity: self->navigate(NavigationAction::open_activity); break;
        case FaceAction::open_weather: self->navigate(NavigationAction::open_weather); break;
        case FaceAction::open_notifications:
            self->navigate(NavigationAction::open_notifications); break;
        case FaceAction::open_connectivity:
            self->navigate(NavigationAction::open_connectivity); break;
        case FaceAction::open_alarm: self->navigate(NavigationAction::open_alarm); break;
        case FaceAction::open_countdown: self->navigate(NavigationAction::open_countdown); break;
        case FaceAction::open_diagnostics:
            self->navigate(NavigationAction::open_diagnostics); break;
        case FaceAction::open_clock_settings:
            self->navigate(NavigationAction::open_clock_settings); break;
    }
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

    const auto &pack = nightglass::services::watchface_service().selected();
    const auto &chrome = chrome_palette();
    lv_obj_set_style_bg_color(content_host_, lv_color_hex(chrome.background), 0);
    lv_obj_set_style_bg_opa(content_host_, LV_OPA_COVER, 0);
    if (quick_settings_open_) {
        render_quick_settings();
        return;
    }
    if (navigation_.route != nightglass::core::Route::home &&
        nightglass::services::valid_face_pack(pack) &&
        pack.chrome.route_background_asset != nightglass::services::FaceAsset::none) {
        if (const auto *asset = face_asset(pack.chrome.route_background_asset)) {
            auto *background = lv_image_create(content_host_);
            lv_image_set_src(background, asset);
            lv_obj_set_pos(background, 0, 0);
            lv_obj_set_style_image_opa(background, pack.chrome.route_background_opacity, 0);
        }
    }

    switch (navigation_.route) {
        case nightglass::core::Route::home:
            render_home();
            break;
        case nightglass::core::Route::launcher:
            render_launcher();
            break;
        case nightglass::core::Route::settings:
            render_settings();
            break;
        case nightglass::core::Route::power_settings:
            render_power_settings();
            break;
        case nightglass::core::Route::clock_settings:
            render_clock_settings();
            break;
        case nightglass::core::Route::watchface_settings:
            render_watchface_settings();
            break;
        case nightglass::core::Route::activity:
            render_activity();
            break;
        case nightglass::core::Route::weather:
            render_weather();
            break;
        case nightglass::core::Route::connectivity:
            render_connectivity();
            break;
        case nightglass::core::Route::media:
            render_media();
            break;
        case nightglass::core::Route::notifications:
            render_notifications();
            break;
        case nightglass::core::Route::alarm:
            render_alarm();
            break;
        case nightglass::core::Route::countdown:
            render_countdown();
            break;
        case nightglass::core::Route::stopwatch:
            render_stopwatch();
            break;
        case nightglass::core::Route::audio:
            render_audio();
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
    const auto power = nightglass::services::power_service().snapshot();
    home_aod_active_ = power.state == nightglass::core::PowerState::dim ||
                       power.state == nightglass::core::PowerState::ambient;
    if (home_aod_active_) {
        render_aod_home();
        configure_refresh_timer(1000);
        refresh_home();
        return;
    }
    const auto &pack = nightglass::services::watchface_service().selected();
    if (pack.layout == nightglass::services::FaceLayout::full_background) {
        render_pack_home();
    } else {
        render_classic_home();
    }
    configure_refresh_timer(1000);
    refresh_home();
}

void Shell::render_aod_home() {
    constexpr std::uint32_t kAodPrimary = 0x303832;
    constexpr std::uint32_t kAodSecondary = 0x202620;
    lv_obj_set_style_bg_color(content_host_, lv_color_hex(kVoid), 0);
    home_time_ = label(content_host_, "--:--", &lv_font_montserrat_48, kAodPrimary);
    lv_obj_set_pos(home_time_, 82, 178);
    home_date_ = label(content_host_, "---- -- --", &lv_font_montserrat_16, kAodSecondary);
    lv_obj_set_pos(home_date_, 145, 244);
    home_time_state_ = label(content_host_, "DIM AMBIENT", &lv_font_montserrat_14,
                             kAodSecondary);
    lv_obj_set_pos(home_time_state_, 186, 278);
    home_battery_ = label(content_host_, "--%", &lv_font_montserrat_14, kAodSecondary);
    lv_obj_set_pos(home_battery_, 182, 318);
    home_motion_ = label(content_host_, "", &lv_font_montserrat_14, kAodSecondary);
    lv_obj_set_pos(home_motion_, 145, 350);
}

void Shell::render_classic_home() {
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
    home_motion_ = label(status_card, "MOTION | UNAVAILABLE", &lv_font_montserrat_20, kPrimary);
    lv_obj_set_pos(home_motion_, 0, 29);
    home_battery_detail_ = label(status_card, "Battery data unavailable", &lv_font_montserrat_14,
                                 kSecondary);
    lv_obj_set_pos(home_battery_detail_, 0, 67);
    lv_obj_set_width(home_battery_detail_, kSafeContentWidth - 32);
    lv_label_set_long_mode(home_battery_detail_, LV_LABEL_LONG_MODE_DOTS);

    make_button(content_host_, kSafeInset, 390, kSafeContentWidth, 70, "APPS", kCyan, kVoid,
                launcher_callback, this);

}

void Shell::render_pack_home() {
    const auto &pack = nightglass::services::watchface_service().selected();
    const auto *asset = face_asset(pack.background_asset);
    if (!asset || !nightglass::services::valid_face_pack(pack)) {
        render_classic_home();
        return;
    }

    auto *background = lv_image_create(content_host_);
    lv_image_set_src(background, asset);
    lv_obj_set_pos(background, 0, 0);

    for (std::uint8_t index = 0; index < pack.action_slot_count; ++index) {
        const auto &slot = pack.action_slots[index];
        auto *zone = lv_button_create(content_host_);
        lv_obj_set_pos(zone, slot.bounds.x, slot.bounds.y);
        lv_obj_set_size(zone, slot.bounds.width, slot.bounds.height);
        lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(zone, lv_color_hex(pack.palette.accent), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(zone, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(zone, 0, 0);
        lv_obj_set_style_border_color(zone, lv_color_hex(pack.palette.accent),
                                      LV_STATE_PRESSED);
        lv_obj_set_style_border_width(zone, 1, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(zone, 0, 0);
        lv_obj_set_user_data(zone, this);
        lv_obj_add_event_cb(
            zone, face_action_callback, LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<std::uintptr_t>(slot.action)));
    }

    for (std::uint8_t index = 0; index < pack.text_slot_count; ++index) {
        const auto &slot = pack.text_slots[index];
        if (slot.field == nightglass::services::FaceField::weather_icon) {
            auto *obj = lv_obj_create(content_host_);
            lv_obj_remove_style_all(obj);
            lv_obj_set_pos(obj, slot.bounds.x, slot.bounds.y);
            lv_obj_set_size(obj, slot.bounds.width, slot.bounds.height);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            home_weather_icon_ = obj;
            continue;
        }
        auto *obj = label(content_host_, slot.fixed_text ? slot.fixed_text : "--",
                          face_font(slot.style), face_color(pack.palette, slot.color));
        lv_obj_set_pos(obj, slot.bounds.x, slot.bounds.y);
        lv_obj_set_size(obj, slot.bounds.width, slot.bounds.height);
        lv_obj_set_style_text_align(obj, face_align(slot.align), 0);

        switch (slot.field) {
            case nightglass::services::FaceField::time:
                home_time_ = obj;
                break;
            case nightglass::services::FaceField::time_state:
                home_time_state_ = obj;
                break;
            case nightglass::services::FaceField::day:
                home_day_ = obj;
                break;
            case nightglass::services::FaceField::date:
                home_date_ = obj;
                break;
            case nightglass::services::FaceField::battery:
                home_battery_ = obj;
                break;
            case nightglass::services::FaceField::battery_detail:
                home_battery_detail_ = obj;
                break;
            case nightglass::services::FaceField::steps:
                home_steps_ = obj;
                break;
            case nightglass::services::FaceField::motion:
                home_motion_ = obj;
                break;
            case nightglass::services::FaceField::alarm:
                home_alarm_ = obj;
                break;
            case nightglass::services::FaceField::timer:
                home_timer_ = obj;
                break;
            case nightglass::services::FaceField::distance:
                home_distance_ = obj;
                break;
            case nightglass::services::FaceField::weather:
                home_weather_ = obj;
                break;
            case nightglass::services::FaceField::weather_icon:
                break;
            case nightglass::services::FaceField::notifications:
                home_notifications_ = obj;
                break;
            case nightglass::services::FaceField::connectivity:
                home_connectivity_ = obj;
                break;
            case nightglass::services::FaceField::fixed_text:
                break;
        }
    }
}

void Shell::render_launcher() {
    add_header(content_host_, "APPS", back_callback, this);
    auto *scroller = make_scroller(content_host_);
    constexpr int row_height = 64;
    constexpr int gap = 74;
    make_button(scroller, 0, 0 * gap, kSafeContentWidth - 16, row_height, "ALARM",
                kSurface, kPrimary, alarm_callback, this);
    make_button(scroller, 0, 1 * gap, kSafeContentWidth - 16, row_height, "TIMER",
                kSurface, kPrimary, countdown_callback, this);
    make_button(scroller, 0, 2 * gap, kSafeContentWidth - 16, row_height, "STOPWATCH",
                kSurface, kPrimary, stopwatch_callback, this);
    make_button(scroller, 0, 3 * gap, kSafeContentWidth - 16, row_height, "SETTINGS",
                kSurface, kPrimary, settings_callback, this);
    make_button(scroller, 0, 4 * gap, kSafeContentWidth - 16, row_height, "ACTIVITY",
                kSurface, kPrimary, activity_callback, this);
    make_button(scroller, 0, 5 * gap, kSafeContentWidth - 16, row_height, "WEATHER",
                kSurface, kPrimary, weather_callback, this);
    make_button(scroller, 0, 6 * gap, kSafeContentWidth - 16, row_height, "PHONE",
                kSurface, kPrimary, connectivity_callback, this);
    make_button(scroller, 0, 7 * gap, kSafeContentWidth - 16, row_height, "MEDIA",
                kSurface, kPrimary, media_app_callback, this);
    make_button(scroller, 0, 8 * gap, kSafeContentWidth - 16, row_height, "NOTIFICATIONS",
                kSurface, kPrimary, notifications_callback, this);
    make_button(scroller, 0, 9 * gap, kSafeContentWidth - 16, row_height, "AUDIO",
                kSurface, kPrimary, audio_callback, this);
    make_button(scroller, 0, 10 * gap, kSafeContentWidth - 16, row_height, "DIAGNOSTICS",
                kSurface, kPrimary, diagnostics_callback, this);
    make_button(scroller, 0, 11 * gap, kSafeContentWidth - 16, row_height, "ABOUT",
                kSurface, kPrimary, about_callback, this);
    make_button(scroller, 0, 12 * gap, kSafeContentWidth - 16, row_height, "QUICK SETTINGS",
                kSurface, kPrimary, quick_settings_callback, this);
}

void Shell::render_quick_settings() {
    add_header(content_host_, "QUICK SETTINGS", quick_settings_back_callback, this);
    auto *scroller = make_scroller(content_host_);
    quick_brightness_ = make_button(scroller, 0, 0, kSafeContentWidth - 16, 64, "",
                                    kSurface, kPrimary, quick_brightness_callback, this);
    quick_mute_ = make_button(scroller, 0, 74, kSafeContentWidth - 16, 64, "",
                              kSurface, kPrimary, quick_mute_callback, this);
    quick_dnd_ = make_button(scroller, 0, 148, kSafeContentWidth - 16, 64, "",
                             kSurface, kPrimary, quick_dnd_callback, this);
    quick_bluetooth_ = make_button(scroller, 0, 222, kSafeContentWidth - 16, 64, "",
                                   kSurface, kPrimary, quick_bluetooth_callback, this);
    auto *note = label(scroller, "Scheduled quiet hours are configured in Alarms.",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, 8, 310);
    lv_obj_set_width(note, kSafeContentWidth - 32);
    refresh_quick_settings();
}

void Shell::render_settings() {
    add_header(content_host_, "SETTINGS", back_callback, this);
    auto *scroller = make_scroller(content_host_);
    make_button(scroller, 0, 0, kSafeContentWidth - 16, 76, "POWER",
                kSurface, kPrimary, power_settings_callback, this);
    make_button(scroller, 0, 90, kSafeContentWidth - 16, 76, "CLOCK & REGION",
                kSurface, kPrimary, clock_settings_callback, this);
    make_button(scroller, 0, 180, kSafeContentWidth - 16, 76, "WATCH FACE",
                kSurface, kPrimary, watchface_settings_callback, this);
    make_button(scroller, 0, 270, kSafeContentWidth - 16, 76, "ACTIVITY",
                kSurface, kPrimary, activity_callback, this);
    make_button(scroller, 0, 360, kSafeContentWidth - 16, 76, "NETWORK & WEATHER",
                kSurface, kPrimary, weather_callback, this);
    make_button(scroller, 0, 450, kSafeContentWidth - 16, 76, "PHONE",
                kSurface, kPrimary, connectivity_callback, this);
    make_button(scroller, 0, 540, kSafeContentWidth - 16, 76, "SOUND & DND",
                kSurface, kPrimary, audio_callback, this);
    auto *note = label(scroller, "All settings are stored on the watch.",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, 8, 630);
    lv_obj_set_width(note, kSafeContentWidth - 32);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
}

void Shell::render_watchface_settings() {
    add_header(content_host_, "WATCH FACE", back_callback, this);
    auto *card = make_route_card(content_host_, 118, 152);
    auto *heading = label(card, "ACTIVE PACK", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(heading, 0, 0);
    watchface_name_ = label(card, "", &lv_font_montserrat_26, kPrimary);
    lv_obj_set_pos(watchface_name_, 0, 38);
    lv_obj_set_width(watchface_name_, kSafeContentWidth - 32);
    auto *contract = label(card, "Declarative | versioned | persistent",
                           &lv_font_montserrat_14, kGreen);
    lv_obj_set_pos(contract, 0, 85);

    make_button(content_host_, kSafeInset, 294, kSafeContentWidth, 72, "NEXT FACE",
                kCyan, kVoid, watchface_next_callback, this);
    auto *note = label(content_host_,
                       "Face packs choose a supported layout, palette, and complications. "
                       "They cannot execute firmware code.",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, kSafeInset, 392);
    lv_obj_set_width(note, kSafeContentWidth);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    refresh_watchface_settings();
}

void Shell::render_activity() {
    add_header(content_host_, "ACTIVITY", back_callback, this);
    auto *card = make_route_card(content_host_, 108, 126);
    activity_steps_ = label(card, "-- STEPS", &lv_font_montserrat_26, kGreen);
    lv_obj_set_pos(activity_steps_, 0, 4);
    activity_detail_ = label(card, "Calibrating activity sensor", &lv_font_montserrat_16,
                             kSecondary);
    lv_obj_set_pos(activity_detail_, 0, 52);
    lv_obj_set_width(activity_detail_, kSafeContentWidth - 32);
    activity_step_length_ = make_button(content_host_, kSafeInset, 246, kSafeContentWidth, 48, "",
                                        kSurface, kPrimary,
                                        activity_step_length_callback, this);
    activity_units_ = make_button(content_host_, kSafeInset, 304, kSafeContentWidth, 48, "",
                                  kSurface, kPrimary, activity_units_callback, this);
    activity_goal_ = make_button(content_host_, kSafeInset, 362, kSafeContentWidth, 48, "",
                                 kSurface, kPrimary, activity_goal_callback, this);
    make_button(content_host_, kSafeInset, 420, kSafeContentWidth, 48, "RESET TODAY",
                kSurface, kAmber, activity_reset_callback, this);
    configure_refresh_timer(1000);
    refresh_activity();
}

void Shell::render_weather() {
    add_header(content_host_, "NETWORK & WEATHER", back_callback, this);
    auto *scroller = make_scroller(content_host_);
    auto *card = lv_obj_create(scroller);
    lv_obj_set_size(card, kSafeContentWidth - 16, 150);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(chrome_palette().surface), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(chrome_palette().border), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    weather_state_ = label(card, "SERVICE STARTING", &lv_font_montserrat_20, kGreen);
    lv_obj_set_pos(weather_state_, 0, 4);
    weather_detail_ = label(card,
                            "Wi-Fi, location, units, and refresh controls are loading.",
                            &lv_font_montserrat_16, kSecondary);
    lv_obj_set_pos(weather_detail_, 0, 48);
    lv_obj_set_width(weather_detail_, kSafeContentWidth - 48);
    lv_label_set_long_mode(weather_detail_, LV_LABEL_LONG_MODE_WRAP);
    weather_toggle_ = make_button(scroller, 0, 164, kSafeContentWidth - 16, 58, "",
                                  kSurface, kPrimary, weather_toggle_callback, this);
    weather_units_ = make_button(scroller, 0, 234, kSafeContentWidth - 16, 58, "",
                                 kSurface, kPrimary, weather_units_callback, this);
    weather_refresh_ = make_button(scroller, 0, 304, kSafeContentWidth - 16, 58, "",
                                   kSurface, kPrimary, weather_refresh_callback, this);
    weather_latitude_ = make_button(scroller, 0, 374, kSafeContentWidth - 16, 58, "",
                                    kSurface, kPrimary, weather_latitude_callback, this);
    weather_longitude_ = make_button(scroller, 0, 444, kSafeContentWidth - 16, 58, "",
                                     kSurface, kPrimary, weather_longitude_callback, this);
    make_button(scroller, 0, 514, kSafeContentWidth - 16, 58, "CLEAR WI-FI",
                kSurface, kAmber, weather_clear_wifi_callback, this);
    auto *note = label(scroller,
                       "Wi-Fi credentials are provisioned through the encrypted companion link. "
                       "Location changes by 0.1 degree per tap.",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, 4, 584);
    lv_obj_set_width(note, kSafeContentWidth - 28);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    configure_refresh_timer(1000);
    refresh_weather();
}

void Shell::render_connectivity() {
    add_header(content_host_, "PHONE", back_callback, this);
    auto *scroller = make_scroller(content_host_);
    auto *card = make_route_card(scroller, 0, 132);
    connectivity_state_ = label(card, "BLUETOOTH", &lv_font_montserrat_20, kGreen);
    lv_obj_set_pos(connectivity_state_, 0, 4);
    connectivity_detail_ = label(card, "Starting companion service", &lv_font_montserrat_16,
                                 kSecondary);
    lv_obj_set_pos(connectivity_detail_, 0, 44);
    lv_obj_set_width(connectivity_detail_, kSafeContentWidth - 32);
    lv_label_set_long_mode(connectivity_detail_, LV_LABEL_LONG_MODE_WRAP);
    connectivity_toggle_ = make_button(scroller, 0, 144, kSafeContentWidth - 16, 54,
                                       "", kSurface, kPrimary,
                                       connectivity_toggle_callback, this);
    phone_battery_ = label(scroller, "PHONE BATTERY --", &lv_font_montserrat_16, kSecondary);
    lv_obj_set_pos(phone_battery_, 8, 214);
    phone_call_ = label(scroller, "NO ACTIVE CALL", &lv_font_montserrat_16, kSecondary);
    lv_obj_set_pos(phone_call_, 8, 246);
    lv_obj_set_width(phone_call_, kSafeContentWidth - 32);
    make_button(scroller, 0, 280, 116, 50, "ANSWER", kSurface, kPrimary,
                call_command_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::CallCommand::answer)));
    make_button(scroller, 124, 280, 116, 50, "REJECT", kSurface, kAmber,
                call_command_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::CallCommand::reject)));
    make_button(scroller, 248, 280, 116, 50, "MUTE", kSurface, kPrimary,
                call_command_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::CallCommand::mute)));
    auto *agenda_heading = label(scroller, "AGENDA", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(agenda_heading, 8, 346);
    for (std::size_t index = 0; index < agenda_items_.size(); ++index) {
        agenda_items_[index] = label(scroller, "No upcoming event", &lv_font_montserrat_16,
                                     kPrimary);
        lv_obj_set_pos(agenda_items_[index], 8, 374 + static_cast<int>(index) * 42);
        lv_obj_set_width(agenda_items_[index], kSafeContentWidth - 32);
        lv_label_set_long_mode(agenda_items_[index], LV_LABEL_LONG_MODE_DOTS);
    }
    make_button(scroller, 0, 510, 174, 52, "RING PHONE", kSurface, kPrimary,
                phone_command_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::PhoneCommand::ring_start)));
    make_button(scroller, 190, 510, 174, 52, "STOP RING", kSurface, kPrimary,
                phone_command_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::PhoneCommand::ring_stop)));
    make_button(scroller, 0, 576, kSafeContentWidth - 16, 52, "OPEN CAMERA",
                kSurface, kPrimary,
                phone_command_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::PhoneCommand::camera)));
    make_button(scroller, 0, 642, kSafeContentWidth - 16, 54, "NOTIFICATIONS",
                kSurface, kPrimary, notifications_callback, this);
    configure_refresh_timer(1000);
    refresh_connectivity();
}

void Shell::render_media() {
    add_header(content_host_, "MEDIA", back_callback, this);
    auto *card = make_route_card(content_host_, 102, 190);
    media_state_ = label(card, "PHONE DISCONNECTED", &lv_font_montserrat_14, kAmber);
    lv_obj_set_pos(media_state_, 0, 0);
    media_title_ = label(card, "Nothing playing", &lv_font_montserrat_20, kPrimary);
    lv_obj_set_pos(media_title_, 0, 34);
    lv_obj_set_width(media_title_, kSafeContentWidth - 44);
    lv_label_set_long_mode(media_title_, LV_LABEL_LONG_MODE_DOTS);
    media_artist_ = label(card, "", &lv_font_montserrat_16, kSecondary);
    lv_obj_set_pos(media_artist_, 0, 76);
    lv_obj_set_width(media_artist_, kSafeContentWidth - 44);
    lv_label_set_long_mode(media_artist_, LV_LABEL_LONG_MODE_DOTS);
    media_progress_ = lv_bar_create(card);
    lv_obj_set_size(media_progress_, kSafeContentWidth - 44, 8);
    lv_obj_set_pos(media_progress_, 0, 116);
    lv_bar_set_range(media_progress_, 0, 1000);
    media_time_ = label(card, "0:00 / 0:00", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(media_time_, 0, 136);
    make_button(content_host_, kSafeInset, 300, 108, 58, "PREV", kSurface, kPrimary,
                media_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::previous)));
    make_button(content_host_, 151, 300, 108, 58, "PLAY", kCyan, kVoid,
                media_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::play_pause)));
    make_button(content_host_, 274, 300, 108, 58, "NEXT", kSurface, kPrimary,
                media_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::next)));
    make_button(content_host_, kSafeInset, 368, 170, 48, "SEEK -10", kSurface, kPrimary,
                media_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::seek_backward)));
    make_button(content_host_, 212, 368, 170, 48, "SEEK +10", kSurface, kPrimary,
                media_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::seek_forward)));
    make_button(content_host_, kSafeInset, 426, 170, 48, "VOLUME -", kSurface, kPrimary,
                media_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::volume_down)));
    make_button(content_host_, 212, 426, 170, 48, "VOLUME +", kSurface, kPrimary,
                media_callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::volume_up)));
    configure_refresh_timer(1000);
    refresh_media();
}

void Shell::render_notifications() {
    const auto snapshot = nightglass::services::connectivity_service().snapshot();
    notification_sequence_ = snapshot.notification_sequence;
    notification_action_count_ = 0;

    if (selected_notification_id_ != 0) {
        const auto selected = std::find_if(
            snapshot.notifications.begin(), snapshot.notifications.end(),
            [this](const auto &notification) {
                return notification.valid && notification.id == selected_notification_id_;
            });
        if (selected == snapshot.notifications.end()) selected_notification_id_ = 0;
        else {
            add_header(content_host_, "NOTIFICATION", notification_list_callback, this);
            auto *detail = make_scroller(content_host_);
            auto *app = label(detail, selected->app.data(), &lv_font_montserrat_14, kGreen);
            lv_obj_set_pos(app, 8, 0);
            auto *title = label(detail, selected->title.data(), &lv_font_montserrat_20, kPrimary);
            lv_obj_set_pos(title, 8, 30);
            lv_obj_set_width(title, kSafeContentWidth - 32);
            lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
            auto *body = label(detail, selected->body.data(), &lv_font_montserrat_16, kSecondary);
            lv_obj_set_pos(body, 8, 92);
            lv_obj_set_width(body, kSafeContentWidth - 32);
            lv_obj_set_height(body, 150);
            lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
            auto &open = notification_actions_[notification_action_count_++];
            open = {this, selected->id, false, nullptr};
            make_button(detail, 0, 252, 174, 50, "OPEN ON PHONE", kSurface, kPrimary,
                        notification_action_callback, &open);
            auto &dismiss = notification_actions_[notification_action_count_++];
            dismiss = {this, selected->id, true, nullptr};
            make_button(detail, 190, 252, 174, 50, "DISMISS", kSurface, kAmber,
                        notification_action_callback, &dismiss);
            if (selected->replyable) {
                auto *heading = label(detail, "QUICK REPLY", &lv_font_montserrat_14, kSecondary);
                lv_obj_set_pos(heading, 8, 322);
                constexpr std::array<const char *, 4> replies{
                    "Yes", "No", "On my way", "I'll follow up"};
                for (std::size_t index = 0; index < replies.size(); ++index) {
                    auto &reply = notification_actions_[notification_action_count_++];
                    reply = {this, selected->id, false, replies[index]};
                    make_button(detail, index % 2 == 0 ? 0 : 190,
                                352 + static_cast<int>(index / 2) * 58,
                                174, 48, replies[index], kSurface, kPrimary,
                                notification_action_callback, &reply);
                }
                if (snapshot.reply_notification_id == selected->id) {
                    const char *reply_state = snapshot.reply_pending
                                                  ? "SENDING REPLY..."
                                                  : snapshot.reply_status == 0
                                                        ? "SENT TO PHONE"
                                                        : "REPLY FAILED";
                    auto *result = label(detail, reply_state, &lv_font_montserrat_14,
                                         snapshot.reply_pending ? kAmber
                                                                : snapshot.reply_status == 0
                                                                      ? kGreen : kRed);
                    lv_obj_set_pos(result, 8, 474);
                }
                auto *custom = label(detail, "TYPE A REPLY", &lv_font_montserrat_14,
                                     kSecondary);
                lv_obj_set_pos(custom, 8, 512);
                notification_reply_box_ = lv_textarea_create(detail);
                lv_obj_set_size(notification_reply_box_, kSafeContentWidth - 24, 68);
                lv_obj_set_pos(notification_reply_box_, 0, 540);
                lv_textarea_set_one_line(notification_reply_box_, true);
                lv_textarea_set_max_length(notification_reply_box_, 96);
                lv_textarea_set_placeholder_text(notification_reply_box_, "Reply text");
                auto *keyboard = lv_keyboard_create(detail);
                lv_obj_set_size(keyboard, kSafeContentWidth - 24, 190);
                lv_obj_set_pos(keyboard, 0, 620);
                lv_keyboard_set_textarea(keyboard, notification_reply_box_);
                make_button(detail, 0, 824, kSafeContentWidth - 16, 52, "SEND REPLY",
                            kCyan, kVoid, notification_reply_send_callback, this);
            } else {
                auto *note = label(detail, "This app did not expose an inline reply action.",
                                   &lv_font_montserrat_14, kSecondary);
                lv_obj_set_pos(note, 8, 326);
                lv_obj_set_width(note, kSafeContentWidth - 32);
                lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
            }
            configure_refresh_timer(500);
            return;
        }
    }

    add_header(content_host_, "NOTIFICATIONS", back_callback, this);
    auto *scroller = make_scroller(content_host_);

    auto *status = lv_obj_create(scroller);
    lv_obj_set_size(status, kSafeContentWidth - 16, 76);
    lv_obj_set_pos(status, 0, 0);
    lv_obj_set_style_radius(status, 14, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(chrome_palette().surface), 0);
    lv_obj_set_style_border_color(status, lv_color_hex(chrome_palette().border), 0);
    lv_obj_set_style_border_width(status, 1, 0);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    const bool connected = snapshot.state ==
                           nightglass::services::CompanionLinkState::connected_encrypted;
    notification_status_ = label(status,
        connected ? "PHONE CONNECTED" :
                    (snapshot.notification_count ? "OFFLINE - CACHED" : "PHONE DISCONNECTED"),
        &lv_font_montserrat_16, connected ? kGreen : kAmber);
    lv_obj_set_pos(notification_status_, 12, 8);
    char count_text[32]{};
    std::snprintf(count_text, sizeof(count_text), "%u NOTIFICATION%s",
                  snapshot.notification_count,
                  snapshot.notification_count == 1 ? "" : "S");
    auto *count = label(status, count_text, &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(count, 12, 38);

    notification_privacy_ = make_button(
        scroller, 0, 90, kSafeContentWidth - 16, 52,
        snapshot.notification_privacy ==
                nightglass::services::NotificationPrivacyPolicy::always_redact
            ? "PRIVACY  HIDDEN (TAP TO SHOW)"
            : "PRIVACY  SHOW DETAILS (TAP TO HIDE)",
        kSurface, kPrimary, notification_privacy_callback, this);

    int y = 156;
    for (const auto &notification : snapshot.notifications) {
        if (!notification.valid || notification_action_count_ + 2 > 24) continue;
        auto *card = lv_obj_create(scroller);
        lv_obj_set_size(card, kSafeContentWidth - 16, 174);
        lv_obj_set_pos(card, 0, y);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(chrome_palette().surface), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(chrome_palette().border), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        auto *app = label(card, notification.app.data(), &lv_font_montserrat_14, kGreen);
        lv_obj_set_pos(app, 12, 8);
        auto *title = label(card, notification.title.data(), &lv_font_montserrat_16, kPrimary);
        lv_obj_set_pos(title, 12, 34);
        lv_obj_set_width(title, kSafeContentWidth - 56);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        auto *body = label(card, notification.body.data(), &lv_font_montserrat_14, kSecondary);
        lv_obj_set_pos(body, 12, 62);
        lv_obj_set_width(body, kSafeContentWidth - 56);
        lv_obj_set_height(body, 46);
        lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);

        auto &view_context = notification_actions_[notification_action_count_++];
        view_context = {this, notification.id, false, nullptr};
        make_button(card, 12, 118, 128, 44, "VIEW", kSurface, kPrimary,
                    notification_detail_callback, &view_context);
        auto &dismiss_context = notification_actions_[notification_action_count_++];
        dismiss_context = {this, notification.id, true, nullptr};
        make_button(card, 158, 118, 152, 44, "DISMISS", kSurface, kAmber,
                    notification_action_callback, &dismiss_context);
        y += 186;
    }
    if (snapshot.notification_count == 0) {
        auto *empty = label(scroller,
                            connected ? "NO NEW NOTIFICATIONS" :
                                        "Connect the companion to receive notifications.",
                            &lv_font_montserrat_16, kSecondary);
        lv_obj_set_pos(empty, 12, 176);
        lv_obj_set_width(empty, kSafeContentWidth - 40);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
    }
    configure_refresh_timer(500);
}

void Shell::refresh_watchface_settings() {
    if (!watchface_name_) return;
    const auto &pack = nightglass::services::watchface_service().selected();
    lv_label_set_text(watchface_name_, pack.name);
}

void Shell::render_power_settings() {
    add_header(content_host_, "POWER", back_callback, this);
    auto *scroller = make_scroller(content_host_);

    setting_active_ = make_button(scroller, 0, 0, kSafeContentWidth - 16, 64, "", kSurface,
                                  kPrimary, active_brightness_callback, this);
    setting_dim_ = make_button(scroller, 0, 74, kSafeContentWidth - 16, 64, "", kSurface,
                               kPrimary, dim_brightness_callback, this);
    setting_dim_after_ = make_button(scroller, 0, 148, kSafeContentWidth - 16, 64, "", kSurface,
                                     kPrimary, dim_after_callback, this);
    setting_blank_after_ = make_button(scroller, 0, 222, kSafeContentWidth - 16, 64, "", kSurface,
                                       kPrimary, blank_after_callback, this);
    setting_sleep_after_ = make_button(scroller, 0, 296, kSafeContentWidth - 16, 64, "", kSurface,
                                       kPrimary, sleep_after_callback, this);
    refresh_settings_labels();
}

void Shell::render_clock_settings() {
    add_header(content_host_, "CLOCK", back_callback, this);
    auto *scroller = make_scroller(content_host_);
    auto *preview = lv_obj_create(scroller);
    lv_obj_set_size(preview, kSafeContentWidth - 16, 98);
    lv_obj_set_pos(preview, 0, 0);
    lv_obj_set_style_radius(preview, 14, 0);
    lv_obj_set_style_bg_color(preview, lv_color_hex(chrome_palette().surface), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(chrome_palette().border), 0);
    lv_obj_set_style_border_width(preview, revenant_chrome() ? 1 : 0, 0);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
    clock_preview_ = label(preview, "TIME UNAVAILABLE", &lv_font_montserrat_24, kPrimary);
    lv_obj_center(clock_preview_);
    setting_time_format_ = make_button(scroller, 0, 110, kSafeContentWidth - 16, 64, "",
                                       kSurface, kPrimary, time_format_callback, this);
    setting_utc_offset_ = make_button(scroller, 0, 184, kSafeContentWidth - 16, 64, "",
                                      kSurface, kPrimary, utc_offset_callback, this);
    setting_dst_ = make_button(scroller, 0, 258, kSafeContentWidth - 16, 64, "",
                               kSurface, kPrimary, dst_callback, this);
    auto *note = label(scroller, "UTC offset advances by 30 minutes per tap.",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, 8, 338);
    lv_obj_set_width(note, kSafeContentWidth - 32);
    configure_refresh_timer(1000);
    refresh_clock_settings();
}

void Shell::refresh_settings_labels() {
    if (!setting_active_) return;
    const auto settings = nightglass::services::power_service().snapshot().settings;
    char text[64]{};
    std::snprintf(text, sizeof(text), "ACTIVE BRIGHTNESS | %u%%", settings.active_brightness);
    set_button_text(setting_active_, text);
    std::snprintf(text, sizeof(text), "DIM BRIGHTNESS | %u%%", settings.dim_brightness);
    set_button_text(setting_dim_, text);
    std::snprintf(text, sizeof(text), "DIM AFTER | %u SEC", settings.dim_after_seconds);
    set_button_text(setting_dim_after_, text);
    std::snprintf(text, sizeof(text), "SCREEN OFF | %u SEC", settings.blank_after_seconds);
    set_button_text(setting_blank_after_, text);
    if (settings.sleep_after_blank_seconds == 0) {
        set_button_text(setting_sleep_after_, "LIGHT SLEEP | OFF");
    } else {
        std::snprintf(text, sizeof(text), "LIGHT SLEEP | +%u SEC",
                      settings.sleep_after_blank_seconds);
        set_button_text(setting_sleep_after_, text);
    }
}

void Shell::render_alarm() {
    add_header(content_host_, "ALARMS", back_callback, this);
    auto *scroller = make_scroller(content_host_);
    auto *card = lv_obj_create(scroller);
    lv_obj_set_size(card, kSafeContentWidth - 16, 142);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(chrome_palette().surface), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(chrome_palette().border), 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    auto *heading = label(card, "PERSISTENT ALARM", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(heading, 0, 0);
    alarm_time_ = label(card, "07:00", &lv_font_montserrat_48, kPrimary);
    lv_obj_set_pos(alarm_time_, 0, 27);
    alarm_state_ = label(card, "OFF", &lv_font_montserrat_16, kAmber);
    lv_obj_align(alarm_state_, LV_ALIGN_BOTTOM_RIGHT, 0, -8);
    alarm_slot_ = make_button(scroller, 0, 156, kSafeContentWidth - 16, 54, "", kSurface,
                              kPrimary, alarm_slot_callback, this);
    alarm_label_ = make_button(scroller, 0, 220, kSafeContentWidth - 16, 54, "", kSurface,
                               kPrimary, alarm_label_callback, this);
    make_button(scroller, 0, 284, 86, 54, "HOUR -", kSurface, kPrimary,
                alarm_hour_back_callback, this);
    make_button(scroller, 94, 284, 86, 54, "HOUR +", kSurface, kPrimary,
                alarm_hour_callback, this);
    make_button(scroller, 190, 284, 86, 54, "MIN -", kSurface, kPrimary,
                alarm_minute_back_callback, this);
    make_button(scroller, 284, 284, 80, 54, "MIN +", kSurface, kPrimary,
                alarm_minute_callback, this);
    alarm_repeat_ = make_button(scroller, 0, 348, kSafeContentWidth - 16, 54, "", kSurface,
                                kPrimary, alarm_repeat_callback, this);
    alarm_toggle_ = make_button(scroller, 0, 412, kSafeContentWidth - 16, 60, "",
                                kCyan, kVoid, alarm_enabled_callback, this);
    quiet_toggle_ = make_button(scroller, 0, 490, kSafeContentWidth - 16, 54, "", kSurface,
                                kPrimary, quiet_toggle_callback, this);
    quiet_start_ = make_button(scroller, 0, 554, 174, 54, "", kSurface, kPrimary,
                               quiet_start_callback, this);
    quiet_end_ = make_button(scroller, 190, 554, 174, 54, "", kSurface, kPrimary,
                             quiet_end_callback, this);
    auto *note = label(scroller, "Quiet hours silence notifications; alarms and timers still sound.",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, 8, 624);
    lv_obj_set_width(note, kSafeContentWidth - 32);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    configure_refresh_timer(1000);
    refresh_alarm();
}

void Shell::render_countdown() {
    add_header(content_host_, "TIMER", back_callback, this);
    auto *card = make_route_card(content_host_, 112, 142);
    auto *heading = label(card, "COUNTDOWN", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(heading, 0, 0);
    countdown_time_ = label(card, "00:05:00", &lv_font_montserrat_32, kPrimary);
    lv_obj_set_pos(countdown_time_, 0, 42);

    countdown_duration_ = make_button(content_host_, kSafeInset, 278, kSafeContentWidth, 58,
                                      "DURATION", kSurface, kPrimary,
                                      countdown_duration_callback, this);
    countdown_toggle_ = make_button(content_host_, kSafeInset, 350, 220, 70, "START",
                                    kCyan, kVoid, countdown_toggle_callback, this);
    make_button(content_host_, 262, 350, 120, 70, "RESET", kSurface, kPrimary,
                countdown_reset_callback, this);
    auto *note = label(content_host_, "Visual + audio timer alert", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, kSafeInset, 440);
    configure_refresh_timer(250);
    refresh_countdown();
}

void Shell::render_stopwatch() {
    add_header(content_host_, "STOPWATCH", back_callback, this);
    auto *card = make_route_card(content_host_, 112, 166);
    auto *heading = label(card, "ELAPSED", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(heading, 0, 0);
    stopwatch_time_ = label(card, "00:00:00.0", &lv_font_montserrat_32, kPrimary);
    lv_obj_set_pos(stopwatch_time_, 0, 52);
    stopwatch_toggle_ = make_button(content_host_, kSafeInset, 310, 220, 70, "START",
                                    kCyan, kVoid, stopwatch_toggle_callback, this);
    make_button(content_host_, 262, 310, 120, 70, "RESET", kSurface, kPrimary,
                stopwatch_reset_callback, this);
    auto *note = label(content_host_, "Runs independently of this screen.",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, kSafeInset, 408);
    lv_obj_set_width(note, kSafeContentWidth);
    configure_refresh_timer(100);
    refresh_stopwatch();
}

void Shell::render_audio() {
    add_header(content_host_, "SOUND & DND", back_callback, this);
    auto *scroller = make_scroller(content_host_);
    auto *card = lv_obj_create(scroller);
    lv_obj_set_size(card, kSafeContentWidth - 16, 154);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(chrome_palette().surface), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(chrome_palette().border), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    auto *heading = label(card, "ES8311 OUT / ES7210 IN", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(heading, 0, 0);
    audio_state_ = label(card, "WAIT", &lv_font_montserrat_20, kAmber);
    lv_obj_set_pos(audio_state_, 0, 30);
    audio_detail_ = label(card, "Waiting for codec status", &lv_font_montserrat_14, kPrimary);
    lv_obj_set_pos(audio_detail_, 0, 66);
    lv_obj_set_width(audio_detail_, kSafeContentWidth - 44);
    lv_label_set_long_mode(audio_detail_, LV_LABEL_LONG_MODE_WRAP);
    audio_level_ = label(scroller, "Mic level: not sampled", &lv_font_montserrat_16,
                         kSecondary);
    lv_obj_set_pos(audio_level_, 4, 166);
    audio_volume_ = make_button(scroller, 0, 204, kSafeContentWidth - 16, 54, "",
                                kSurface, kPrimary, audio_volume_callback, this);
    audio_mute_ = make_button(scroller, 0, 270, kSafeContentWidth - 16, 54, "",
                              kSurface, kPrimary, audio_mute_callback, this);
    audio_dnd_ = make_button(scroller, 0, 336, kSafeContentWidth - 16, 54, "",
                             kSurface, kPrimary, audio_dnd_callback, this);
    make_button(scroller, 0, 408, kSafeContentWidth - 16, 60, "PLAY TEST TONE",
                kCyan, kVoid, audio_play_callback, this);
    make_button(scroller, 0, 480, kSafeContentWidth - 16, 60, "SAMPLE MICROPHONE",
                kSurface, kPrimary, audio_capture_callback, this);
    configure_refresh_timer(1000);
    refresh_audio();
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
    lv_obj_set_style_bg_color(scroller, lv_color_hex(chrome_palette().border),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroller, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(scroller, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroller, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    make_diagnostic_card(scroller, 106, "RTC | PCF85063", &diagnostics_rtc_state_,
                         &diagnostics_rtc_detail_);
    make_diagnostic_card(scroller, 126, "POWER | AXP2101", &diagnostics_battery_state_,
                         &diagnostics_battery_detail_);
    make_diagnostic_card(scroller, 152, "MOTION | QMI8658", &diagnostics_motion_state_,
                         &diagnostics_motion_detail_);
    make_diagnostic_card(scroller, 116, "HAPTIC | GPIO18", &diagnostics_haptic_state_,
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
    auto *version = label(identity, "ESP-IDF 5.5.5 | LVGL 9.5", &lv_font_montserrat_14,
                          kSecondary);
    lv_obj_set_pos(version, 0, 78);

    auto *hardware = make_route_card(content_host_, 270, 154);
    auto *heading = label(hardware, "HARDWARE", &lv_font_montserrat_14, kViolet);
    lv_obj_set_pos(heading, 0, 0);
    auto *detail = label(hardware,
                         "ESP32-S3 | 8 MB PSRAM\n32 MB flash | 410 x 502 AMOLED\n"
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
        case nightglass::core::Route::clock_settings:
            refresh_clock_settings();
            break;
        case nightglass::core::Route::watchface_settings:
            refresh_watchface_settings();
            break;
        case nightglass::core::Route::activity:
            refresh_activity();
            break;
        case nightglass::core::Route::weather:
            refresh_weather();
            break;
        case nightglass::core::Route::connectivity:
            refresh_connectivity();
            break;
        case nightglass::core::Route::media:
            refresh_media();
            break;
        case nightglass::core::Route::notifications:
            refresh_notifications();
            break;
        case nightglass::core::Route::alarm:
            refresh_alarm();
            break;
        case nightglass::core::Route::countdown:
            refresh_countdown();
            break;
        case nightglass::core::Route::stopwatch:
            refresh_stopwatch();
            break;
        case nightglass::core::Route::audio:
            refresh_audio();
            break;
        case nightglass::core::Route::settings:
        case nightglass::core::Route::power_settings:
        case nightglass::core::Route::launcher:
        case nightglass::core::Route::about:
            break;
    }
}

void Shell::refresh_notifications() {
    const auto snapshot = nightglass::services::connectivity_service().snapshot();
    if (snapshot.notification_sequence != notification_sequence_) render_route();
}

void Shell::refresh_activity() {
    if (!activity_steps_) return;
    const auto snapshot = nightglass::services::activity_service().snapshot();
    char text[96]{};
    std::snprintf(text, sizeof(text), "%lu STEPS", static_cast<unsigned long>(snapshot.steps_today));
    lv_label_set_text(activity_steps_, text);
    char distance[32]{};
    nightglass::services::format_activity_distance(
        distance, sizeof(distance), snapshot.distance_mm, snapshot.settings.units);
    std::snprintf(text, sizeof(text), "EST %s | %u%% goal | %s", distance,
                  snapshot.goal_percent,
                  snapshot.readiness == nightglass::services::ActivityReadiness::ready
                      ? "READY"
                      : snapshot.readiness == nightglass::services::ActivityReadiness::warming_up
                            ? "CALIBRATING"
                            : "UNAVAILABLE");
    lv_label_set_text(activity_detail_, text);
    nightglass::services::format_step_length(
        text, sizeof(text), snapshot.settings.step_length_mm, snapshot.settings.units);
    set_button_text(activity_step_length_, text);
    set_button_text(activity_units_, snapshot.settings.units ==
                                         nightglass::services::ActivityUnits::imperial
                                     ? "DISTANCE  IMPERIAL" : "DISTANCE  METRIC");
    std::snprintf(text, sizeof(text), "DAILY GOAL  %lu",
                  static_cast<unsigned long>(snapshot.settings.daily_goal_steps));
    set_button_text(activity_goal_, text);
}

void Shell::refresh_weather() {
    if (!weather_state_) return;
    const auto snapshot = nightglass::services::network_weather_service().snapshot();
    char text[128]{};
    const char *source = snapshot.source == nightglass::services::WeatherSource::phone
                             ? "PHONE"
                             : snapshot.source == nightglass::services::WeatherSource::direct
                                   ? "DIRECT"
                                   : snapshot.source == nightglass::services::WeatherSource::cache
                                         ? "CACHED"
                                         : "NO DATA";
    if (snapshot.data_valid) {
        std::snprintf(text, sizeof(text), "%.0f%s | %s", snapshot.current.temperature,
                      snapshot.settings.units == nightglass::services::WeatherUnits::metric
                          ? " C" : " F", source);
        set_state(weather_state_, text, snapshot.stale ? kAmber : kGreen);
        std::snprintf(text, sizeof(text), "FEELS %.0f | WIND %.0f | CODE %u\nAGE %lu min%s",
                      snapshot.current.apparent_temperature, snapshot.current.wind_speed,
                      snapshot.current.weather_code,
                      static_cast<unsigned long>(snapshot.age_seconds / 60),
                      snapshot.stale ? " | OFFLINE" : "");
    } else {
        set_state(weather_state_, "WEATHER UNAVAILABLE", kAmber);
        std::snprintf(text, sizeof(text), "%s | %s",
                      snapshot.settings.location_configured ? "PHONE DATA WAITING" : "LOCATION NEEDED",
                      snapshot.credentials_configured ? "DIRECT FALLBACK READY"
                                                      : "PHONE PROXY PREFERRED");
    }
    lv_label_set_text(weather_detail_, text);
    std::snprintf(text, sizeof(text), "WEATHER  %s", snapshot.settings.enabled ? "ON" : "OFF");
    set_button_text(weather_toggle_, text);
    set_button_text(weather_units_, snapshot.settings.units == nightglass::services::WeatherUnits::metric
                                        ? "UNITS  METRIC" : "UNITS  IMPERIAL");
    std::snprintf(text, sizeof(text), "REFRESH  %u MIN", snapshot.settings.refresh_minutes);
    set_button_text(weather_refresh_, text);
    std::snprintf(text, sizeof(text), "LAT  %.1f", snapshot.settings.latitude_e6 / 1'000'000.0);
    set_button_text(weather_latitude_, text);
    std::snprintf(text, sizeof(text), "LON  %.1f", snapshot.settings.longitude_e6 / 1'000'000.0);
    set_button_text(weather_longitude_, text);
}

void Shell::refresh_connectivity() {
    if (!connectivity_state_) return;
    const auto snapshot = nightglass::services::connectivity_service().snapshot();
    const char *state = snapshot.state == nightglass::services::CompanionLinkState::connected_encrypted
                            ? "PHONE CONNECTED"
                            : snapshot.state == nightglass::services::CompanionLinkState::advertising
                                  ? "PAIRING READY"
                                  : snapshot.state == nightglass::services::CompanionLinkState::disabled
                                        ? "BLUETOOTH OFF" : "LINK UNAVAILABLE";
    set_state(connectivity_state_, state,
              snapshot.state == nightglass::services::CompanionLinkState::connected_encrypted
                  ? kGreen : kAmber);
    char text[96]{};
    if (snapshot.notification_count && snapshot.notifications[0].valid) {
        std::snprintf(text, sizeof(text), "%s | %s", snapshot.notifications[0].app.data(),
                      snapshot.notifications[0].title.data());
    } else {
        std::snprintf(text, sizeof(text), "%s | %u notifications", snapshot.detail.data(),
                      snapshot.notification_count);
    }
    lv_label_set_text(connectivity_detail_, text);
    set_button_text(connectivity_toggle_, snapshot.settings.enabled ? "BLUETOOTH  ON"
                                                                    : "BLUETOOTH  OFF");
    if (phone_battery_) {
        if (snapshot.phone_battery.valid) {
            std::snprintf(text, sizeof(text), "PHONE BATTERY  %u%%%s%s",
                          snapshot.phone_battery.percent,
                          snapshot.phone_battery.charging ? " | CHARGING" : "",
                          snapshot.phone_battery.power_save ? " | SAVER" : "");
        } else {
            std::snprintf(text, sizeof(text), "PHONE BATTERY  --");
        }
        lv_label_set_text(phone_battery_, text);
    }
    if (phone_call_) {
        std::snprintf(text, sizeof(text), "%s%s%s",
                      snapshot.call.ringing ? "INCOMING | "
                      : snapshot.call.active ? "IN CALL | " : "NO ACTIVE CALL",
                      snapshot.call.ringing || snapshot.call.active
                          ? snapshot.call.label.data() : "",
                      snapshot.call.muted ? " | MUTED" : "");
        lv_label_set_text(phone_call_, text);
    }
    for (std::size_t index = 0; index < agenda_items_.size(); ++index) {
        if (!agenda_items_[index]) continue;
        const auto &entry = snapshot.agenda.events[index];
        if (index < snapshot.agenda.count && entry.valid) {
            if (entry.all_day) {
                std::snprintf(text, sizeof(text), "ALL DAY | %s", entry.title.data());
            } else {
                const auto clock = nightglass::services::clock_service().snapshot();
                const auto offset_minutes = clock.settings.utc_offset_minutes +
                                            (clock.settings.daylight_saving ? 60 : 0);
                const auto start = nightglass::services::epoch_to_civil(
                    static_cast<std::int64_t>(entry.start_epoch_seconds) +
                    static_cast<std::int64_t>(offset_minutes) * 60);
                const auto end = nightglass::services::epoch_to_civil(
                    static_cast<std::int64_t>(entry.end_epoch_seconds) +
                    static_cast<std::int64_t>(offset_minutes) * 60);
                const auto duration_minutes = entry.end_epoch_seconds >= entry.start_epoch_seconds
                                                  ? (entry.end_epoch_seconds -
                                                     entry.start_epoch_seconds) / 60U
                                                  : 0U;
                std::snprintf(text, sizeof(text), "%02u:%02u-%02u:%02u (%lum) | %s",
                              start.hour, start.minute, end.hour, end.minute,
                              static_cast<unsigned long>(duration_minutes),
                              entry.title.data());
            }
            lv_label_set_text(agenda_items_[index], text);
        } else {
            lv_label_set_text(agenda_items_[index], index == 0 ? "No upcoming events" : "");
        }
    }
}

void Shell::refresh_media() {
    if (!media_state_) return;
    const auto snapshot = nightglass::services::connectivity_service().snapshot();
    const bool connected = snapshot.state ==
                           nightglass::services::CompanionLinkState::connected_encrypted;
    set_state(media_state_, !connected ? "PHONE DISCONNECTED"
                                      : snapshot.media.playing ? "PLAYING"
                                                               : snapshot.media.available
                                                                     ? "PAUSED"
                                                                     : "NO MEDIA SESSION",
              connected && snapshot.media.available ? kGreen : kAmber);
    lv_label_set_text(media_title_, snapshot.media.available && snapshot.media.title[0]
                                         ? snapshot.media.title.data()
                                         : "Nothing playing");
    lv_label_set_text(media_artist_, snapshot.media.available
                                          ? snapshot.media.artist.data()
                                          : "Start music on the phone");
    if (media_progress_) {
        const auto progress = snapshot.media.duration_ms > 0
                                  ? std::min<std::uint32_t>(
                                        1000U, static_cast<std::uint32_t>(
                                            static_cast<std::uint64_t>(
                                                snapshot.media.position_ms) * 1000U /
                                            snapshot.media.duration_ms))
                                  : 0U;
        lv_bar_set_value(media_progress_, static_cast<std::int32_t>(progress), LV_ANIM_OFF);
        if (snapshot.media.seekable) lv_obj_remove_flag(media_progress_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(media_progress_, LV_OBJ_FLAG_HIDDEN);
    }
    if (media_time_) {
        char position[16]{};
        char duration[16]{};
        char combined[40]{};
        format_media_time(position, sizeof(position), snapshot.media.position_ms);
        format_media_time(duration, sizeof(duration), snapshot.media.duration_ms);
        std::snprintf(combined, sizeof(combined), "%s / %s%s", position, duration,
                      snapshot.media.seekable ? " | SEEK READY" : "");
        lv_label_set_text(media_time_, combined);
    }
}

void Shell::refresh_home() {
    if (!home_time_) return;
    const auto power = nightglass::services::power_service().snapshot();
    const bool should_use_aod = power.state == nightglass::core::PowerState::dim ||
                                power.state == nightglass::core::PowerState::ambient;
    if (should_use_aod != home_aod_active_) {
        home_aod_active_ = should_use_aod;
        render_route();
        return;
    }
    const auto snapshot = nightglass::services::hardware_service().snapshot();
    const auto clock = nightglass::services::clock_service().snapshot();
    const auto now = esp_timer_get_time();
    const auto &pack = nightglass::services::watchface_service().selected();
    const bool full_background = !home_aod_active_ && pack.layout ==
                                 nightglass::services::FaceLayout::full_background;
    char buffer[96]{};

    if (clock.time_valid) {
        const char *period = "";
        format_time(buffer, sizeof(buffer), clock.local_time,
                    clock.settings.use_24_hour, &period);
        if (period[0]) {
            const auto length = std::strlen(buffer);
            std::snprintf(buffer + length, sizeof(buffer) - length, " %s", period);
        }
        lv_label_set_text(home_time_, buffer);
        if (full_background) {
            static constexpr const char *days[]{"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY",
                                                "THURSDAY", "FRIDAY", "SATURDAY"};
            static constexpr const char *months[]{"", "JAN", "FEB", "MAR", "APR", "MAY",
                                                  "JUN", "JUL", "AUG", "SEP", "OCT", "NOV",
                                                  "DEC"};
            const auto day_index = static_cast<unsigned>(clock.local_time.weekday % 7);
            lv_label_set_text(home_day_, days[day_index]);
            const auto month_index = clock.local_time.month <= 12 ? clock.local_time.month : 0;
            std::snprintf(buffer, sizeof(buffer), "%s %02u", months[month_index],
                          clock.local_time.day);
        } else {
            std::snprintf(buffer, sizeof(buffer), "%04ld-%02u-%02u",
                          static_cast<long>(clock.local_time.year), clock.local_time.month,
                          clock.local_time.day);
        }
        lv_label_set_text(home_date_, buffer);
        const int effective_offset = clock.settings.utc_offset_minutes +
                                     (clock.settings.daylight_saving ? 60 : 0);
        const int offset_abs = effective_offset < 0 ? -effective_offset : effective_offset;
        if (full_background) {
            std::snprintf(buffer, sizeof(buffer), "RTC LIVE");
            set_state(home_time_state_, buffer, pack.palette.accent);
        } else {
            std::snprintf(buffer, sizeof(buffer), "UTC%c%d:%02d | LIVE",
                          effective_offset < 0 ? '-' : '+',
                          offset_abs / 60, offset_abs % 60);
            set_state(home_time_state_, buffer, kGreen);
        }
    } else {
        lv_label_set_text(home_time_, "--:--");
        lv_label_set_text(home_date_, full_background ? "NO DATE" : "DATE UNAVAILABLE");
        if (home_day_) lv_label_set_text(home_day_, "NO DAY");
        set_state(home_time_state_, full_background ? "NO TIME" : "TIME UNAVAILABLE", kRed);
    }

    const auto &battery = snapshot.battery;
    const auto battery_age = now - battery.sampled_at_us;
    if (battery.pmic_present && battery.battery_present && battery.percent_valid &&
        battery_age <= 10'000'000) {
        if (full_background) {
            std::snprintf(buffer, sizeof(buffer), "%u%%", battery.percent);
        } else {
            std::snprintf(buffer, sizeof(buffer), "%s%u%%", battery.charging ? "CHG " : "",
                          battery.percent);
        }
        lv_label_set_text(home_battery_, buffer);
        if (battery.voltage_valid) {
            if (full_background) {
                std::snprintf(buffer, sizeof(buffer), "%u.%02uV | %s",
                              battery.voltage_mv / 1000, (battery.voltage_mv % 1000) / 10,
                              battery.charging ? "CHARGE" : battery.discharging ? "DRAIN"
                                                                                 : "STATE ?");
            } else {
                std::snprintf(buffer, sizeof(buffer), "%u.%02u V | %s",
                              battery.voltage_mv / 1000, (battery.voltage_mv % 1000) / 10,
                              battery.charging ? "Charging" : battery.discharging ? "Battery"
                                                                                 : "State partial");
            }
            if (home_battery_detail_) lv_label_set_text(home_battery_detail_, buffer);
        } else {
            if (home_battery_detail_) {
                lv_label_set_text(home_battery_detail_,
                                  full_background ? "VOLTAGE N/A"
                                  : battery.charging ? "Charging | voltage unavailable"
                                                     : "Voltage unavailable");
            }
        }
        lv_obj_set_style_text_color(home_battery_,
                                    lv_color_hex(battery.percent <= 10 ? kRed
                                                 : battery.percent <= 20 ? kAmber
                                                 : full_background ? pack.palette.accent
                                                                   : kSecondary),
                                    0);
    } else if (battery.pmic_present && !battery.battery_present) {
        set_state(home_battery_, full_background ? "N/A" : "NO BATTERY", kAmber);
        if (home_battery_detail_) {
            lv_label_set_text(home_battery_detail_,
                              full_background ? "NO BATTERY" : "Battery not detected");
        }
    } else {
        set_state(home_battery_, full_background ? "N/A" : "BATTERY --", kRed);
        if (home_battery_detail_) {
            lv_label_set_text(home_battery_detail_,
                              full_background ? "NO DATA" : "Battery data unavailable");
        }
    }

    const auto activity = nightglass::services::activity_service().snapshot();
    if (home_steps_) {
        if (activity.readiness == nightglass::services::ActivityReadiness::ready) {
            std::snprintf(buffer, sizeof(buffer), "%lu",
                          static_cast<unsigned long>(activity.steps_today));
            set_state(home_steps_, buffer, pack.palette.accent);
        } else {
            set_state(home_steps_, "CAL", kAmber);
        }
    }
    if (home_distance_) {
        if (activity.readiness == nightglass::services::ActivityReadiness::ready) {
            nightglass::services::format_activity_distance(
                buffer, sizeof(buffer), activity.distance_mm, activity.settings.units);
            lv_label_set_text(home_distance_, buffer);
        } else {
            lv_label_set_text(home_distance_, "CALIBRATING");
        }
    }
    const auto weather = nightglass::services::network_weather_service().snapshot();
    if (home_weather_) {
        if (weather.data_valid) {
            std::snprintf(buffer, sizeof(buffer), "%.0f%s", weather.current.temperature,
                          weather.settings.units == nightglass::services::WeatherUnits::metric
                              ? "C" : "F");
            set_state(home_weather_, buffer, weather.stale ? kAmber : pack.palette.accent);
        } else {
            set_state(home_weather_, "--", pack.palette.secondary);
        }
    }
    const auto connectivity = nightglass::services::connectivity_service().snapshot();
    if (home_weather_icon_) {
        if (weather.data_valid) {
            const auto icon = nightglass::services::weather_icon_for_code(
                weather.current.weather_code, weather.current.is_day);
            const auto color = weather.stale ? kAmber : pack.palette.accent;
            if (!home_weather_icon_state_valid_ || icon != home_weather_icon_state_ ||
                color != home_weather_icon_color_) {
                draw_weather_icon(home_weather_icon_, icon, color);
                home_weather_icon_state_valid_ = true;
                home_weather_icon_state_ = icon;
                home_weather_icon_color_ = color;
            }
        } else if (home_weather_icon_state_valid_) {
            lv_obj_clean(home_weather_icon_);
            home_weather_icon_state_valid_ = false;
        }
    }
    if (home_notifications_) {
        std::snprintf(buffer, sizeof(buffer), "%u NEW", connectivity.notification_count);
        set_state(home_notifications_, buffer,
                  connectivity.notification_count ? pack.palette.accent : pack.palette.secondary);
    }
    if (home_connectivity_) {
        const bool connected = connectivity.state ==
                               nightglass::services::CompanionLinkState::connected_encrypted;
        const char *link = connected ? "LINK\nON"
                           : connectivity.state ==
                                 nightglass::services::CompanionLinkState::advertising
                               ? "PAIR"
                               : connectivity.settings.enabled ? "LINK\nWAIT" : "OFF";
        set_state(home_connectivity_, link,
                  connected ? pack.palette.accent : pack.palette.secondary);
    }

    const auto &motion = snapshot.motion;
    const auto motion_age = now - motion.sampled_at_us;
    if (motion.present && motion.valid && motion_age <= 2'000'000) {
        if (!motion.gyro_calibrated) {
            if (full_background) {
                const unsigned progress = motion.gyro_calibration_required == 0
                                              ? 0
                                              : 100U * motion.gyro_calibration_samples /
                                                    motion.gyro_calibration_required;
                std::snprintf(buffer, sizeof(buffer), "CAL\n%u%%", progress);
                set_state(home_motion_, buffer, kAmber);
            } else {
                set_state(home_motion_, "MOTION | CALIBRATING", kAmber);
            }
        } else {
            const auto color = motion_age > 500'000
                                   ? kAmber
                                   : full_background
                                         ? (motion.moving ? pack.palette.accent
                                                          : pack.palette.primary)
                                         : (motion.moving ? kCyan : kPrimary);
            set_state(home_motion_, full_background ? (motion.moving ? "MOVE" : "STILL")
                                                    : (motion.moving ? "MOTION | MOVING"
                                                                     : "MOTION | STILL"),
                      color);
        }
    } else {
        set_state(home_motion_, full_background ? "N/A\nNO DATA" : "MOTION | UNAVAILABLE", kRed);
    }

    if (home_alarm_) {
        const auto next = clock.time_valid
                              ? nightglass::services::next_alarm_index(
                                    clock.alarms, nightglass::services::civil_to_epoch(
                                                      clock.local_time),
                                    clock.local_time.weekday)
                              : nightglass::services::kNoAlarmIndex;
        if (next < clock.alarms.size()) {
            const auto &alarm = clock.alarms[next];
            std::snprintf(buffer, sizeof(buffer), full_background ? "%02u:%02u\nON" : "%02u:%02u",
                          alarm.hour, alarm.minute);
        } else {
            std::snprintf(buffer, sizeof(buffer), "OFF");
        }
        lv_label_set_text(home_alarm_, buffer);
    }
    if (home_timer_) {
        if (clock.timer_ringing) {
            std::snprintf(buffer, sizeof(buffer), "DONE");
        } else if (clock.timer_running) {
            if (full_background) {
                std::snprintf(buffer, sizeof(buffer), "%02u:%02u",
                              static_cast<unsigned>(clock.timer_remaining_seconds / 60),
                              static_cast<unsigned>(clock.timer_remaining_seconds % 60));
            } else {
                const auto minutes = (clock.timer_remaining_seconds + 59) / 60;
                std::snprintf(buffer, sizeof(buffer), "%uM", static_cast<unsigned>(minutes));
            }
        } else {
            std::snprintf(buffer, sizeof(buffer), "IDLE");
        }
        lv_label_set_text(home_timer_, buffer);
    }
    if (home_aod_active_) {
        constexpr std::uint32_t kAodPrimary = 0x303832;
        constexpr std::uint32_t kAodSecondary = 0x202620;
        const auto shift_minute = clock.time_valid
                                      ? static_cast<std::uint64_t>(clock.local_time.minute)
                                      : static_cast<std::uint64_t>(now / 60'000'000);
        const int shift_x = static_cast<int>(shift_minute % 3U) * 4 - 4;
        const int shift_y = static_cast<int>(shift_minute % 2U) * 4 - 2;
        lv_obj_set_pos(home_time_, 82 + shift_x, 178 + shift_y);
        lv_obj_set_pos(home_date_, 145 - shift_x, 244 - shift_y);
        lv_obj_set_pos(home_time_state_, 186 + shift_y, 278 - shift_x);
        lv_obj_set_pos(home_battery_, 182 - shift_x, 318 + shift_y);
        lv_obj_set_pos(home_motion_, 145 + shift_x, 350 - shift_y);
        lv_obj_set_style_text_color(home_time_, lv_color_hex(kAodPrimary), 0);
        lv_obj_set_style_text_color(home_date_, lv_color_hex(kAodSecondary), 0);
        lv_obj_set_style_text_color(home_time_state_, lv_color_hex(kAodSecondary), 0);
        lv_obj_set_style_text_color(home_battery_, lv_color_hex(kAodSecondary), 0);
        lv_obj_set_style_text_color(home_motion_, lv_color_hex(kAodSecondary), 0);
    }
}

void Shell::refresh_clock_settings() {
    if (!setting_time_format_) return;
    const auto snapshot = nightglass::services::clock_service().snapshot();
    set_button_text(setting_time_format_, snapshot.settings.use_24_hour
                                             ? "TIME FORMAT | 24 HOUR"
                                             : "TIME FORMAT | 12 HOUR");
    const int offset = snapshot.settings.utc_offset_minutes;
    const int offset_abs = offset < 0 ? -offset : offset;
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "UTC OFFSET | %c%d:%02d",
                  offset < 0 ? '-' : '+', offset_abs / 60, offset_abs % 60);
    set_button_text(setting_utc_offset_, buffer);
    set_button_text(setting_dst_, snapshot.settings.daylight_saving
                                      ? "DAYLIGHT SAVING | ON"
                                      : "DAYLIGHT SAVING | OFF");
    if (snapshot.time_valid && clock_preview_) {
        const char *period = "";
        format_time(buffer, sizeof(buffer), snapshot.local_time,
                    snapshot.settings.use_24_hour, &period);
        char preview[96]{};
        std::snprintf(preview, sizeof(preview), "%s%s%s\n%04ld-%02u-%02u",
                      buffer, period[0] ? " " : "", period,
                      static_cast<long>(snapshot.local_time.year), snapshot.local_time.month,
                      snapshot.local_time.day);
        lv_label_set_text(clock_preview_, preview);
        lv_obj_set_style_text_align(clock_preview_, LV_TEXT_ALIGN_CENTER, 0);
    } else if (clock_preview_) {
        lv_label_set_text(clock_preview_, "TIME UNAVAILABLE");
    }
}

void Shell::refresh_alarm() {
    if (!alarm_time_) return;
    const auto snapshot = nightglass::services::clock_service().snapshot();
    const auto &alarm = snapshot.alarms[alarm_slot_index_];
    nightglass::services::CivilTime alarm_time{};
    alarm_time.hour = alarm.hour;
    alarm_time.minute = alarm.minute;
    char buffer[64]{};
    const char *period = "";
    format_time(buffer, sizeof(buffer), alarm_time, snapshot.settings.use_24_hour, &period);
    char display[64]{};
    std::snprintf(display, sizeof(display), "%s%s%s", buffer, period[0] ? " " : "", period);
    lv_label_set_text(alarm_time_, display);
    const bool selected_ringing = snapshot.alarm_ringing &&
                                  snapshot.ringing_alarm_index == alarm_slot_index_;
    set_state(alarm_state_, selected_ringing ? "RINGING" : alarm.enabled ? "ON" : "OFF",
              selected_ringing ? kRed : alarm.enabled ? kGreen : kAmber);
    if (alarm_toggle_) {
        set_button_text(alarm_toggle_, alarm.enabled ? "DISABLE THIS ALARM" : "ENABLE THIS ALARM");
    }
    std::snprintf(buffer, sizeof(buffer), "ALARM %u OF %u", alarm_slot_index_ + 1,
                  static_cast<unsigned>(nightglass::services::kAlarmCapacity));
    set_button_text(alarm_slot_, buffer);
    std::snprintf(buffer, sizeof(buffer), "LABEL  %s", alarm.label.data());
    set_button_text(alarm_label_, buffer);
    const char *repeat = alarm.repeat_days == nightglass::services::kEveryDayMask
                             ? "EVERY DAY"
                             : alarm.repeat_days == nightglass::services::kWeekdayMask
                                   ? "WEEKDAYS" : "WEEKENDS";
    std::snprintf(buffer, sizeof(buffer), "REPEAT  %s", repeat);
    set_button_text(alarm_repeat_, buffer);
    set_button_text(quiet_toggle_, snapshot.quiet_hours.enabled
                                       ? snapshot.quiet_hours_active
                                             ? "QUIET HOURS  ACTIVE"
                                             : "QUIET HOURS  ON"
                                       : "QUIET HOURS  OFF");
    std::snprintf(buffer, sizeof(buffer), "START  %02u:%02u",
                  snapshot.quiet_hours.start_minute / 60,
                  snapshot.quiet_hours.start_minute % 60);
    set_button_text(quiet_start_, buffer);
    std::snprintf(buffer, sizeof(buffer), "END  %02u:%02u",
                  snapshot.quiet_hours.end_minute / 60,
                  snapshot.quiet_hours.end_minute % 60);
    set_button_text(quiet_end_, buffer);
}

void Shell::refresh_countdown() {
    if (!countdown_time_) return;
    const auto snapshot = nightglass::services::clock_service().snapshot();
    char buffer[64]{};
    format_duration(buffer, sizeof(buffer),
                    static_cast<std::uint64_t>(snapshot.timer_remaining_seconds) * 1000, false);
    lv_label_set_text(countdown_time_, buffer);
    std::snprintf(buffer, sizeof(buffer), "DURATION | %u MIN",
                  static_cast<unsigned>(snapshot.timer_configured_seconds / 60));
    set_button_text(countdown_duration_, buffer);
    set_button_text(countdown_toggle_, snapshot.timer_running ? "PAUSE" : "START");
}

void Shell::refresh_stopwatch() {
    if (!stopwatch_time_) return;
    const auto snapshot = nightglass::services::clock_service().snapshot();
    char buffer[64]{};
    format_duration(buffer, sizeof(buffer), snapshot.stopwatch_elapsed_ms, true);
    lv_label_set_text(stopwatch_time_, buffer);
    set_button_text(stopwatch_toggle_, snapshot.stopwatch_running ? "PAUSE" : "START");
}

void Shell::refresh_audio() {
    if (!audio_state_) return;
    const auto snapshot = nightglass::services::audio_service().snapshot();
    char setting[48]{};
    std::snprintf(setting, sizeof(setting), "WATCH VOLUME  %u%%",
                  snapshot.settings.volume_percent);
    set_button_text(audio_volume_, setting);
    set_button_text(audio_mute_, snapshot.settings.muted ? "WATCH SOUND  MUTED"
                                                         : "WATCH SOUND  ON");
    set_button_text(audio_dnd_, snapshot.settings.do_not_disturb ? "DO NOT DISTURB  ON"
                               : snapshot.scheduled_dnd ? "QUIET HOURS  ACTIVE"
                                                        : "DO NOT DISTURB  OFF");
    if (!snapshot.enabled) {
        set_state(audio_state_, "DISABLED", kAmber);
        lv_label_set_text(audio_detail_, "Audio is disabled in this build");
        return;
    }
    if (snapshot.hardware_failed) {
        set_state(audio_state_, "UNAVAILABLE", kRed);
        lv_label_set_text(audio_detail_, snapshot.amplifier_disabled_verified
                                             ? "Audio cleanup failed; restart required"
                                             : "PA low was not verified; audio locked");
        return;
    }
    if (snapshot.operation_pending) {
        set_state(audio_state_, "RUNNING", kAmber);
        lv_label_set_text(audio_detail_,
                          snapshot.operation == nightglass::services::AudioOperation::capture
                              ? "Microphone sample in progress"
                              : "Speaker test in progress");
        return;
    }
    if (snapshot.operation != nightglass::services::AudioOperation::none &&
        !snapshot.last_operation_ok) {
        set_state(audio_state_, "FAILED", kRed);
        lv_label_set_text(audio_detail_,
                          snapshot.operation == nightglass::services::AudioOperation::capture
                              ? "Microphone capture failed"
                              : "Speaker test failed; output was shut down");
        return;
    }
    if (!snapshot.hardware_initialized) {
        set_state(audio_state_, "READY TO TEST", kAmber);
        lv_label_set_text(audio_detail_, "Codec opens only for an explicit test");
        return;
    }
    char buffer[160]{};
    if (snapshot.operation == nightglass::services::AudioOperation::capture &&
        snapshot.last_operation_ok) {
        char capture_buffer[96]{};
        std::snprintf(capture_buffer, sizeof(capture_buffer), "MIC RMS %lu | %lu B",
                      static_cast<unsigned long>(snapshot.last_capture_rms),
                      static_cast<unsigned long>(snapshot.last_transfer_bytes));
        set_state(audio_state_, "CAPTURED", kGreen);
        lv_label_set_text(audio_level_, capture_buffer);
        std::snprintf(buffer, sizeof(buffer),
                      "%s | 16 kHz mono\nRX %lu | TX %lu frames\nErrors %lu/%lu\nPA OFF",
                      capture_buffer,
                      static_cast<unsigned long>(snapshot.frames_captured),
                      static_cast<unsigned long>(snapshot.frames_played),
                      static_cast<unsigned long>(snapshot.read_errors),
                      static_cast<unsigned long>(snapshot.write_errors));
    } else if (snapshot.operation == nightglass::services::AudioOperation::playback &&
               snapshot.last_operation_ok) {
        set_state(audio_state_, "PLAYED", kGreen);
        std::snprintf(buffer, sizeof(buffer),
                      "880 Hz / 2 s test %lu B | PA OFF\nRX %lu | TX %lu frames\nErrors %lu/%lu",
                      static_cast<unsigned long>(snapshot.last_transfer_bytes),
                      static_cast<unsigned long>(snapshot.frames_captured),
                      static_cast<unsigned long>(snapshot.frames_played),
                      static_cast<unsigned long>(snapshot.read_errors),
                      static_cast<unsigned long>(snapshot.write_errors));
    } else {
        set_state(audio_state_, snapshot.amplifier_disabled_verified ? "ARMED | PA OFF"
                                                                      : "PA CHECK",
                  snapshot.amplifier_disabled_verified ? kGreen : kAmber);
        std::snprintf(buffer, sizeof(buffer),
                      "%lu Hz mono | cue catalog\nRX %lu | TX %lu frames\nErrors %lu/%lu\nCodec idle; PA OFF",
                      static_cast<unsigned long>(snapshot.sample_rate_hz),
                      static_cast<unsigned long>(snapshot.frames_captured),
                      static_cast<unsigned long>(snapshot.frames_played),
                      static_cast<unsigned long>(snapshot.read_errors),
                      static_cast<unsigned long>(snapshot.write_errors));
    }
    lv_label_set_text(audio_detail_, buffer);
}

void Shell::refresh_quick_settings() {
    if (!quick_brightness_) return;
    const auto power = nightglass::services::power_service().snapshot();
    const auto audio = nightglass::services::audio_service().snapshot();
    const auto phone = nightglass::services::connectivity_service().snapshot();
    char text[48]{};
    std::snprintf(text, sizeof(text), "BRIGHTNESS  %u%%", power.settings.active_brightness);
    set_button_text(quick_brightness_, text);
    set_button_text(quick_mute_, audio.settings.muted ? "WATCH SOUND  MUTED"
                                                      : "WATCH SOUND  ON");
    set_button_text(quick_dnd_, audio.settings.do_not_disturb ? "DO NOT DISTURB  ON"
                               : audio.scheduled_dnd ? "QUIET HOURS  ACTIVE"
                                                     : "DO NOT DISTURB  OFF");
    set_button_text(quick_bluetooth_, phone.settings.enabled ? "BLUETOOTH  ON"
                                                             : "BLUETOOTH  OFF");
}

void Shell::refresh_system_overlay() {
    if (!overlay_layer_) return;
    constexpr std::uint8_t kUpdateOverlay = 0xfd;
    constexpr std::uint8_t kPairingOverlay = 0xfe;
    const auto kind = nightglass::services::clock_service().active_alert();
    const auto connectivity = nightglass::services::connectivity_service().snapshot();
    const auto update = nightglass::services::update_transport().snapshot();
    // Alarm and countdown controls are safety-critical. Pairing can wait and
    // reappear after the active alert is dismissed.
    if ((update.awaiting_confirmation || update.ready_to_reboot) &&
        kind == nightglass::services::AlertKind::none) {
        if (alert_card_ && displayed_alert_kind_ == kUpdateOverlay) return;
        lv_obj_clean(overlay_layer_);
        lv_obj_set_style_bg_color(overlay_layer_, lv_color_hex(kVoid), 0);
        lv_obj_set_style_bg_opa(overlay_layer_, LV_OPA_90, 0);
        lv_obj_remove_flag(overlay_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay_layer_);
        alert_card_ = make_route_card(overlay_layer_, 82, 280);
        auto *heading = label(alert_card_, update.ready_to_reboot ? "UPDATE READY" :
                              "INSTALL UPDATE?", &lv_font_montserrat_24,
                              kPrimary);
        lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 12);
        auto *version = label(alert_card_, update.target_version.data(),
                              &lv_font_montserrat_20, kCyan);
        lv_obj_align(version, LV_ALIGN_CENTER, 0, -10);
        if (update.ready_to_reboot) {
            make_button(overlay_layer_, 112, 380, 240, 66, "RESTART NOW",
                        kCyan, kVoid, update_restart_callback, this);
        } else {
            make_button(overlay_layer_, kSafeInset, 380, 170, 66, "CANCEL",
                        kSurface, kPrimary, update_abort_callback, this);
            make_button(overlay_layer_, 212, 380, 170, 66, "INSTALL",
                        kCyan, kVoid, update_confirm_callback, this);
        }
        displayed_alert_kind_ = kUpdateOverlay;
        return;
    }
    if (alert_card_ && displayed_alert_kind_ == kUpdateOverlay) {
        lv_obj_clean(overlay_layer_);
        lv_obj_add_flag(overlay_layer_, LV_OBJ_FLAG_HIDDEN);
        alert_card_ = nullptr;
        displayed_alert_kind_ = 0;
    }
    if (connectivity.pairing_passkey_active &&
        kind == nightglass::services::AlertKind::none) {
        if (alert_card_ && displayed_alert_kind_ == kPairingOverlay) return;
        lv_obj_clean(overlay_layer_);
        lv_obj_set_style_bg_color(overlay_layer_, lv_color_hex(kVoid), 0);
        lv_obj_set_style_bg_opa(overlay_layer_, LV_OPA_90, 0);
        lv_obj_remove_flag(overlay_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay_layer_);
        alert_card_ = make_route_card(overlay_layer_, 112, 250);
        auto *heading = label(alert_card_, "PAIR PHONE", &lv_font_montserrat_26, kPrimary);
        lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 12);
        char code[16]{};
        std::snprintf(code, sizeof(code), "%06lu",
                      static_cast<unsigned long>(connectivity.pairing_passkey));
        auto *passkey = label(alert_card_, code, &lv_font_montserrat_48, kCyan);
        lv_obj_align(passkey, LV_ALIGN_CENTER, 0, 8);
        auto *note = label(overlay_layer_, "Enter this code on the phone",
                           &lv_font_montserrat_16, kSecondary);
        lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -72);
        displayed_alert_kind_ = kPairingOverlay;
        return;
    }
    if (alert_card_ && displayed_alert_kind_ == kPairingOverlay) {
        lv_obj_clean(overlay_layer_);
        lv_obj_add_flag(overlay_layer_, LV_OBJ_FLAG_HIDDEN);
        alert_card_ = nullptr;
        displayed_alert_kind_ = 0;
        displayed_alert_alarm_index_ = nightglass::services::kNoAlarmIndex;
    }
    const auto encoded = static_cast<std::uint8_t>(kind);
    if (kind == nightglass::services::AlertKind::none) {
        if (alert_card_) {
            lv_obj_clean(overlay_layer_);
            lv_obj_add_flag(overlay_layer_, LV_OBJ_FLAG_HIDDEN);
            alert_card_ = nullptr;
            displayed_alert_kind_ = 0;
            displayed_alert_alarm_index_ = nightglass::services::kNoAlarmIndex;
        }
        return;
    }
    const auto clock = nightglass::services::clock_service().snapshot();
    const auto alarm_index = kind == nightglass::services::AlertKind::alarm ||
                                     kind == nightglass::services::AlertKind::both
                                 ? clock.ringing_alarm_index
                                 : nightglass::services::kNoAlarmIndex;
    if (alert_card_ && displayed_alert_kind_ == encoded &&
        displayed_alert_alarm_index_ == alarm_index) return;

    lv_obj_clean(overlay_layer_);
    lv_obj_set_style_bg_color(overlay_layer_, lv_color_hex(kVoid), 0);
    lv_obj_set_style_bg_opa(overlay_layer_, LV_OPA_90, 0);
    lv_obj_remove_flag(overlay_layer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay_layer_);
    const auto &pack = nightglass::services::watchface_service().selected();
    if (nightglass::services::valid_face_pack(pack) &&
        pack.chrome.route_background_asset != nightglass::services::FaceAsset::none) {
        if (const auto *asset = face_asset(pack.chrome.route_background_asset)) {
            auto *background = lv_image_create(overlay_layer_);
            lv_image_set_src(background, asset);
            lv_obj_set_pos(background, 0, 0);
            lv_obj_set_style_image_opa(background, pack.chrome.route_background_opacity, 0);
        }
    }
    alert_card_ = make_route_card(overlay_layer_, 112, 230);
    const char *title = kind == nightglass::services::AlertKind::alarm ? "ALARM"
                        : kind == nightglass::services::AlertKind::countdown ? "TIMER COMPLETE"
                                                                           : "ALARMS DUE";
    auto *heading = label(alert_card_, title, &lv_font_montserrat_32, kPrimary);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 18);
    const char *alarm_label = clock.ringing_alarm_index < clock.alarms.size()
                                  ? clock.alarms[clock.ringing_alarm_index].label.data()
                                  : "Visual + audio alert";
    auto *note = label(alert_card_, kind == nightglass::services::AlertKind::countdown
                                        ? "Countdown complete\nVisual + audio alert"
                                        : alarm_label,
                       &lv_font_montserrat_16, kSecondary);
    lv_obj_align(note, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    if (kind == nightglass::services::AlertKind::alarm ||
        kind == nightglass::services::AlertKind::both) {
        make_button(overlay_layer_, kSafeInset, 350, 170, 70, "SNOOZE 10",
                    kSurface, kPrimary, alarm_snooze_callback, this);
        make_button(overlay_layer_, 212, 350, 170, 70, "DISMISS",
                    kCyan, kVoid, dismiss_alert_callback, this);
    } else {
        make_button(overlay_layer_, kSafeInset, 370, kSafeContentWidth, 70, "DISMISS",
                    kCyan, kVoid, dismiss_alert_callback, this);
    }
    displayed_alert_kind_ = encoded;
    displayed_alert_alarm_index_ = alarm_index;
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
        lv_label_set_text(diagnostics_battery_detail_, "PMIC live | battery not detected");
    } else {
        set_state(diagnostics_battery_state_, battery_age > 5'000'000 ? "STALE" : "LIVE",
                  battery_age > 5'000'000 ? kAmber : kGreen);
        const char *charge = battery.charging ? "Charging"
                             : battery.discharging ? "Discharging"
                                                  : "Charge state partial";
        if (battery.percent_valid && battery.voltage_valid) {
            std::snprintf(buffer, sizeof(buffer), "%u%% | %u.%02u V\n%s", battery.percent,
                          battery.voltage_mv / 1000, (battery.voltage_mv % 1000) / 10, charge);
        } else {
            std::snprintf(buffer, sizeof(buffer), "Battery detected\n%s | metrics partial", charge);
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
                      "Keep watch stationary | %u%%\nGyro zero %u / %u samples\nRestarts %lu",
                      progress, motion.gyro_calibration_samples,
                      motion.gyro_calibration_required,
                      static_cast<unsigned long>(motion.gyro_calibration_restarts));
        lv_label_set_text(diagnostics_motion_detail_, buffer);
    } else {
        set_state(diagnostics_motion_state_, motion_age > 500'000 ? "STALE" : "LIVE",
                  motion_age > 500'000 ? kAmber : kGreen);
        std::snprintf(buffer, sizeof(buffer),
                      "%s | bias %+.1f %+.1f %+.1f\nACC %+.2f  %+.2f  %+.2f g\nGYR %+.1f  %+.1f  %+.1f d/s",
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
        lv_label_set_text(diagnostics_haptic_detail_, "Actuator present | service unavailable");
    }
}

void Shell::clear_route_objects() {
    if (notification_reply_box_) lv_textarea_set_text(notification_reply_box_, "");
    home_time_ = nullptr;
    home_time_state_ = nullptr;
    home_day_ = nullptr;
    home_date_ = nullptr;
    home_battery_ = nullptr;
    home_battery_detail_ = nullptr;
    home_motion_ = nullptr;
    home_steps_ = nullptr;
    diagnostics_rtc_state_ = nullptr;
    diagnostics_rtc_detail_ = nullptr;
    diagnostics_battery_state_ = nullptr;
    diagnostics_battery_detail_ = nullptr;
    diagnostics_motion_state_ = nullptr;
    diagnostics_motion_detail_ = nullptr;
    diagnostics_haptic_state_ = nullptr;
    diagnostics_haptic_detail_ = nullptr;
    setting_active_ = nullptr;
    setting_dim_ = nullptr;
    setting_dim_after_ = nullptr;
    setting_blank_after_ = nullptr;
    setting_sleep_after_ = nullptr;
    setting_time_format_ = nullptr;
    setting_utc_offset_ = nullptr;
    setting_dst_ = nullptr;
    clock_preview_ = nullptr;
    watchface_name_ = nullptr;
    alarm_time_ = nullptr;
    alarm_state_ = nullptr;
    alarm_toggle_ = nullptr;
    alarm_slot_ = nullptr;
    alarm_repeat_ = nullptr;
    alarm_label_ = nullptr;
    quiet_toggle_ = nullptr;
    quiet_start_ = nullptr;
    quiet_end_ = nullptr;
    countdown_time_ = nullptr;
    countdown_duration_ = nullptr;
    countdown_toggle_ = nullptr;
    stopwatch_time_ = nullptr;
    stopwatch_toggle_ = nullptr;
    audio_state_ = nullptr;
    audio_detail_ = nullptr;
    audio_level_ = nullptr;
    audio_volume_ = nullptr;
    audio_mute_ = nullptr;
    audio_dnd_ = nullptr;
    quick_brightness_ = nullptr;
    quick_mute_ = nullptr;
    quick_dnd_ = nullptr;
    quick_bluetooth_ = nullptr;
    home_alarm_ = nullptr;
    home_timer_ = nullptr;
    home_connectivity_ = nullptr;
    home_distance_ = nullptr;
    home_weather_ = nullptr;
    home_weather_icon_ = nullptr;
    home_weather_icon_state_valid_ = false;
    home_notifications_ = nullptr;
    activity_steps_ = nullptr;
    activity_detail_ = nullptr;
    activity_step_length_ = nullptr;
    activity_units_ = nullptr;
    activity_goal_ = nullptr;
    weather_state_ = nullptr;
    weather_detail_ = nullptr;
    weather_toggle_ = nullptr;
    weather_units_ = nullptr;
    weather_refresh_ = nullptr;
    weather_latitude_ = nullptr;
    weather_longitude_ = nullptr;
    connectivity_state_ = nullptr;
    connectivity_detail_ = nullptr;
    connectivity_toggle_ = nullptr;
    phone_battery_ = nullptr;
    phone_call_ = nullptr;
    agenda_items_.fill(nullptr);
    media_state_ = nullptr;
    media_title_ = nullptr;
    media_artist_ = nullptr;
    media_progress_ = nullptr;
    media_time_ = nullptr;
    notification_status_ = nullptr;
    notification_privacy_ = nullptr;
    notification_reply_box_ = nullptr;
    notification_action_count_ = 0;
}

Shell &shell() { return instance; }

}  // namespace nightglass::ui

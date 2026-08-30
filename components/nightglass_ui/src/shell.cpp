#include "nightglass/ui/shell.hpp"

#include <cstdio>
#include <cstdint>

#include "esp_timer.h"
#include "lvgl.h"
#include "nightglass/services/clock.hpp"
#include "nightglass/services/activity.hpp"
#include "nightglass/services/connectivity.hpp"
#include "nightglass/services/network_weather.hpp"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/power.hpp"
#include "nightglass/services/watchface.hpp"
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
    if ((before.state == nightglass::core::PowerState::screen_blank || before.wake_touch_pending) &&
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
    nightglass::services::clock_service().update_clock_settings(settings);
    self->refresh_clock_settings();
}

void Shell::utc_offset_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::clock_service().snapshot().settings;
    settings.utc_offset_minutes = static_cast<std::int16_t>(settings.utc_offset_minutes + 30);
    if (settings.utc_offset_minutes > 14 * 60) settings.utc_offset_minutes = -12 * 60;
    nightglass::services::clock_service().update_clock_settings(settings);
    self->refresh_clock_settings();
}

void Shell::dst_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::clock_service().snapshot().settings;
    settings.daylight_saving = !settings.daylight_saving;
    nightglass::services::clock_service().update_clock_settings(settings);
    self->refresh_clock_settings();
}

void Shell::alarm_hour_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto alarm = nightglass::services::clock_service().snapshot().alarm;
    alarm.hour = static_cast<std::uint8_t>((alarm.hour + 1) % 24);
    nightglass::services::clock_service().update_alarm(alarm);
    self->refresh_alarm();
}

void Shell::alarm_minute_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto alarm = nightglass::services::clock_service().snapshot().alarm;
    alarm.minute = static_cast<std::uint8_t>((alarm.minute + 5) % 60);
    nightglass::services::clock_service().update_alarm(alarm);
    self->refresh_alarm();
}

void Shell::alarm_enabled_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto alarm = nightglass::services::clock_service().snapshot().alarm;
    alarm.enabled = !alarm.enabled;
    nightglass::services::clock_service().update_alarm(alarm);
    self->refresh_alarm();
}

void Shell::countdown_duration_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    const auto snapshot = nightglass::services::clock_service().snapshot();
    constexpr std::uint32_t values[]{60, 300, 600, 900, 1800, 3600};
    nightglass::services::clock_service().set_timer_duration(
        next_value(snapshot.timer_configured_seconds, values));
    self->refresh_countdown();
}

void Shell::countdown_toggle_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    nightglass::services::clock_service().toggle_timer();
    self->refresh_countdown();
}

void Shell::countdown_reset_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    nightglass::services::clock_service().reset_timer();
    self->refresh_countdown();
}

void Shell::stopwatch_toggle_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    nightglass::services::clock_service().toggle_stopwatch();
    self->refresh_stopwatch();
}

void Shell::stopwatch_reset_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    nightglass::services::clock_service().reset_stopwatch();
    self->refresh_stopwatch();
}

void Shell::dismiss_alert_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    nightglass::services::clock_service().dismiss_alerts();
    self->refresh_system_overlay();
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

void Shell::activity_stride_callback(lv_event_t *event) {
    auto *self = static_cast<Shell *>(lv_event_get_user_data(event));
    auto settings = nightglass::services::activity_service().snapshot().settings;
    settings.stride_length_mm = static_cast<std::uint16_t>(settings.stride_length_mm + 50);
    if (settings.stride_length_mm > 1500) settings.stride_length_mm = 300;
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
    nightglass::services::connectivity_service().send_media(command);
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
        case nightglass::core::Route::alarm:
            render_alarm();
            break;
        case nightglass::core::Route::countdown:
            render_countdown();
            break;
        case nightglass::core::Route::stopwatch:
            render_stopwatch();
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
    const auto &pack = nightglass::services::watchface_service().selected();
    if (pack.layout == nightglass::services::FaceLayout::full_background) {
        render_pack_home();
    } else {
        render_classic_home();
    }
    configure_refresh_timer(1000);
    refresh_home();
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
        lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(zone, 0, 0);
        lv_obj_set_style_shadow_width(zone, 0, 0);
        if (slot.action == nightglass::services::FaceAction::open_apps) {
            lv_obj_add_event_cb(zone, launcher_callback, LV_EVENT_CLICKED, this);
        }
    }

    for (std::uint8_t index = 0; index < pack.text_slot_count; ++index) {
        const auto &slot = pack.text_slots[index];
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
            case nightglass::services::FaceField::notifications:
                home_notifications_ = obj;
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
    make_button(scroller, 0, 7 * gap, kSafeContentWidth - 16, row_height, "DIAGNOSTICS",
                kSurface, kPrimary, diagnostics_callback, this);
    make_button(scroller, 0, 8 * gap, kSafeContentWidth - 16, row_height, "ABOUT",
                kSurface, kPrimary, about_callback, this);
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
    make_button(scroller, 0, 450, kSafeContentWidth - 16, 76, "PHONE & MEDIA",
                kSurface, kPrimary, connectivity_callback, this);
    auto *note = label(scroller, "All settings are stored on the watch.",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, 8, 540);
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
    auto *card = make_route_card(content_host_, 112, 140);
    activity_steps_ = label(card, "-- STEPS", &lv_font_montserrat_26, kGreen);
    lv_obj_set_pos(activity_steps_, 0, 4);
    activity_detail_ = label(card, "Calibrating activity sensor", &lv_font_montserrat_16,
                             kSecondary);
    lv_obj_set_pos(activity_detail_, 0, 52);
    lv_obj_set_width(activity_detail_, kSafeContentWidth - 32);
    activity_stride_ = make_button(content_host_, kSafeInset, 270, kSafeContentWidth, 58, "",
                                   kSurface, kPrimary, activity_stride_callback, this);
    activity_goal_ = make_button(content_host_, kSafeInset, 340, kSafeContentWidth, 58, "",
                                 kSurface, kPrimary, activity_goal_callback, this);
    make_button(content_host_, kSafeInset, 410, kSafeContentWidth, 58, "RESET TODAY",
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
    add_header(content_host_, "PHONE & MEDIA", back_callback, this);
    auto *card = make_route_card(content_host_, 112, 145);
    connectivity_state_ = label(card, "BLUETOOTH", &lv_font_montserrat_20, kGreen);
    lv_obj_set_pos(connectivity_state_, 0, 4);
    connectivity_detail_ = label(card, "Starting companion service", &lv_font_montserrat_16,
                                 kSecondary);
    lv_obj_set_pos(connectivity_detail_, 0, 44);
    lv_obj_set_width(connectivity_detail_, kSafeContentWidth - 32);
    lv_label_set_long_mode(connectivity_detail_, LV_LABEL_LONG_MODE_WRAP);
    connectivity_toggle_ = make_button(content_host_, kSafeInset, 274, kSafeContentWidth, 58,
                                       "", kSurface, kPrimary,
                                       connectivity_toggle_callback, this);
    make_button(content_host_, kSafeInset, 346, 108, 58, "PREV", kSurface, kPrimary,
                media_callback,
                reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::previous)));
    make_button(content_host_, 151, 346, 108, 58, "PLAY", kSurface, kPrimary,
                media_callback,
                reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::play_pause)));
    make_button(content_host_, 274, 346, 108, 58, "NEXT", kSurface, kPrimary,
                media_callback,
                reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    nightglass::services::MediaCommand::next)));
    configure_refresh_timer(1000);
    refresh_connectivity();
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
    add_header(content_host_, "ALARM", back_callback, this);
    auto *card = make_route_card(content_host_, 110, 126);
    auto *heading = label(card, "DAILY ALARM", &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(heading, 0, 0);
    alarm_time_ = label(card, "07:00", &lv_font_montserrat_48, kPrimary);
    lv_obj_set_pos(alarm_time_, 0, 27);
    alarm_state_ = label(card, "OFF", &lv_font_montserrat_16, kAmber);
    lv_obj_align(alarm_state_, LV_ALIGN_BOTTOM_RIGHT, 0, -8);

    make_button(content_host_, kSafeInset, 256, 170, 64, "HOUR +", kSurface, kPrimary,
                alarm_hour_callback, this);
    make_button(content_host_, 212, 256, 170, 64, "MIN +5", kSurface, kPrimary,
                alarm_minute_callback, this);
    alarm_toggle_ = make_button(content_host_, kSafeInset, 338, kSafeContentWidth, 70, "",
                                kCyan, kVoid, alarm_enabled_callback, this);
    auto *note = label(content_host_, "Visual alert only | wakes light sleep",
                       &lv_font_montserrat_14, kSecondary);
    lv_obj_set_pos(note, kSafeInset, 430);
    lv_obj_set_width(note, kSafeContentWidth);
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
    auto *note = label(content_host_, "Visual alert only", &lv_font_montserrat_14, kSecondary);
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
        case nightglass::core::Route::alarm:
            refresh_alarm();
            break;
        case nightglass::core::Route::countdown:
            refresh_countdown();
            break;
        case nightglass::core::Route::stopwatch:
            refresh_stopwatch();
            break;
        case nightglass::core::Route::settings:
        case nightglass::core::Route::power_settings:
        case nightglass::core::Route::launcher:
        case nightglass::core::Route::about:
            break;
    }
}

void Shell::refresh_activity() {
    if (!activity_steps_) return;
    const auto snapshot = nightglass::services::activity_service().snapshot();
    char text[96]{};
    std::snprintf(text, sizeof(text), "%lu STEPS", static_cast<unsigned long>(snapshot.steps_today));
    lv_label_set_text(activity_steps_, text);
    std::snprintf(text, sizeof(text), "%lu m | %u%% goal | %s",
                  static_cast<unsigned long>(snapshot.distance_m), snapshot.goal_percent,
                  snapshot.readiness == nightglass::services::ActivityReadiness::ready
                      ? "READY"
                      : snapshot.readiness == nightglass::services::ActivityReadiness::warming_up
                            ? "CALIBRATING"
                            : "UNAVAILABLE");
    lv_label_set_text(activity_detail_, text);
    std::snprintf(text, sizeof(text), "STRIDE  %u.%02u m",
                  snapshot.settings.stride_length_mm / 1000,
                  (snapshot.settings.stride_length_mm % 1000) / 10);
    set_button_text(activity_stride_, text);
    std::snprintf(text, sizeof(text), "DAILY GOAL  %lu",
                  static_cast<unsigned long>(snapshot.settings.daily_goal_steps));
    set_button_text(activity_goal_, text);
}

void Shell::refresh_weather() {
    if (!weather_state_) return;
    const auto snapshot = nightglass::services::network_weather_service().snapshot();
    char text[128]{};
    if (snapshot.data_valid) {
        std::snprintf(text, sizeof(text), "%.0f%s | CODE %u", snapshot.current.temperature,
                      snapshot.settings.units == nightglass::services::WeatherUnits::metric
                          ? " C" : " F", snapshot.current.weather_code);
        set_state(weather_state_, text, snapshot.stale ? kAmber : kGreen);
        std::snprintf(text, sizeof(text), "FEELS %.0f | WIND %.0f | AGE %lu min",
                      snapshot.current.apparent_temperature, snapshot.current.wind_speed,
                      static_cast<unsigned long>(snapshot.age_seconds / 60));
    } else {
        set_state(weather_state_, snapshot.connected ? "WEATHER UNAVAILABLE" : "OFFLINE", kAmber);
        std::snprintf(text, sizeof(text), "%s | %s",
                      snapshot.credentials_configured ? "Wi-Fi active this boot" : "Wi-Fi setup needed",
                      snapshot.settings.location_configured ? "location set" : "location needed");
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
}

void Shell::refresh_home() {
    if (!home_time_) return;
    const auto snapshot = nightglass::services::hardware_service().snapshot();
    const auto clock = nightglass::services::clock_service().snapshot();
    const auto now = esp_timer_get_time();
    const auto &pack = nightglass::services::watchface_service().selected();
    const bool full_background = pack.layout ==
                                 nightglass::services::FaceLayout::full_background;
    char buffer[96]{};

    if (clock.time_valid) {
        const char *period = "";
        format_time(buffer, sizeof(buffer), clock.local_time,
                    clock.settings.use_24_hour, &period);
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
            std::snprintf(buffer, sizeof(buffer), "%s%sRTC LIVE",
                          period, period[0] ? " | " : "");
            set_state(home_time_state_, buffer, pack.palette.accent);
        } else {
            std::snprintf(buffer, sizeof(buffer), "%s%sUTC%c%d:%02d | LIVE",
                          period, period[0] ? " | " : "", effective_offset < 0 ? '-' : '+',
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
            std::snprintf(buffer, sizeof(buffer), "%.2f KM", activity.distance_m / 1000.0);
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
    if (home_notifications_) {
        std::snprintf(buffer, sizeof(buffer), "%u NEW", connectivity.notification_count);
        set_state(home_notifications_, buffer,
                  connectivity.notification_count ? pack.palette.accent : pack.palette.secondary);
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
        const auto &alarm = clock.alarm;
        if (alarm.enabled) {
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
    nightglass::services::CivilTime alarm_time{};
    alarm_time.hour = snapshot.alarm.hour;
    alarm_time.minute = snapshot.alarm.minute;
    char buffer[64]{};
    const char *period = "";
    format_time(buffer, sizeof(buffer), alarm_time, snapshot.settings.use_24_hour, &period);
    char display[64]{};
    std::snprintf(display, sizeof(display), "%s%s%s", buffer, period[0] ? " " : "", period);
    lv_label_set_text(alarm_time_, display);
    set_state(alarm_state_, snapshot.alarm_ringing ? "RINGING"
                           : snapshot.alarm.enabled ? "ON" : "OFF",
              snapshot.alarm_ringing ? kRed : snapshot.alarm.enabled ? kGreen : kAmber);
    if (alarm_toggle_) {
        set_button_text(alarm_toggle_, snapshot.alarm.enabled ? "DISABLE" : "ENABLE");
    }
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

void Shell::refresh_system_overlay() {
    if (!overlay_layer_) return;
    const auto kind = nightglass::services::clock_service().active_alert();
    const auto encoded = static_cast<std::uint8_t>(kind);
    if (kind == nightglass::services::AlertKind::none) {
        if (alert_card_) {
            lv_obj_clean(overlay_layer_);
            lv_obj_add_flag(overlay_layer_, LV_OBJ_FLAG_HIDDEN);
            alert_card_ = nullptr;
            displayed_alert_kind_ = 0;
        }
        return;
    }
    if (alert_card_ && displayed_alert_kind_ == encoded) return;

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
    auto *note = label(alert_card_, "Visual alert\nAudio and haptics unavailable",
                       &lv_font_montserrat_16, kSecondary);
    lv_obj_align(note, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    make_button(overlay_layer_, kSafeInset, 370, kSafeContentWidth, 70, "DISMISS",
                kCyan, kVoid, dismiss_alert_callback, this);
    displayed_alert_kind_ = encoded;
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
    countdown_time_ = nullptr;
    countdown_duration_ = nullptr;
    countdown_toggle_ = nullptr;
    stopwatch_time_ = nullptr;
    stopwatch_toggle_ = nullptr;
    home_alarm_ = nullptr;
    home_timer_ = nullptr;
    home_distance_ = nullptr;
    home_weather_ = nullptr;
    home_notifications_ = nullptr;
    activity_steps_ = nullptr;
    activity_detail_ = nullptr;
    activity_stride_ = nullptr;
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
}

Shell &shell() { return instance; }

}  // namespace nightglass::ui

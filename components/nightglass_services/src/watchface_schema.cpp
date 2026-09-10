#include "nightglass/services/watchface.hpp"

namespace nightglass::services {

namespace {
constexpr std::uint16_t kCanvasWidth = 410;
constexpr std::uint16_t kCanvasHeight = 502;
constexpr std::uint16_t kTextSafeInset = 28;
constexpr std::uint16_t kKnownComplications =
    complication_time | complication_date | complication_battery |
    complication_motion | complication_steps | complication_alarm |
    complication_timer | complication_distance | complication_weather |
    complication_notifications | complication_connectivity;
constexpr std::uint8_t kMaxTextSlots = 24;
constexpr std::uint8_t kMaxActionSlots = 12;
constexpr std::uint8_t kMaxRouteBackgroundOpacity = 64;
constexpr std::uint16_t kMinimumTouchSize = 48;

bool valid_rect(const FaceRect &rect) {
    if (rect.x < 0 || rect.y < 0 || rect.width == 0 || rect.height == 0) return false;
    return static_cast<std::uint32_t>(rect.x) + rect.width <= kCanvasWidth &&
           static_cast<std::uint32_t>(rect.y) + rect.height <= kCanvasHeight;
}

bool valid_field(FaceField field) {
    return field >= FaceField::fixed_text && field <= FaceField::connectivity;
}

bool valid_style(FaceTextStyle style) {
    return style >= FaceTextStyle::caption_14 && style <= FaceTextStyle::time_48;
}

bool valid_align(FaceTextAlign align) {
    return align >= FaceTextAlign::left && align <= FaceTextAlign::right;
}

bool valid_color(FaceColorRole color) {
    return color >= FaceColorRole::primary && color <= FaceColorRole::accent_dim;
}

std::uint16_t field_complication(FaceField field) {
    switch (field) {
        case FaceField::time:
        case FaceField::time_state:
            return complication_time;
        case FaceField::day:
        case FaceField::date:
            return complication_date;
        case FaceField::battery:
        case FaceField::battery_detail:
            return complication_battery;
        case FaceField::steps_icon:
        case FaceField::steps:
            return complication_steps;
        case FaceField::motion:
            return complication_motion;
        case FaceField::alarm:
            return complication_alarm;
        case FaceField::timer:
            return complication_timer;
        case FaceField::distance:
            return complication_distance;
        case FaceField::weather:
        case FaceField::weather_icon:
            return complication_weather;
        case FaceField::notifications:
            return complication_notifications;
        case FaceField::connectivity:
            return complication_connectivity;
        case FaceField::fixed_text:
            return 0;
    }
    return kKnownComplications + 1;
}

bool valid_text_slot(const FaceTextSlot &slot, std::uint16_t complications) {
    if (!valid_field(slot.field) || !valid_style(slot.style) ||
        !valid_align(slot.align) || !valid_color(slot.color)) {
        return false;
    }
    if (!valid_rect(slot.bounds)) return false;
    if (slot.bounds.x < kTextSafeInset || slot.bounds.y < kTextSafeInset ||
        static_cast<std::uint32_t>(slot.bounds.x) + slot.bounds.width >
            kCanvasWidth - kTextSafeInset ||
        static_cast<std::uint32_t>(slot.bounds.y) + slot.bounds.height >
            kCanvasHeight - kTextSafeInset) {
        return false;
    }
    if (slot.field == FaceField::fixed_text) return slot.fixed_text && slot.fixed_text[0];
    return slot.fixed_text == nullptr &&
           (complications & field_complication(slot.field)) != 0;
}

bool valid_chrome(const FaceChrome &chrome) {
    switch (chrome.theme) {
        case ChromeTheme::classic:
            return chrome.route_background_asset == FaceAsset::none &&
                   chrome.route_background_opacity == 0;
        case ChromeTheme::revenant:
            return (chrome.route_background_asset == FaceAsset::revenant_grid_v2 ||
                    chrome.route_background_asset == FaceAsset::revenant_shell_v1) &&
                   chrome.route_background_opacity > 0 &&
                   chrome.route_background_opacity <= kMaxRouteBackgroundOpacity;
    }
    return false;
}

bool valid_action(FaceAction action) {
    return action >= FaceAction::open_apps && action <= FaceAction::open_openclaw;
}

bool overlaps(const FaceRect &left, const FaceRect &right) {
    const auto left_right = static_cast<std::int32_t>(left.x) + left.width;
    const auto right_right = static_cast<std::int32_t>(right.x) + right.width;
    const auto left_bottom = static_cast<std::int32_t>(left.y) + left.height;
    const auto right_bottom = static_cast<std::int32_t>(right.y) + right.height;
    return left.x < right_right && right.x < left_right &&
           left.y < right_bottom && right.y < left_bottom;
}

bool valid_action_slot(const FaceActionSlot &slot) {
    return valid_action(slot.action) && valid_rect(slot.bounds) &&
           slot.bounds.width >= kMinimumTouchSize &&
           slot.bounds.height >= kMinimumTouchSize &&
           slot.bounds.x >= kTextSafeInset && slot.bounds.y >= kTextSafeInset &&
           static_cast<std::uint32_t>(slot.bounds.x) + slot.bounds.width <=
               kCanvasWidth - kTextSafeInset &&
           static_cast<std::uint32_t>(slot.bounds.y) + slot.bounds.height <=
               kCanvasHeight - kTextSafeInset;
}
}  // namespace

bool valid_face_pack(const FacePack &pack) {
    if (!pack.slug || !pack.slug[0] || !pack.name || !pack.name[0] ||
        pack.format_version != 5 || (pack.complications & ~kKnownComplications) != 0 ||
        !valid_chrome(pack.chrome)) {
        return false;
    }

    if (pack.layout == FaceLayout::classic) {
        return pack.id == 0 && pack.background_asset == FaceAsset::none &&
               pack.chrome.theme == ChromeTheme::classic &&
               pack.text_slots == nullptr && pack.text_slot_count == 0 &&
               pack.action_slots == nullptr && pack.action_slot_count == 0;
    }
    if (pack.id == 0 || pack.layout != FaceLayout::full_background ||
        pack.background_asset != FaceAsset::revenant_grid_v2 ||
        !pack.text_slots || pack.text_slot_count == 0 ||
        pack.text_slot_count > kMaxTextSlots || !pack.action_slots ||
        pack.action_slot_count == 0 || pack.action_slot_count > kMaxActionSlots) {
        return false;
    }
    for (std::uint8_t index = 0; index < pack.text_slot_count; ++index) {
        if (!valid_text_slot(pack.text_slots[index], pack.complications)) return false;
    }
    for (std::uint8_t index = 0; index < pack.action_slot_count; ++index) {
        if (!valid_action_slot(pack.action_slots[index])) return false;
        for (std::uint8_t other = 0; other < index; ++other) {
            // Slots are evaluated in declaration order, but overlap is rejected
            // so a pack can never hide a higher-priority action underneath one.
            if (overlaps(pack.action_slots[index].bounds,
                         pack.action_slots[other].bounds)) return false;
        }
    }
    return true;
}

}  // namespace nightglass::services

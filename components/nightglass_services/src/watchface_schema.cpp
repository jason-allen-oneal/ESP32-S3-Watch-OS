#include "nightglass/services/watchface.hpp"

namespace nightglass::services {

namespace {
constexpr std::uint16_t kCanvasWidth = 410;
constexpr std::uint16_t kCanvasHeight = 502;
constexpr std::uint16_t kTextSafeInset = 28;
constexpr std::uint16_t kKnownComplications =
    complication_time | complication_date | complication_battery |
    complication_motion | complication_steps | complication_alarm |
    complication_timer;
constexpr std::uint8_t kMaxTextSlots = 24;
constexpr std::uint8_t kMaxActionSlots = 4;
constexpr std::uint8_t kMaxRouteBackgroundOpacity = 64;

bool valid_rect(const FaceRect &rect) {
    if (rect.x < 0 || rect.y < 0 || rect.width == 0 || rect.height == 0) return false;
    return static_cast<std::uint32_t>(rect.x) + rect.width <= kCanvasWidth &&
           static_cast<std::uint32_t>(rect.y) + rect.height <= kCanvasHeight;
}

bool valid_field(FaceField field) {
    return field >= FaceField::fixed_text && field <= FaceField::timer;
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
        case FaceField::steps:
            return complication_steps;
        case FaceField::motion:
            return complication_motion;
        case FaceField::alarm:
            return complication_alarm;
        case FaceField::timer:
            return complication_timer;
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
            return chrome.route_background_asset == FaceAsset::revenant_grid_v2 &&
                   chrome.route_background_opacity > 0 &&
                   chrome.route_background_opacity <= kMaxRouteBackgroundOpacity;
    }
    return false;
}
}  // namespace

bool valid_face_pack(const FacePack &pack) {
    if (!pack.slug || !pack.slug[0] || !pack.name || !pack.name[0] ||
        pack.format_version != 3 || (pack.complications & ~kKnownComplications) != 0 ||
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
        if (!valid_rect(pack.action_slots[index].bounds) ||
            pack.action_slots[index].action != FaceAction::open_apps) {
            return false;
        }
    }
    return true;
}

}  // namespace nightglass::services

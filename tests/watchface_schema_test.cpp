#include <cassert>

#include "nightglass/services/watchface.hpp"

using namespace nightglass::services;

namespace {
constexpr FacePalette kPalette{0, 0, 0, 0, 0, 0, 0};
constexpr FaceTextSlot kText[]{
    {FaceField::fixed_text, {28, 28, 80, 20}, FaceTextStyle::caption_14,
     FaceTextAlign::left, FaceColorRole::primary, "SAFE"},
    {FaceField::time, {40, 200, 330, 60}, FaceTextStyle::time_48,
     FaceTextAlign::center, FaceColorRole::accent, nullptr},
    {FaceField::weather_icon, {300, 100, 50, 50}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, nullptr},
    {FaceField::steps_icon, {50, 100, 50, 24}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, nullptr},
};
constexpr FaceActionSlot kActions[]{
    {FaceAction::open_apps, {28, 400, 100, 74}},
    {FaceAction::open_activity, {140, 400, 100, 74}},
};

FacePack full_pack() {
    return {1,
            "test",
            "Test",
            5,
            FaceLayout::full_background,
            complication_time | complication_weather | complication_steps,
            kPalette,
            FaceAsset::revenant_grid_v2,
            {ChromeTheme::revenant, FaceAsset::revenant_grid_v2, 36},
            kText,
            4,
            kActions,
            2};
}
}  // namespace

int main() {
    const FacePack classic{0, "classic", "Classic", 5, FaceLayout::classic,
                           complication_time, kPalette, FaceAsset::none,
                           {ChromeTheme::classic, FaceAsset::none, 0}, nullptr, 0,
                           nullptr, 0};
    assert(valid_face_pack(classic));

    auto pack = full_pack();
    assert(valid_face_pack(pack));

    pack.complications &= ~complication_steps;
    assert(!valid_face_pack(pack));

    pack = full_pack();
    pack.format_version = 99;
    assert(!valid_face_pack(pack));
    pack = full_pack();
    pack.background_asset = FaceAsset::none;
    assert(!valid_face_pack(pack));
    pack = full_pack();
    pack.chrome.theme = static_cast<ChromeTheme>(99);
    assert(!valid_face_pack(pack));
    pack = full_pack();
    pack.chrome.route_background_asset = FaceAsset::none;
    assert(!valid_face_pack(pack));
    pack = full_pack();
    pack.chrome.route_background_opacity = 65;
    assert(!valid_face_pack(pack));
    pack = full_pack();
    pack.chrome.route_background_opacity = 0;
    assert(!valid_face_pack(pack));

    constexpr FaceTextSlot unsafe_text[]{
        {FaceField::time, {10, 20, 100, 30}, FaceTextStyle::value_20,
         FaceTextAlign::left, FaceColorRole::primary, nullptr},
    };
    pack = full_pack();
    pack.text_slots = unsafe_text;
    pack.text_slot_count = 1;
    assert(!valid_face_pack(pack));

    constexpr FaceTextSlot executable_like_text[]{
        {FaceField::time, {40, 40, 100, 30}, FaceTextStyle::value_20,
         FaceTextAlign::left, FaceColorRole::primary, "not allowed"},
    };
    pack.text_slots = executable_like_text;
    assert(!valid_face_pack(pack));

    constexpr FaceActionSlot overlapping_actions[]{
        {FaceAction::open_apps, {28, 400, 100, 74}},
        {FaceAction::open_weather, {100, 400, 100, 74}},
    };
    pack = full_pack();
    pack.action_slots = overlapping_actions;
    assert(!valid_face_pack(pack));

    constexpr FaceActionSlot unsafe_action[]{
        {FaceAction::open_apps, {0, 0, 100, 100}},
    };
    pack = full_pack();
    pack.action_slots = unsafe_action;
    pack.action_slot_count = 1;
    assert(!valid_face_pack(pack));

    constexpr FaceActionSlot too_small_action[]{
        {FaceAction::open_apps, {28, 400, 47, 74}},
    };
    pack = full_pack();
    pack.action_slots = too_small_action;
    pack.action_slot_count = 1;
    assert(!valid_face_pack(pack));

    constexpr FaceActionSlot invalid_action[]{
        {static_cast<FaceAction>(99), {28, 400, 100, 74}},
    };
    pack = full_pack();
    pack.action_slots = invalid_action;
    pack.action_slot_count = 1;
    assert(!valid_face_pack(pack));
}

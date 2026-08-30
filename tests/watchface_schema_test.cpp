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
};
constexpr FaceActionSlot kActions[]{{FaceAction::open_apps, {0, 0, 410, 502}}};

FacePack full_pack() {
    return {1,
            "test",
            "Test",
            3,
            FaceLayout::full_background,
            complication_time,
            kPalette,
            FaceAsset::revenant_grid_v2,
            {ChromeTheme::revenant, FaceAsset::revenant_grid_v2, 36},
            kText,
            2,
            kActions,
            1};
}
}  // namespace

int main() {
    const FacePack classic{0, "classic", "Classic", 3, FaceLayout::classic,
                           complication_time, kPalette, FaceAsset::none,
                           {ChromeTheme::classic, FaceAsset::none, 0}, nullptr, 0,
                           nullptr, 0};
    assert(valid_face_pack(classic));

    auto pack = full_pack();
    assert(valid_face_pack(pack));

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
}

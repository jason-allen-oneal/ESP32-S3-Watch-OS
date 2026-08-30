#include "nightglass/services/watchface.hpp"

#include <iterator>

#include "esp_log.h"
#include "nvs.h"

namespace nightglass::services {

namespace {
constexpr char kTag[] = "nightglass_face";
constexpr char kNamespace[] = "ng_face";
constexpr char kSelectedKey[] = "selected";

constexpr FaceTextSlot kRevenantTextSlots[]{
    {FaceField::battery, {145, 28, 120, 24}, FaceTextStyle::value_20,
     FaceTextAlign::center, FaceColorRole::accent, nullptr},
    {FaceField::fixed_text, {160, 51, 90, 18}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::secondary, "BATTERY"},
    {FaceField::fixed_text, {44, 92, 68, 18}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, "STEPS"},
    {FaceField::steps, {44, 113, 68, 42}, FaceTextStyle::body_16,
     FaceTextAlign::center, FaceColorRole::primary, nullptr},
    {FaceField::fixed_text, {298, 92, 68, 18}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, "ACTIVITY"},
    {FaceField::motion, {298, 113, 68, 42}, FaceTextStyle::body_16,
     FaceTextAlign::center, FaceColorRole::primary, nullptr},
    {FaceField::fixed_text, {42, 198, 108, 18}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, "POWER"},
    {FaceField::battery_detail, {42, 217, 108, 30}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::primary, nullptr},
    {FaceField::fixed_text, {260, 198, 108, 18}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, "RTC"},
    {FaceField::time_state, {260, 217, 108, 30}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::primary, nullptr},
    {FaceField::time, {43, 272, 324, 58}, FaceTextStyle::time_48,
     FaceTextAlign::center, FaceColorRole::accent, nullptr},
    {FaceField::day, {45, 352, 112, 19}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, nullptr},
    {FaceField::date, {253, 352, 112, 19}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, nullptr},
    {FaceField::fixed_text, {38, 395, 88, 18}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, "ALARM"},
    {FaceField::alarm, {38, 417, 88, 38}, FaceTextStyle::body_16,
     FaceTextAlign::center, FaceColorRole::primary, nullptr},
    {FaceField::fixed_text, {284, 395, 88, 18}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, "TIMER"},
    {FaceField::timer, {284, 417, 88, 38}, FaceTextStyle::body_16,
     FaceTextAlign::center, FaceColorRole::primary, nullptr},
    {FaceField::fixed_text, {167, 455, 76, 18}, FaceTextStyle::caption_14,
     FaceTextAlign::center, FaceColorRole::accent, "APPS"},
};

constexpr FaceActionSlot kRevenantActions[]{
    {FaceAction::open_apps, {0, 0, 410, 502}},
};

constexpr FacePack kPacks[]{
    {0, "classic", "Nightglass Classic", 3, FaceLayout::classic,
     complication_time | complication_date | complication_battery |
         complication_motion,
     {0x000000, 0x0B0D12, 0x272D3A, 0xF4F6FA, 0xA2ABBA, 0x63DDE4, 0x274C55},
     FaceAsset::none, {ChromeTheme::classic, FaceAsset::none, 0}, nullptr, 0,
     nullptr, 0},
    {1, "revenant-grid-v2", "Revenant Grid v2", 3, FaceLayout::full_background,
     complication_time | complication_date | complication_battery |
         complication_motion | complication_steps | complication_alarm |
         complication_timer,
     {0x000000, 0x101413, 0x252D29, 0xF3F7F4, 0x829087, 0xA8FF32, 0x0B3A24},
     FaceAsset::revenant_grid_v2,
     {ChromeTheme::revenant, FaceAsset::revenant_grid_v2, 36}, kRevenantTextSlots,
     static_cast<std::uint8_t>(std::size(kRevenantTextSlots)), kRevenantActions,
     static_cast<std::uint8_t>(std::size(kRevenantActions))},
};

WatchFaceService instance;
std::uint8_t selected_id = 1;

const FacePack *find_pack(std::uint8_t id) {
    for (const auto &pack : kPacks) {
        if (pack.id == id && valid_face_pack(pack)) return &pack;
    }
    return nullptr;
}
}  // namespace

nightglass::core::Status WatchFaceService::start() {
    if (!valid_face_pack(kPacks[0])) {
        ESP_LOGE(kTag, "Compiled Classic fallback is invalid");
        return {nightglass::core::StatusCode::invalid_state, "watch-face fallback invalid"};
    }
    selected_id = 1;
    nvs_handle_t handle{};
    const auto opened = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (opened == ESP_OK) {
        std::uint8_t stored{};
        if (nvs_get_u8(handle, kSelectedKey, &stored) == ESP_OK) {
            selected_id = find_pack(stored) ? stored : kPacks[0].id;
        }
        nvs_close(handle);
    } else if (opened != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "Face selection unavailable: %s", esp_err_to_name(opened));
        return {nightglass::core::StatusCode::degraded, "face selection persistence unavailable"};
    }
    ESP_LOGI(kTag, "Selected face: %s", selected().slug);
    return nightglass::core::Status::Ok();
}

const FacePack &WatchFaceService::selected() const {
    const auto *pack = find_pack(selected_id);
    return pack ? *pack : kPacks[0];
}

const FacePack *WatchFaceService::packs() const { return kPacks; }

std::size_t WatchFaceService::pack_count() const { return std::size(kPacks); }

bool WatchFaceService::select(std::uint8_t id) {
    if (!find_pack(id)) return false;
    nvs_handle_t handle{};
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const auto written = nvs_set_u8(handle, kSelectedKey, id);
    const auto committed = written == ESP_OK ? nvs_commit(handle) : written;
    nvs_close(handle);
    if (committed != ESP_OK) return false;
    selected_id = id;
    ESP_LOGI(kTag, "Selected face: %s", selected().slug);
    return true;
}

WatchFaceService &watchface_service() { return instance; }

}  // namespace nightglass::services

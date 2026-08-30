#include "nightglass/services/watchface.hpp"

#include <iterator>

#include "esp_log.h"
#include "nvs.h"

namespace nightglass::services {

namespace {
constexpr char kTag[] = "nightglass_face";
constexpr char kNamespace[] = "ng_face";
constexpr char kSelectedKey[] = "selected";

constexpr FacePack kPacks[]{
    {0, "classic", "Nightglass Classic", 1, FaceLayout::classic,
     complication_date | complication_battery | complication_motion,
     {0x000000, 0x0B0D12, 0x272D3A, 0xF4F6FA, 0xA2ABBA, 0x63DDE4, 0x274C55}},
    {1, "revenant-grid", "Revenant Grid", 1, FaceLayout::revenant_grid,
     complication_date | complication_battery | complication_motion |
         complication_alarm | complication_timer,
     {0x000000, 0x101413, 0x252D29, 0xF3F7F4, 0x829087, 0xA8FF32, 0x0B3A24}},
};

WatchFaceService instance;
std::uint8_t selected_id = 1;

const FacePack *find_pack(std::uint8_t id) {
    for (const auto &pack : kPacks) {
        if (pack.id == id) return &pack;
    }
    return nullptr;
}
}  // namespace

nightglass::core::Status WatchFaceService::start() {
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

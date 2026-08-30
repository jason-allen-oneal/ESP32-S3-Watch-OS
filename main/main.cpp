#include "esp_log.h"
#include "nvs_flash.h"

#include "morrow/bsp/board.hpp"
#include "morrow/core/health.hpp"
#include "morrow/ui/shell.hpp"

namespace {
constexpr char kTag[] = "morrow_boot";
}

extern "C" void app_main() {
    ESP_LOGI(kTag, "MorrowOS foundation boot");

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
    morrow::core::health_registry().set("nvs", morrow::core::HealthState::ok,
                                       "NVS initialized");

    auto &board = morrow::bsp::board();
    const auto board_status = board.start_essential();
    if (!board_status.is_ok()) {
        ESP_LOGE(kTag, "Essential board startup failed: %s", board_status.detail);
        return;
    }

    if (!board.lock_display(0)) {
        ESP_LOGE(kTag, "Unable to lock LVGL display");
        return;
    }
    const auto ui_status = morrow::ui::shell().start();
    board.unlock_display();

    if (!ui_status.is_ok()) {
        ESP_LOGE(kTag, "System shell startup failed: %s", ui_status.detail);
        return;
    }
    morrow::core::health_registry().set("ui", morrow::core::HealthState::ok,
                                       "Foundation shell active");
    ESP_LOGI(kTag, "Foundation health gate active");
}

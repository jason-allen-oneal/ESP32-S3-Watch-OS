#include "esp_log.h"
#include "nvs_flash.h"

#include "morrow/bsp/board.hpp"
#include "morrow/core/health.hpp"
#include "morrow/services/hardware.hpp"
#include "morrow/ui/shell.hpp"

namespace {
constexpr char kTag[] = "morrow_boot";
}

extern "C" void app_main() {
    ESP_LOGI(kTag, "MorrowOS Milestone 2 diagnostics boot");

    auto &board = morrow::bsp::board();
    const auto safe_output_status = board.prepare_safe_outputs();
    if (!safe_output_status.is_ok()) {
        ESP_LOGE(kTag, "Early output fail-safe failed: %s", safe_output_status.detail);
    }

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result != ESP_OK) {
        // Settings, alarms, and pairing state will become authoritative here.
        // Never destroy them as an automatic recovery side effect.
        ESP_LOGE(kTag, "NVS initialization failed without erase: %s", esp_err_to_name(nvs_result));
        morrow::core::health_registry().set("nvs", morrow::core::HealthState::failed,
                                           "NVS unavailable; retained without erase");
    } else {
        morrow::core::health_registry().set("nvs", morrow::core::HealthState::ok,
                                           "NVS initialized");
    }

    const auto board_status = board.start_essential();
    if (!board_status.is_ok()) {
        ESP_LOGE(kTag, "Essential board startup failed: %s", board_status.detail);
        return;
    }

    const auto hardware_status = morrow::services::hardware_service().start(board.i2c_bus());
    if (!hardware_status.is_ok()) {
        ESP_LOGW(kTag, "Hardware services degraded: %s", hardware_status.detail);
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
                                       "Hardware diagnostics shell active");
    ESP_LOGI(kTag, "Milestone 2 hardware diagnostics active");
}

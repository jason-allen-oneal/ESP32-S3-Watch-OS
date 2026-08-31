#include "esp_log.h"
#include "nvs_flash.h"

#include "nightglass/bsp/board.hpp"
#include "nightglass/core/health.hpp"
#include "nightglass/services/activity.hpp"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/clock.hpp"
#include "nightglass/services/connectivity.hpp"
#include "nightglass/services/network_weather.hpp"
#include "nightglass/services/power.hpp"
#include "nightglass/services/watchface.hpp"
#include "nightglass/services/audio.hpp"
#include "nightglass/ui/shell.hpp"

namespace {
constexpr char kTag[] = "nightglass_boot";
}

extern "C" void app_main() {
    ESP_LOGI(kTag, "Nightglass shell and power-policy boot");

    auto &board = nightglass::bsp::board();
    const auto safe_output_status = board.prepare_safe_outputs();
    if (!safe_output_status.is_ok()) {
        ESP_LOGE(kTag, "Early output fail-safe failed: %s", safe_output_status.detail);
    }

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result != ESP_OK) {
        // Settings, alarms, and pairing state will become authoritative here.
        // Never destroy them as an automatic recovery side effect.
        ESP_LOGE(kTag, "NVS initialization failed without erase: %s", esp_err_to_name(nvs_result));
        nightglass::core::health_registry().set("nvs", nightglass::core::HealthState::failed,
                                           "NVS unavailable; retained without erase");
    } else {
        nightglass::core::health_registry().set("nvs", nightglass::core::HealthState::ok,
                                           "NVS initialized");
    }

    const auto board_status = board.start_essential();
    if (!board_status.is_ok()) {
        ESP_LOGE(kTag, "Essential board startup failed: %s", board_status.detail);
        return;
    }

    const auto hardware_status = nightglass::services::hardware_service().start(board.i2c_bus());
    if (!hardware_status.is_ok()) {
        ESP_LOGW(kTag, "Hardware services degraded: %s", hardware_status.detail);
    }

    const auto audio_status = nightglass::services::audio_service().start(board.i2c_bus());
    if (!audio_status.is_ok()) {
        ESP_LOGI(kTag, "Audio unavailable by policy: %s", audio_status.detail);
    }

    const auto power_status = nightglass::services::power_service().start();
    if (!power_status.is_ok()) {
        ESP_LOGW(kTag, "Power service degraded: %s", power_status.detail);
    }

    const auto clock_status = nightglass::services::clock_service().start();
    if (!clock_status.is_ok()) {
        ESP_LOGW(kTag, "Clock service degraded: %s", clock_status.detail);
    }

    const auto activity_status = nightglass::services::activity_service().start();
    if (!activity_status.is_ok()) {
        ESP_LOGW(kTag, "Activity service degraded: %s", activity_status.detail);
    }

    const auto face_status = nightglass::services::watchface_service().start();
    if (!face_status.is_ok()) {
        ESP_LOGW(kTag, "Watch face service degraded: %s", face_status.detail);
    }

    const auto connectivity_status = nightglass::services::connectivity_service().start();
    if (!connectivity_status.is_ok()) {
        ESP_LOGW(kTag, "Connectivity service degraded: %s", connectivity_status.detail);
    }

    const auto network_status = nightglass::services::network_weather_service().start();
    if (!network_status.is_ok()) {
        ESP_LOGW(kTag, "Network/weather service degraded: %s", network_status.detail);
    }

    if (!board.lock_display(0)) {
        ESP_LOGE(kTag, "Unable to lock LVGL display");
        return;
    }
    const auto ui_status = nightglass::ui::shell().start();
    board.unlock_display();

    if (!ui_status.is_ok()) {
        ESP_LOGE(kTag, "System shell startup failed: %s", ui_status.detail);
        return;
    }
    nightglass::core::health_registry().set("ui", nightglass::core::HealthState::ok,
                                       "Daily watch shell active");
    ESP_LOGI(kTag, "Nightglass daily shell active");
}

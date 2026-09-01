#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_NIGHTGLASS_AUDIO_BOOT_SELF_TEST
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "nightglass/bsp/board.hpp"
#include "nightglass/core/health.hpp"
#include "nightglass/services/activity.hpp"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/clock.hpp"
#include "nightglass/services/connectivity.hpp"
#include "nightglass/services/network_weather.hpp"
#include "nightglass/services/power.hpp"
#include "nightglass/services/storage.hpp"
#include "nightglass/services/watchface.hpp"
#include "nightglass/services/audio.hpp"
#include "nightglass/ui/shell.hpp"
#include "nightglass/update/service.hpp"
#include "nightglass/update/verifier.hpp"

namespace {
constexpr char kTag[] = "nightglass_boot";

#if CONFIG_NIGHTGLASS_AUDIO_BOOT_SELF_TEST
bool wait_for_audio_completion(std::uint32_t initial_sequence,
                               nightglass::services::AudioSnapshot &snapshot) {
    constexpr std::uint32_t kPollMs = 20;
    constexpr std::uint32_t kTimeoutMs = 3000;
    for (std::uint32_t waited = 0; waited < kTimeoutMs; waited += kPollMs) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        snapshot = nightglass::services::audio_service().snapshot();
        if (!snapshot.operation_pending && snapshot.sequence > initial_sequence &&
            snapshot.last_io_us > 0) {
            return true;
        }
    }
    return false;
}

void run_audio_boot_self_test() {
    auto &audio = nightglass::services::audio_service();
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(kTag, "AUDIO_HIL heap internal_free=%lu internal_largest=%lu dma_free=%lu dma_largest=%lu",
             static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));

    auto before = audio.snapshot();
    auto request_status = audio.request_microphone_sample();
    nightglass::services::AudioSnapshot capture{};
    const bool capture_completed = request_status.is_ok() &&
                                   wait_for_audio_completion(before.sequence, capture);
    const bool capture_ok = capture_completed && capture.last_operation_ok &&
                            capture.last_transfer_bytes == 2048 &&
                            capture.last_capture_rms > 0 &&
                            capture.amplifier_disabled_verified;
    ESP_LOGI(kTag,
             "AUDIO_HIL capture ok=%u bytes=%lu rms=%lu pa_off=%u status=%u",
             capture_ok, static_cast<unsigned long>(capture.last_transfer_bytes),
             static_cast<unsigned long>(capture.last_capture_rms),
             capture.amplifier_disabled_verified, capture.last_operation_status);

    before = audio.snapshot();
    request_status = audio.request_sound(nightglass::services::SoundCue::notification);
    nightglass::services::AudioSnapshot playback{};
    const bool playback_completed = request_status.is_ok() &&
                                    wait_for_audio_completion(before.sequence, playback);
    const bool playback_ok = playback_completed && playback.last_operation_ok &&
                             playback.last_transfer_bytes ==
                                 nightglass::services::sound_cue_transfer_bytes(
                                     nightglass::services::SoundCue::notification) &&
                             playback.amplifier_disabled_verified;
    ESP_LOGI(kTag,
             "AUDIO_HIL playback ok=%u bytes=%lu pa_off=%u status=%u",
             playback_ok, static_cast<unsigned long>(playback.last_transfer_bytes),
             playback.amplifier_disabled_verified, playback.last_operation_status);

    if (capture_ok && playback_ok) {
        ESP_LOGI(kTag, "AUDIO_HIL PASS");
    } else {
        ESP_LOGE(kTag, "AUDIO_HIL FAIL");
    }
}
#endif
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

    auto &update = nightglass::update::update_service();
    const auto verifier_init =
        nightglass::update::initialize_provisioned_signature_verifier();
    if (verifier_init.is_ok()) {
        nightglass::core::health_registry().set(
            "update_crypto", nightglass::core::HealthState::ok,
            "P-256 update verifier provisioned");
    } else if (verifier_init.code == nightglass::core::StatusCode::unavailable) {
        nightglass::core::health_registry().set(
            "update_crypto", nightglass::core::HealthState::degraded,
            "OTA signing key not provisioned; updates disabled");
    } else {
        nightglass::core::health_registry().set(
            "update_crypto", nightglass::core::HealthState::failed,
            verifier_init.detail);
        ESP_LOGE(kTag, "OTA verifier provisioning failed: %s", verifier_init.detail);
    }
    const auto verifier_status =
        update.set_signature_verifier(nightglass::update::provisioned_signature_verifier());
    if (!verifier_status.is_ok()) {
        ESP_LOGE(kTag, "OTA verifier setup failed: %s", verifier_status.detail);
    }
    const bool recovery_button = nightglass::update::recovery_button_held_at_boot();
    const auto recovery_status = update.begin_boot(nvs_result == ESP_OK, recovery_button);
    if (!recovery_status.is_ok()) {
        ESP_LOGE(kTag, "Recovery supervisor failed: %s", recovery_status.detail);
    }
    const auto gate_status = update.arm_health_gate();
    if (!gate_status.is_ok()) {
        ESP_LOGE(kTag, "Health gate failed to arm: %s", gate_status.detail);
    }
    const bool safe_mode = update.snapshot().safe_mode;
    if (safe_mode) {
        ESP_LOGW(kTag, "SAFE MODE: optional hardware, audio, activity, and radios disabled");
    }

    const auto board_status = board.start_essential();
    if (!board_status.is_ok()) {
        ESP_LOGE(kTag, "Essential board startup failed: %s", board_status.detail);
        return;
    }

    if (!safe_mode) {
        const auto storage_status = nightglass::services::storage_service().start();
        if (!storage_status.is_ok()) {
            ESP_LOGW(kTag, "Runtime assets unavailable: %s", storage_status.detail);
        }

        const auto hardware_status =
            nightglass::services::hardware_service().start(board.i2c_bus());
        if (!hardware_status.is_ok()) {
            ESP_LOGW(kTag, "Hardware services degraded: %s", hardware_status.detail);
        }

        const auto audio_status = nightglass::services::audio_service().start(board.i2c_bus());
        if (!audio_status.is_ok()) {
            ESP_LOGI(kTag, "Audio service unavailable: %s", audio_status.detail);
        }
    }

    const auto power_status = nightglass::services::power_service().start();
    if (!power_status.is_ok()) {
        ESP_LOGW(kTag, "Power service degraded: %s", power_status.detail);
    }

    const auto clock_status = nightglass::services::clock_service().start();
    if (!clock_status.is_ok()) {
        ESP_LOGW(kTag, "Clock service degraded: %s", clock_status.detail);
    }

    if (!safe_mode) {
        const auto activity_status = nightglass::services::activity_service().start();
        if (!activity_status.is_ok()) {
            ESP_LOGW(kTag, "Activity service degraded: %s", activity_status.detail);
        }
    }

    const auto face_status = nightglass::services::watchface_service().start();
    if (!face_status.is_ok()) {
        ESP_LOGW(kTag, "Watch face service degraded: %s", face_status.detail);
    }

    if (!safe_mode) {
        const auto connectivity_status = nightglass::services::connectivity_service().start();
        if (!connectivity_status.is_ok()) {
            ESP_LOGW(kTag, "Connectivity service degraded: %s", connectivity_status.detail);
        }

        const auto network_status = nightglass::services::network_weather_service().start();
        if (!network_status.is_ok()) {
            ESP_LOGW(kTag, "Network/weather service degraded: %s", network_status.detail);
        }
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

#if CONFIG_NIGHTGLASS_AUDIO_BOOT_SELF_TEST
    if (!safe_mode) run_audio_boot_self_test();
#endif
}

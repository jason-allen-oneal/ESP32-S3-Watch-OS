#include "nightglass/update/service.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nightglass/core/health.hpp"
#include "nightglass/update/policy.hpp"
#include "nvs.h"
#include "sdkconfig.h"

namespace nightglass::update {
namespace {

constexpr char kTag[] = "nightglass_update";
constexpr char kNvsNamespace[] = "ng_recovery";
constexpr char kUnhealthyBootsKey[] = "bad_boots";
constexpr gpio_num_t kRecoveryButton = GPIO_NUM_10;
constexpr TickType_t kRecoverySamplePeriod = pdMS_TO_TICKS(50);
constexpr std::uint32_t kRecoveryHoldSamples = 24;

UpdateService instance;
SemaphoreHandle_t service_mutex = nullptr;
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
UpdateSnapshot current{};
const SignatureVerifier *signature_verifier = nullptr;
esp_ota_handle_t ota_handle = 0;
const esp_partition_t *target_partition = nullptr;
UpdateManifest active_manifest{};
mbedtls_sha256_context sha_context{};
bool sha_active = false;
bool nvs_was_available = false;
TaskHandle_t health_task_handle = nullptr;

class ServiceLock {
public:
    ServiceLock() {
        if (service_mutex == nullptr) service_mutex = xSemaphoreCreateMutex();
        locked_ = service_mutex != nullptr &&
                  xSemaphoreTake(service_mutex, portMAX_DELAY) == pdTRUE;
    }
    ~ServiceLock() {
        if (locked_) xSemaphoreGive(service_mutex);
    }
    [[nodiscard]] bool locked() const { return locked_; }

private:
    bool locked_{false};
};

void publish(const UpdateSnapshot &snapshot) {
    portENTER_CRITICAL(&snapshot_mux);
    current = snapshot;
    portEXIT_CRITICAL(&snapshot_mux);
}

void set_failed(SignatureState signature_state, const char *detail) {
    auto snapshot = instance.snapshot();
    snapshot.state = UpdateState::failed;
    snapshot.signature_state = signature_state;
    publish(snapshot);
    nightglass::core::health_registry().set("update", nightglass::core::HealthState::failed,
                                           detail);
}

void clean_active_write() {
    if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
    }
    if (sha_active) {
        mbedtls_sha256_free(&sha_context);
        sha_active = false;
    }
    target_partition = nullptr;
    active_manifest = {};
}

bool constant_time_equal(const std::array<std::uint8_t, kSha256Size> &left,
                         const std::array<std::uint8_t, kSha256Size> &right) {
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

bool partition_hash_matches(const esp_partition_t *partition, std::uint32_t image_size,
                            const std::array<std::uint8_t, kSha256Size> &expected) {
    if (partition == nullptr || image_size == 0 || image_size > partition->size) return false;
    mbedtls_sha256_context context{};
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts(&context, 0) != 0) {
        mbedtls_sha256_free(&context);
        return false;
    }
    std::array<std::uint8_t, 1024> buffer{};
    std::uint32_t offset = 0;
    while (offset < image_size) {
        const auto count = std::min<std::uint32_t>(buffer.size(), image_size - offset);
        if (esp_partition_read(partition, offset, buffer.data(), count) != ESP_OK ||
            mbedtls_sha256_update(&context, buffer.data(), count) != 0) {
            mbedtls_sha256_free(&context);
            return false;
        }
        offset += count;
        if ((offset & 0xffffU) == 0) taskYIELD();
    }
    std::array<std::uint8_t, kSha256Size> actual{};
    const bool finished = mbedtls_sha256_finish(&context, actual.data()) == 0;
    mbedtls_sha256_free(&context);
    return finished && constant_time_equal(actual, expected);
}

bool required_health_is_ok() {
#if CONFIG_NIGHTGLASS_OTA_HIL_FORCE_HEALTH_FAILURE
    ESP_LOGE(kTag, "OTA_HIL forced essential-health failure");
    return false;
#endif
    constexpr std::array<const char *, 7> safe_mode_required{
        "nvs", "display", "touch", "power", "clock", "ui", "update_crypto"};
    constexpr std::array<const char *, 9> full_required{
        "nvs", "display", "touch", "power", "clock", "ui",
        "update_crypto", "connectivity", "update_transport"};
    const auto check = [](const char *name) {
        nightglass::core::HealthRecord record{};
        if (!nightglass::core::health_registry().copy(name, record)) return false;
        const bool degradation_allowed = std::strcmp(name, "display") == 0 ||
                                         std::strcmp(name, "power") == 0 ||
                                         std::strcmp(name, "clock") == 0;
        if (record.state != nightglass::core::HealthState::ok &&
            !(degradation_allowed && record.state == nightglass::core::HealthState::degraded)) {
            return false;
        }
        return true;
    };
    const auto safe_mode = instance.snapshot().safe_mode;
    if (safe_mode) {
        for (const auto *name : safe_mode_required) {
            if (!check(name)) return false;
        }
    } else {
        for (const auto *name : full_required) {
            if (!check(name)) return false;
        }
    }
    return true;
}

bool store_unhealthy_boots(std::uint32_t count) {
    if (!nvs_was_available) return false;
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t set_result = nvs_set_u32(handle, kUnhealthyBootsKey, count);
    const esp_err_t commit_result = set_result == ESP_OK ? nvs_commit(handle) : set_result;
    nvs_close(handle);
    return set_result == ESP_OK && commit_result == ESP_OK;
}

void health_gate_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(kHealthGateSeconds * 1000U));
    const auto snapshot = instance.snapshot();
    const BootDecision boot{
        .safe_mode = snapshot.safe_mode,
        .pending_verification = snapshot.pending_verification,
        .unhealthy_boots = snapshot.unhealthy_boots,
    };
    const auto action = evaluate_health_gate(boot, required_health_is_ok());

    switch (action) {
        case HealthGateAction::accept_pending_image:
            // Persist healthy state before permanently cancelling rollback.
            // If NVS fails, the pending image must remain rejectable.
            if (!store_unhealthy_boots(0)) {
                nightglass::core::health_registry().set(
                    "recovery", nightglass::core::HealthState::failed,
                    "healthy boot persistence failed; rolling back pending image");
                ESP_LOGE(kTag, "Could not persist healthy boot; requesting rollback");
                if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) {
                    ESP_LOGE(kTag, "Rollback request failed after NVS error");
                }
            } else if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                nightglass::core::health_registry().set(
                    "recovery", nightglass::core::HealthState::ok,
                    "pending image accepted after 60-second health gate");
                auto accepted = instance.snapshot();
                accepted.pending_verification = false;
                publish(accepted);
                ESP_LOGI(kTag, "OTA_HEALTH_ACCEPTED state=VALID pending=0");
            } else {
                nightglass::core::health_registry().set(
                    "recovery", nightglass::core::HealthState::failed,
                    "could not accept pending image; requesting rollback");
                ESP_LOGE(kTag, "Could not accept pending image; requesting rollback");
                if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) {
                    ESP_LOGE(kTag, "Rollback request failed after acceptance error");
                }
            }
            break;
        case HealthGateAction::rollback_pending_image:
            ESP_LOGE(kTag, "Pending image failed health gate; requesting rollback");
            nightglass::core::health_registry().set(
                "recovery", nightglass::core::HealthState::failed,
                "pending image failed 60-second health gate");
            // On success this call does not return. If there is no valid prior
            // slot, retain the pending state rather than falsely accepting it.
            if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) {
                ESP_LOGE(kTag, "Rollback request failed; image remains unconfirmed");
            }
            break;
        case HealthGateAction::record_healthy:
            if (store_unhealthy_boots(0)) {
                nightglass::core::health_registry().set(
                    "recovery", nightglass::core::HealthState::ok,
                    "healthy boot recorded after 60-second gate");
            }
            break;
        case HealthGateAction::retain_unhealthy:
            nightglass::core::health_registry().set(
                "recovery", nightglass::core::HealthState::failed,
                "essential health gate failed");
            break;
    }

    auto updated = instance.snapshot();
    updated.health_gate_armed = false;
    publish(updated);
    health_task_handle = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

nightglass::core::Status UpdateService::begin_boot(bool nvs_available,
                                                   bool recovery_button_held) {
    ServiceLock lock;
    if (!lock.locked()) {
        return {nightglass::core::StatusCode::no_memory, "update mutex unavailable"};
    }
    nvs_was_available = nvs_available;

    bool pending_verification = false;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *configured_boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (running != nullptr && esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        pending_verification = ota_state == ESP_OTA_IMG_PENDING_VERIFY;
    }
    // This bounded line is the authoritative, non-resetting USB diagnostic.
    // Host-side otadata inspection can identify only the boot candidate; it
    // cannot prove which image actually ran after bootloader fallback.
    ESP_LOGI(kTag,
             "OTA_BOOT running=%s@0x%08lx configured=%s@0x%08lx state=%lu "
             "pending=%u rollback_possible=%u",
             running == nullptr ? "none" : running->label,
             static_cast<unsigned long>(running == nullptr ? 0 : running->address),
             configured_boot == nullptr ? "none" : configured_boot->label,
             static_cast<unsigned long>(configured_boot == nullptr ? 0
                                                                    : configured_boot->address),
             static_cast<unsigned long>(ota_state), pending_verification,
             esp_ota_check_rollback_is_possible());

    std::uint32_t unhealthy_boots = 0;
    bool persisted = false;
    if (nvs_available) {
        nvs_handle_t handle = 0;
        if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
            const esp_err_t get_result = nvs_get_u32(handle, kUnhealthyBootsKey,
                                                     &unhealthy_boots);
            if (get_result == ESP_ERR_NVS_NOT_FOUND) unhealthy_boots = 0;
            if (get_result == ESP_OK || get_result == ESP_ERR_NVS_NOT_FOUND) {
                if (unhealthy_boots < std::numeric_limits<std::uint32_t>::max()) {
                    ++unhealthy_boots;
                }
                persisted = nvs_set_u32(handle, kUnhealthyBootsKey, unhealthy_boots) == ESP_OK &&
                            nvs_commit(handle) == ESP_OK;
            }
            nvs_close(handle);
        }
    }

    const auto decision = evaluate_boot(unhealthy_boots, recovery_button_held,
                                        pending_verification);
    UpdateSnapshot snapshot{};
#if CONFIG_NIGHTGLASS_OTA_ENABLED
    snapshot.state = UpdateState::idle;
#else
    snapshot.state = UpdateState::disabled;
#endif
    snapshot.safe_mode = decision.safe_mode;
    snapshot.pending_verification = decision.pending_verification;
    snapshot.unhealthy_boots = decision.unhealthy_boots;
    publish(snapshot);

    if (!nvs_available || !persisted) {
        nightglass::core::health_registry().set(
            "recovery", nightglass::core::HealthState::degraded,
            "boot counter unavailable; pending OTA still requires health gate");
    } else if (decision.safe_mode) {
        nightglass::core::health_registry().set(
            "recovery", nightglass::core::HealthState::degraded,
            recovery_button_held ? "safe mode requested by held side key"
                                 : "safe mode after three unhealthy boots");
    } else {
        nightglass::core::health_registry().set("recovery",
                                               nightglass::core::HealthState::ok,
                                               "boot health gate awaiting startup");
    }
    return nightglass::core::Status::Ok();
}

nightglass::core::Status UpdateService::arm_health_gate() {
    ServiceLock lock;
    if (!lock.locked()) {
        return {nightglass::core::StatusCode::no_memory, "update mutex unavailable"};
    }
    if (health_task_handle != nullptr) return nightglass::core::Status::Ok();
    if (xTaskCreate(health_gate_task, "nightglass_health", 4096, nullptr, 4,
                    &health_task_handle) != pdPASS) {
        health_task_handle = nullptr;
        const auto current_snapshot = this->snapshot();
        if (current_snapshot.pending_verification) {
            nightglass::core::health_registry().set(
                "recovery", nightglass::core::HealthState::failed,
                "pending image health gate unavailable; rolling back");
            ESP_LOGE(kTag, "Health gate task unavailable; requesting rollback");
            if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) {
                ESP_LOGE(kTag, "Rollback request failed after health task error");
            }
        }
        return {nightglass::core::StatusCode::no_memory, "health gate task unavailable"};
    }
    auto snapshot = this->snapshot();
    snapshot.health_gate_armed = true;
    publish(snapshot);
    return nightglass::core::Status::Ok();
}

nightglass::core::Status UpdateService::set_signature_verifier(
    const SignatureVerifier *verifier) {
    ServiceLock lock;
    if (!lock.locked()) {
        return {nightglass::core::StatusCode::no_memory, "update mutex unavailable"};
    }
    if (ota_handle != 0) {
        return {nightglass::core::StatusCode::invalid_state,
                "cannot change verifier during update"};
    }
    signature_verifier = verifier;
    return nightglass::core::Status::Ok();
}

nightglass::core::Status UpdateService::begin_update(
    const UpdateManifest &manifest, std::span<const std::uint8_t> signature) {
#if !CONFIG_NIGHTGLASS_OTA_ENABLED
    (void)manifest;
    (void)signature;
    return {nightglass::core::StatusCode::unavailable, "OTA backend disabled"};
#else
    ServiceLock lock;
    if (!lock.locked()) {
        return {nightglass::core::StatusCode::no_memory, "update mutex unavailable"};
    }
    const auto initial_snapshot = this->snapshot();
    if (ota_handle != 0 || initial_snapshot.state != UpdateState::idle) {
        return {nightglass::core::StatusCode::invalid_state,
                "update service is not idle"};
    }
    if (initial_snapshot.pending_verification) {
        return {nightglass::core::StatusCode::invalid_state,
                "running image has not passed health gate"};
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (running == nullptr || target == nullptr || target == running ||
        target->type != ESP_PARTITION_TYPE_APP ||
        target->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        target->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        return {nightglass::core::StatusCode::invalid_state,
                "inactive OTA partition unavailable"};
    }
    const std::size_t target_label_size = strnlen(target->label, sizeof(target->label));
    if (target_label_size >= initial_snapshot.target_partition.size()) {
        return {nightglass::core::StatusCode::invalid_state,
                "inactive OTA partition label is too long"};
    }

    const esp_app_desc_t *running_description = esp_app_get_description();
    const ValidationContext context{
        .running_version = running_description->version,
        .running_secure_version = running_description->secure_version,
        .inactive_slot_size = target->size,
    };
    const auto validation = validate_manifest(manifest, context);
    if (validation != ManifestStatus::ok) {
        set_failed(SignatureState::not_checked, manifest_status_detail(validation));
        return {nightglass::core::StatusCode::invalid_state,
                manifest_status_detail(validation)};
    }

    std::array<char, 512> canonical{};
    std::size_t canonical_size = 0;
    const auto canonical_result = canonical_signature_payload(manifest, canonical,
                                                              canonical_size);
    if (canonical_result != ManifestStatus::ok) {
        set_failed(SignatureState::not_checked, manifest_status_detail(canonical_result));
        return {nightglass::core::StatusCode::invalid_state,
                manifest_status_detail(canonical_result)};
    }

    const SignatureResult signature_result =
        signature_verifier == nullptr
            ? SignatureResult::unavailable
            : signature_verifier->verify(
                  std::span(reinterpret_cast<const std::uint8_t *>(canonical.data()),
                            canonical_size),
                  signature);
    SignatureState signature_state = SignatureState::not_checked;
    if (signature_result == SignatureResult::verified) {
        signature_state = SignatureState::verified;
    } else if (signature_result == SignatureResult::rejected) {
        set_failed(SignatureState::rejected, "manifest signature rejected");
        return {nightglass::core::StatusCode::invalid_state,
                "manifest signature rejected"};
    } else {
#if CONFIG_NIGHTGLASS_OTA_ALLOW_UNSIGNED_DEVELOPMENT
        signature_state = SignatureState::unsigned_development;
        ESP_LOGW(kTag, "Accepting UNSIGNED DEVELOPMENT update");
#else
        set_failed(SignatureState::verifier_unavailable,
                   "signature verifier is not provisioned");
        return {nightglass::core::StatusCode::unavailable,
                "signature verifier is not provisioned"};
#endif
    }

    esp_ota_handle_t handle = 0;
    const esp_err_t begin_result = esp_ota_begin(target, manifest.image_size, &handle);
    if (begin_result != ESP_OK) {
        set_failed(signature_state, "inactive-slot erase failed");
        return {nightglass::core::StatusCode::io_error, "inactive-slot erase failed"};
    }
    mbedtls_sha256_init(&sha_context);
    if (mbedtls_sha256_starts(&sha_context, 0) != 0) {
        esp_ota_abort(handle);
        mbedtls_sha256_free(&sha_context);
        set_failed(signature_state, "SHA-256 initialization failed");
        return {nightglass::core::StatusCode::io_error, "SHA-256 initialization failed"};
    }

    ota_handle = handle;
    target_partition = target;
    active_manifest = manifest;
    sha_active = true;
    auto snapshot = this->snapshot();
    snapshot.state = UpdateState::receiving;
    snapshot.signature_state = signature_state;
    snapshot.expected_bytes = manifest.image_size;
    snapshot.received_bytes = 0;
    std::copy(manifest.app_version.begin(), manifest.app_version.end(),
              snapshot.target_version.begin());
    std::memcpy(snapshot.target_partition.data(), target->label, target_label_size);
    snapshot.target_partition[target_label_size] = '\0';
    publish(snapshot);
    nightglass::core::health_registry().set("update", nightglass::core::HealthState::ok,
                                           "receiving bounded inactive-slot image");
    return nightglass::core::Status::Ok();
#endif
}

nightglass::core::Status UpdateService::write(std::span<const std::uint8_t> chunk) {
#if !CONFIG_NIGHTGLASS_OTA_ENABLED
    (void)chunk;
    return {nightglass::core::StatusCode::unavailable, "OTA backend disabled"};
#else
    ServiceLock lock;
    if (!lock.locked()) {
        return {nightglass::core::StatusCode::no_memory, "update mutex unavailable"};
    }
    auto snapshot = this->snapshot();
    if (ota_handle == 0 || !sha_active || snapshot.state != UpdateState::receiving) {
        return {nightglass::core::StatusCode::invalid_state, "no active update"};
    }
    if (chunk.empty() || chunk.size() > kMaximumWriteChunk ||
        chunk.size() > snapshot.expected_bytes - snapshot.received_bytes) {
        clean_active_write();
        set_failed(snapshot.signature_state, "update chunk violates stream bounds");
        return {nightglass::core::StatusCode::invalid_state,
                "update chunk violates stream bounds"};
    }
    if (esp_ota_write(ota_handle, chunk.data(), chunk.size()) != ESP_OK ||
        mbedtls_sha256_update(&sha_context, chunk.data(), chunk.size()) != 0) {
        clean_active_write();
        set_failed(snapshot.signature_state, "inactive-slot stream write failed");
        return {nightglass::core::StatusCode::io_error,
                "inactive-slot stream write failed"};
    }
    snapshot.received_bytes += static_cast<std::uint32_t>(chunk.size());
    publish(snapshot);
    return nightglass::core::Status::Ok();
#endif
}

nightglass::core::Status UpdateService::finish() {
#if !CONFIG_NIGHTGLASS_OTA_ENABLED
    return {nightglass::core::StatusCode::unavailable, "OTA backend disabled"};
#else
    ServiceLock lock;
    if (!lock.locked()) {
        return {nightglass::core::StatusCode::no_memory, "update mutex unavailable"};
    }
    auto snapshot = this->snapshot();
    if (ota_handle == 0 || !sha_active || target_partition == nullptr ||
        snapshot.state != UpdateState::receiving) {
        return {nightglass::core::StatusCode::invalid_state, "no active update"};
    }
    if (snapshot.received_bytes != snapshot.expected_bytes) {
        clean_active_write();
        set_failed(snapshot.signature_state, "image stream ended before declared size");
        return {nightglass::core::StatusCode::invalid_state,
                "image stream ended before declared size"};
    }

    std::array<std::uint8_t, kSha256Size> calculated{};
    const int hash_result = mbedtls_sha256_finish(&sha_context, calculated.data());
    mbedtls_sha256_free(&sha_context);
    sha_active = false;
    if (hash_result != 0 || !constant_time_equal(calculated, active_manifest.image_sha256)) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
        target_partition = nullptr;
        set_failed(snapshot.signature_state, "image SHA-256 mismatch");
        return {nightglass::core::StatusCode::invalid_state, "image SHA-256 mismatch"};
    }

    const esp_ota_handle_t completed_handle = ota_handle;
    ota_handle = 0;
    if (esp_ota_end(completed_handle) != ESP_OK) {
        target_partition = nullptr;
        set_failed(snapshot.signature_state, "ESP image validation failed");
        return {nightglass::core::StatusCode::invalid_state,
                "ESP image validation failed"};
    }

    esp_app_desc_t target_description{};
    if (esp_ota_get_partition_description(target_partition, &target_description) != ESP_OK ||
        std::strncmp(target_description.project_name, "nightglass",
                     sizeof(target_description.project_name)) != 0 ||
        std::strncmp(target_description.version, active_manifest.app_version.data(),
                     sizeof(target_description.version)) != 0 ||
        target_description.secure_version != active_manifest.secure_version) {
        target_partition = nullptr;
        set_failed(snapshot.signature_state, "embedded app descriptor mismatch");
        return {nightglass::core::StatusCode::invalid_state,
                "embedded app descriptor mismatch"};
    }
    if (!partition_hash_matches(target_partition, active_manifest.image_size,
                                active_manifest.image_sha256)) {
        target_partition = nullptr;
        set_failed(snapshot.signature_state, "inactive-slot readback SHA-256 mismatch");
        return {nightglass::core::StatusCode::io_error,
                "inactive-slot readback SHA-256 mismatch"};
    }
    const auto *selected_partition = target_partition;
    if (esp_ota_set_boot_partition(target_partition) != ESP_OK) {
        target_partition = nullptr;
        set_failed(snapshot.signature_state, "could not select validated OTA slot");
        return {nightglass::core::StatusCode::io_error,
                "could not select validated OTA slot"};
    }

    esp_ota_img_states_t selected_state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_result =
        esp_ota_get_state_partition(selected_partition, &selected_state);
    ESP_LOGI(kTag,
             "OTA_READY target=%s@0x%08lx state=%ld readback_sha256=verified bytes=%lu",
             selected_partition->label,
             static_cast<unsigned long>(selected_partition->address),
             state_result == ESP_OK ? static_cast<long>(selected_state) : -1L,
             static_cast<unsigned long>(active_manifest.image_size));

    target_partition = nullptr;
    active_manifest = {};
    snapshot.state = UpdateState::ready_to_reboot;
    publish(snapshot);
    nightglass::core::health_registry().set(
        "update", nightglass::core::HealthState::ok,
        "inactive slot validated and selected; reboot not automatic");
    return nightglass::core::Status::Ok();
#endif
}

nightglass::core::Status UpdateService::abort() {
    ServiceLock lock;
    if (!lock.locked()) {
        return {nightglass::core::StatusCode::no_memory, "update mutex unavailable"};
    }
    auto snapshot = this->snapshot();
    if (snapshot.state == UpdateState::ready_to_reboot) {
        return {nightglass::core::StatusCode::invalid_state,
                "validated slot is already selected for reboot"};
    }
    clean_active_write();
#if CONFIG_NIGHTGLASS_OTA_ENABLED
    snapshot.state = UpdateState::idle;
#else
    snapshot.state = UpdateState::disabled;
#endif
    snapshot.signature_state = SignatureState::not_checked;
    snapshot.expected_bytes = 0;
    snapshot.received_bytes = 0;
    snapshot.target_version = {};
    snapshot.target_partition = {};
    publish(snapshot);
    return nightglass::core::Status::Ok();
}

UpdateSnapshot UpdateService::snapshot() const {
    portENTER_CRITICAL(&snapshot_mux);
    const auto copy = current;
    portEXIT_CRITICAL(&snapshot_mux);
    return copy;
}

bool recovery_button_held_at_boot() {
    const gpio_config_t config{
        .pin_bit_mask = 1ULL << kRecoveryButton,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK || gpio_get_level(kRecoveryButton) == 0) {
        return false;
    }
    for (std::uint32_t sample = 0; sample < kRecoveryHoldSamples; ++sample) {
        vTaskDelay(kRecoverySamplePeriod);
        if (gpio_get_level(kRecoveryButton) == 0) return false;
    }
    return true;
}

UpdateService &update_service() { return instance; }

}  // namespace nightglass::update

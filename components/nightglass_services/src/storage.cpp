#include "nightglass/services/storage.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nightglass/core/health.hpp"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_storage";
constexpr char kPartitionLabel[] = "assets";
constexpr char kMountPoint[] = "/assets";
constexpr std::array<const char *, 8> kSoundCatalog{
    "notification", "call", "calendar", "email",
    "alarm", "timer", "success", "warning",
};

StorageService instance;
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
StorageSnapshot current{};

void publish(bool mounted, std::size_t total, std::size_t used, const char *detail) {
    portENTER_CRITICAL(&snapshot_mux);
    current.mounted = mounted;
    current.total_bytes = total;
    current.used_bytes = used;
    std::snprintf(current.detail.data(), current.detail.size(), "%s", detail);
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
}

bool catalog_contains(const char *catalog_id) {
    if (!catalog_id || catalog_id[0] == '\0') return false;
    return std::any_of(kSoundCatalog.begin(), kSoundCatalog.end(),
                       [catalog_id](const char *entry) {
                           return std::strcmp(entry, catalog_id) == 0;
                       });
}

}  // namespace

nightglass::core::Status StorageService::start() {
    if (snapshot().mounted) return nightglass::core::Status::Ok();

    const esp_vfs_littlefs_conf_t config{
        .base_path = kMountPoint,
        .partition_label = kPartitionLabel,
        .partition = nullptr,
        .format_if_mount_failed = false,
        // Assets remain immutable until an authenticated, atomic installer is
        // available. The volume never formats itself on mount failure.
        .read_only = true,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    const esp_err_t mounted = esp_vfs_littlefs_register(&config);
    if (mounted != ESP_OK) {
        publish(false, 0, 0, "Asset volume unavailable; built-in assets active");
        nightglass::core::health_registry().set(
            "storage", nightglass::core::HealthState::degraded,
            "LittleFS assets unavailable; no automatic format");
        ESP_LOGW(kTag, "Asset volume mount skipped: %s", esp_err_to_name(mounted));
        return {nightglass::core::StatusCode::degraded,
                "asset volume unavailable; built-in assets active"};
    }

    std::size_t total = 0;
    std::size_t used = 0;
    const esp_err_t info = esp_littlefs_info(kPartitionLabel, &total, &used);
    if (info != ESP_OK || used > total) {
        esp_vfs_littlefs_unregister(kPartitionLabel);
        publish(false, 0, 0, "Asset volume metadata invalid");
        nightglass::core::health_registry().set(
            "storage", nightglass::core::HealthState::failed,
            "LittleFS metadata validation failed");
        return {nightglass::core::StatusCode::io_error,
                "asset volume metadata invalid"};
    }

    publish(true, total, used, "Runtime asset volume mounted");
    nightglass::core::health_registry().set(
        "storage", nightglass::core::HealthState::ok,
        "Runtime asset volume mounted without format fallback");
    ESP_LOGI(kTag, "Asset volume mounted: used=%u total=%u",
             static_cast<unsigned>(used), static_cast<unsigned>(total));
    return nightglass::core::Status::Ok();
}

StorageSnapshot StorageService::snapshot() const {
    StorageSnapshot copy{};
    portENTER_CRITICAL(&snapshot_mux);
    copy = current;
    portEXIT_CRITICAL(&snapshot_mux);
    return copy;
}

bool StorageService::sound_path(const char *catalog_id, char *path,
                                std::size_t path_size) const {
    if (!path || path_size == 0 || !snapshot().mounted || !catalog_contains(catalog_id)) {
        return false;
    }
    const int written = std::snprintf(path, path_size, "%s/sounds/%s.wav",
                                      kMountPoint, catalog_id);
    return written > 0 && static_cast<std::size_t>(written) < path_size;
}

StorageService &storage_service() { return instance; }

}  // namespace nightglass::services

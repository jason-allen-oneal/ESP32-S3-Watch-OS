#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "nightglass/core/status.hpp"

namespace nightglass::services {

struct StorageSnapshot {
    std::uint32_t sequence{0};
    bool mounted{false};
    std::size_t total_bytes{0};
    std::size_t used_bytes{0};
    std::array<char, 64> detail{"Not started"};
};

static_assert(std::is_trivially_copyable_v<StorageSnapshot>);

class StorageService {
public:
    nightglass::core::Status start();
    [[nodiscard]] StorageSnapshot snapshot() const;

    // Catalog entries are fixed ASCII identifiers, never caller-provided paths.
    // The returned path is valid only when the asset volume is mounted.
    [[nodiscard]] bool sound_path(const char *catalog_id, char *path,
                                  std::size_t path_size) const;
};

StorageService &storage_service();

}  // namespace nightglass::services

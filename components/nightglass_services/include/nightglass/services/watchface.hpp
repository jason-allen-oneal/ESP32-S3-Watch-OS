#pragma once

#include <cstddef>
#include <cstdint>

#include "nightglass/core/status.hpp"

namespace nightglass::services {

enum class FaceLayout : std::uint8_t {
    classic,
    revenant_grid,
};

enum FaceComplication : std::uint16_t {
    complication_date = 1U << 0,
    complication_battery = 1U << 1,
    complication_motion = 1U << 2,
    complication_alarm = 1U << 3,
    complication_timer = 1U << 4,
};

struct FacePalette {
    std::uint32_t background;
    std::uint32_t surface;
    std::uint32_t border;
    std::uint32_t primary;
    std::uint32_t secondary;
    std::uint32_t accent;
    std::uint32_t accent_dim;
};

// Face packs are declarative data. They may choose a supported layout and
// palette, but never execute code or take ownership of system services.
struct FacePack {
    std::uint8_t id;
    const char *slug;
    const char *name;
    std::uint8_t format_version;
    FaceLayout layout;
    std::uint16_t complications;
    FacePalette palette;
};

class WatchFaceService {
public:
    nightglass::core::Status start();
    [[nodiscard]] const FacePack &selected() const;
    [[nodiscard]] const FacePack *packs() const;
    [[nodiscard]] std::size_t pack_count() const;
    bool select(std::uint8_t id);
};

WatchFaceService &watchface_service();

}  // namespace nightglass::services

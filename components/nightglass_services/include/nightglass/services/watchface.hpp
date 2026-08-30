#pragma once

#include <cstddef>
#include <cstdint>

#include "nightglass/core/status.hpp"

namespace nightglass::services {

enum class FaceLayout : std::uint8_t {
    classic,
    full_background,
};

enum FaceComplication : std::uint16_t {
    complication_time = 1U << 0,
    complication_date = 1U << 1,
    complication_battery = 1U << 2,
    complication_motion = 1U << 3,
    complication_steps = 1U << 4,
    complication_alarm = 1U << 5,
    complication_timer = 1U << 6,
    complication_distance = 1U << 7,
    complication_weather = 1U << 8,
    complication_notifications = 1U << 9,
};

enum class FaceAsset : std::uint8_t { none, revenant_grid_v2 };

enum class ChromeTheme : std::uint8_t { classic, revenant };

struct FaceChrome {
    ChromeTheme theme;
    FaceAsset route_background_asset;
    std::uint8_t route_background_opacity;
};

enum class FaceField : std::uint8_t {
    fixed_text,
    time,
    time_state,
    day,
    date,
    battery,
    battery_detail,
    steps,
    motion,
    alarm,
    timer,
    distance,
    weather,
    notifications,
};

enum class FaceTextStyle : std::uint8_t {
    caption_14,
    body_16,
    value_20,
    time_48,
};

enum class FaceTextAlign : std::uint8_t { left, center, right };

enum class FaceColorRole : std::uint8_t { primary, secondary, accent, accent_dim };

enum class FaceAction : std::uint8_t {
    open_apps,
    open_activity,
    open_weather,
    open_notifications,
    open_connectivity,
    open_alarm,
    open_countdown,
    open_diagnostics,
    open_clock_settings,
};

struct FaceRect {
    std::int16_t x;
    std::int16_t y;
    std::uint16_t width;
    std::uint16_t height;
};

struct FaceTextSlot {
    FaceField field;
    FaceRect bounds;
    FaceTextStyle style;
    FaceTextAlign align;
    FaceColorRole color;
    const char *fixed_text;
};

struct FaceActionSlot {
    FaceAction action;
    FaceRect bounds;
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
    FaceAsset background_asset;
    FaceChrome chrome;
    const FaceTextSlot *text_slots;
    std::uint8_t text_slot_count;
    const FaceActionSlot *action_slots;
    std::uint8_t action_slot_count;
};

// Enforces a bounded firmware schema. Resources are stable enum IDs, never
// executable callbacks or filesystem paths.
[[nodiscard]] bool valid_face_pack(const FacePack &pack);

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

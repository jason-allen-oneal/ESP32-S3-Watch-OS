#pragma once

#include <cstdint>
#include <type_traits>

#include "nightglass/core/service.hpp"
#include "nightglass/core/status.hpp"

namespace nightglass::services {

struct PowerSettings {
    std::uint8_t active_brightness{30};
    std::uint8_t dim_brightness{8};
    std::uint16_t dim_after_seconds{15};
    std::uint16_t blank_after_seconds{30};
    std::uint16_t sleep_after_blank_seconds{15};  // 0 disables light sleep.
};

static_assert(std::is_trivially_copyable_v<PowerSettings>);

struct PowerSnapshot {
    std::uint32_t sequence{0};
    nightglass::core::PowerState state{nightglass::core::PowerState::active};
    nightglass::core::WakeReason last_wake{nightglass::core::WakeReason::cold_boot};
    std::uint8_t requested_brightness{30};
    bool side_key_ready{false};
    bool side_key_pressed{false};
    bool light_sleep_enabled{false};
    bool automatic_light_sleep_enabled{false};
    bool sleeping{false};
    bool wake_touch_pending{false};
    std::uint32_t sleep_count{0};
    std::uint32_t last_sleep_ms{0};
    std::int64_t last_activity_us{0};
    PowerSettings settings{};
};

static_assert(std::is_trivially_copyable_v<PowerSnapshot>);

class PowerService {
public:
    nightglass::core::Status start();
    void note_activity(nightglass::core::WakeReason reason = nightglass::core::WakeReason::touch);
    nightglass::core::Status update_settings(const PowerSettings &settings);
    [[nodiscard]] PowerSnapshot snapshot() const;
};

PowerService &power_service();

}  // namespace nightglass::services

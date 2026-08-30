#pragma once

#include <cstdint>
#include <type_traits>

#include "nightglass/core/service.hpp"
#include "nightglass/core/status.hpp"

namespace nightglass::services {

struct PowerSnapshot {
    std::uint32_t sequence{0};
    nightglass::core::PowerState state{nightglass::core::PowerState::active};
    nightglass::core::WakeReason last_wake{nightglass::core::WakeReason::cold_boot};
    std::uint8_t requested_brightness{30};
    bool side_key_ready{false};
    bool side_key_pressed{false};
    bool light_sleep_enabled{false};
    std::int64_t last_activity_us{0};
};

static_assert(std::is_trivially_copyable_v<PowerSnapshot>);

class PowerService {
public:
    nightglass::core::Status start();
    void note_activity(nightglass::core::WakeReason reason = nightglass::core::WakeReason::touch);
    [[nodiscard]] PowerSnapshot snapshot() const;
};

PowerService &power_service();

}  // namespace nightglass::services

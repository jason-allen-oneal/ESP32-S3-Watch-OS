#pragma once

#include "nightglass/services/gesture_processor.hpp"

namespace nightglass::services {

enum class GestureAction : std::uint8_t {
    none,
    wake_only,
    open_quick_settings,
    open_notifications,
    media_next,
};

struct GesturePolicyContext {
    GestureKind kind{GestureKind::none};
    bool enabled{false};
    bool screen_inactive{false};
    bool external_power{false};
    bool recent_physical_input{false};
    bool critical_overlay{false};
    bool reply_active{false};
};

[[nodiscard]] GestureAction decide_gesture_action(
    const GesturePolicyContext &context) noexcept;

}  // namespace nightglass::services

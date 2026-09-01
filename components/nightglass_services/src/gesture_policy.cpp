#include "nightglass/services/gesture_policy.hpp"

namespace nightglass::services {

GestureAction decide_gesture_action(const GesturePolicyContext &context) noexcept {
    if (context.kind == GestureKind::none || !context.enabled ||
        context.external_power || context.recent_physical_input) {
        return GestureAction::none;
    }
    if (context.critical_overlay || context.reply_active) return GestureAction::none;
    if (context.kind == GestureKind::raise) {
        return context.screen_inactive ? GestureAction::wake_only : GestureAction::none;
    }
    if (context.screen_inactive) return GestureAction::wake_only;
    switch (context.kind) {
        case GestureKind::double_twist: return GestureAction::open_quick_settings;
        case GestureKind::shake: return GestureAction::open_notifications;
        case GestureKind::flick: return GestureAction::media_next;
        case GestureKind::raise:
        case GestureKind::none: break;
    }
    return GestureAction::none;
}

}  // namespace nightglass::services

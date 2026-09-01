#include <cassert>

#include "nightglass/services/gesture_policy.hpp"

using namespace nightglass::services;

int main() {
    assert(decide_gesture_action({GestureKind::raise, true, true}) ==
           GestureAction::wake_only);
    assert(decide_gesture_action({GestureKind::raise, true, false}) == GestureAction::none);
    assert(decide_gesture_action({GestureKind::double_twist, true, false}) ==
           GestureAction::open_quick_settings);
    assert(decide_gesture_action({GestureKind::shake, true, false}) ==
           GestureAction::open_notifications);
    assert(decide_gesture_action({GestureKind::flick, true, false}) ==
           GestureAction::media_next);
    assert(decide_gesture_action({GestureKind::flick, false, false}) == GestureAction::none);
    assert(decide_gesture_action({GestureKind::double_twist, true, true}) ==
           GestureAction::wake_only);
    assert(decide_gesture_action({GestureKind::shake, true, false, true}) ==
           GestureAction::none);
    assert(decide_gesture_action({GestureKind::shake, true, false, false, true}) ==
           GestureAction::none);
    assert(decide_gesture_action({GestureKind::shake, true, false, false, false, true}) ==
           GestureAction::none);
    assert(decide_gesture_action({GestureKind::shake, true, false, false, false, false,
                                  true}) == GestureAction::none);
    assert(decide_gesture_action({.kind = GestureKind::double_twist,
                                  .enabled = true,
                                  .screen_inactive = true,
                                  .critical_overlay = true}) == GestureAction::none);
    return 0;
}

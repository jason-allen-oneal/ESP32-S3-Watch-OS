#include <cassert>

#include "nightglass/core/navigation.hpp"

using namespace nightglass::core;

int main() {
    auto state = reduce_navigation({}, NavigationAction::open_notifications);
    assert(state.route == Route::notifications);
    state = reduce_navigation(state, NavigationAction::back);
    assert(state.route == Route::launcher);
    state = reduce_navigation({Route::notifications, Overlay::system_modal},
                              NavigationAction::back);
    assert(state.route == Route::notifications);
    assert(state.overlay == Overlay::none);
}

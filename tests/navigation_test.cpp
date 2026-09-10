#include <cassert>

#include "nightglass/core/navigation.hpp"

using namespace nightglass::core;

int main() {
    auto state = reduce_navigation({}, NavigationAction::open_notifications);
    assert(state.route == Route::notifications);
    state = reduce_navigation(state, NavigationAction::back);
    assert(state.route == Route::home);
    state = reduce_navigation({Route::notifications, Overlay::system_modal, Route::home,
                               Route::home},
                              NavigationAction::back);
    assert(state.route == Route::notifications);
    assert(state.overlay == Overlay::none);

    state = reduce_navigation({}, NavigationAction::open_context_deck);
    assert(state.route == Route::context_deck);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::home);

    state = reduce_navigation({}, NavigationAction::open_launcher);
    state = reduce_navigation(state, NavigationAction::open_activity);
    assert(state.back_route == Route::launcher);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::launcher);

    state = reduce_navigation({}, NavigationAction::open_settings);
    state = reduce_navigation(state, NavigationAction::open_power_settings);
    state = reduce_navigation(state, NavigationAction::back);
    assert(state.route == Route::settings);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::home);

    state = reduce_navigation({}, NavigationAction::open_launcher);
    state = reduce_navigation(state, NavigationAction::open_settings);
    state = reduce_navigation(state, NavigationAction::open_clock_settings);
    state = reduce_navigation(state, NavigationAction::back);
    assert(state.route == Route::settings);
    state = reduce_navigation(state, NavigationAction::back);
    assert(state.route == Route::launcher);

    state = reduce_navigation({}, NavigationAction::open_clock_settings);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::home);

    state = reduce_navigation({}, NavigationAction::open_audio);
    assert(state.route == Route::audio);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::home);

    state = reduce_navigation({}, NavigationAction::open_media);
    assert(state.route == Route::media);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::home);

    state = reduce_navigation({}, NavigationAction::open_spotify);
    assert(state.route == Route::spotify);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::home);

    state = reduce_navigation({}, NavigationAction::open_discord);
    assert(state.route == Route::discord);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::home);

    state = reduce_navigation({}, NavigationAction::open_gestures);
    assert(state.route == Route::gestures);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::home);

    state = reduce_navigation({}, NavigationAction::open_openclaw);
    assert(state.route == Route::openclaw);
    assert(reduce_navigation(state, NavigationAction::back).route == Route::home);
}

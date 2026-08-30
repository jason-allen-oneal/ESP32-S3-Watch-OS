#include "nightglass/core/navigation.hpp"

namespace nightglass::core {

namespace {
constexpr NavigationState reduce_impl(NavigationState state, NavigationAction action) noexcept {
    if (action == NavigationAction::back && state.overlay != Overlay::none) {
        state.overlay = Overlay::none;
        return state;
    }

    switch (action) {
        case NavigationAction::back:
            state.route = state.route == Route::home ? Route::home
                          : state.route == Route::launcher ? Route::home
                          : state.route == Route::power_settings ||
                                state.route == Route::clock_settings ||
                                state.route == Route::watchface_settings
                              ? Route::settings
                              : Route::launcher;
            break;
        case NavigationAction::home:
            state.route = Route::home;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_launcher:
            state.route = Route::launcher;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_settings:
            state.route = Route::settings;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_power_settings:
            state.route = Route::power_settings;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_clock_settings:
            state.route = Route::clock_settings;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_watchface_settings:
            state.route = Route::watchface_settings;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_alarm:
            state.route = Route::alarm;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_countdown:
            state.route = Route::countdown;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_stopwatch:
            state.route = Route::stopwatch;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_diagnostics:
            state.route = Route::diagnostics;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::open_about:
            state.route = Route::about;
            state.overlay = Overlay::none;
            break;
        case NavigationAction::show_system_modal:
            state.overlay = Overlay::system_modal;
            break;
        case NavigationAction::dismiss_overlay:
            state.overlay = Overlay::none;
            break;
    }
    return state;
}

static_assert(reduce_impl({Route::home, Overlay::none},
                          NavigationAction::open_launcher) ==
              NavigationState{Route::launcher, Overlay::none});
static_assert(reduce_impl({Route::diagnostics, Overlay::none}, NavigationAction::back) ==
              NavigationState{Route::launcher, Overlay::none});
static_assert(reduce_impl({Route::launcher, Overlay::none}, NavigationAction::back) ==
              NavigationState{Route::home, Overlay::none});
static_assert(reduce_impl({Route::about, Overlay::system_modal}, NavigationAction::back) ==
              NavigationState{Route::about, Overlay::none});
}  // namespace

NavigationState reduce_navigation(NavigationState state, NavigationAction action) noexcept {
    return reduce_impl(state, action);
}

}  // namespace nightglass::core

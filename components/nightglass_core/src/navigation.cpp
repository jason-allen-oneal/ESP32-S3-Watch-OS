#include "nightglass/core/navigation.hpp"

namespace nightglass::core {

namespace {
constexpr void push_route(NavigationState &state, Route route) noexcept {
    state.back_back_route = state.back_route;
    state.back_route = state.route;
    state.route = route;
    state.overlay = Overlay::none;
}

constexpr NavigationState reduce_impl(NavigationState state, NavigationAction action) noexcept {
    if (action == NavigationAction::back && state.overlay != Overlay::none) {
        state.overlay = Overlay::none;
        return state;
    }

    switch (action) {
        case NavigationAction::back:
            if (state.route != Route::home) {
                state.route = state.back_route;
                state.back_route = state.back_back_route;
                state.back_back_route = Route::home;
            }
            break;
        case NavigationAction::home:
            state.route = Route::home;
            state.overlay = Overlay::none;
            state.back_route = Route::home;
            state.back_back_route = Route::home;
            break;
        case NavigationAction::open_launcher:
            state.route = Route::launcher;
            state.overlay = Overlay::none;
            state.back_route = Route::home;
            state.back_back_route = Route::home;
            break;
        case NavigationAction::open_settings:
            push_route(state, Route::settings);
            break;
        case NavigationAction::open_power_settings:
            push_route(state, Route::power_settings);
            break;
        case NavigationAction::open_clock_settings:
            push_route(state, Route::clock_settings);
            break;
        case NavigationAction::open_watchface_settings:
            push_route(state, Route::watchface_settings);
            break;
        case NavigationAction::open_activity:
            push_route(state, Route::activity);
            break;
        case NavigationAction::open_weather:
            push_route(state, Route::weather);
            break;
        case NavigationAction::open_connectivity:
            push_route(state, Route::connectivity);
            break;
        case NavigationAction::open_media:
            push_route(state, Route::media);
            break;
        case NavigationAction::open_notifications:
            push_route(state, Route::notifications);
            break;
        case NavigationAction::open_alarm:
            push_route(state, Route::alarm);
            break;
        case NavigationAction::open_countdown:
            push_route(state, Route::countdown);
            break;
        case NavigationAction::open_stopwatch:
            push_route(state, Route::stopwatch);
            break;
        case NavigationAction::open_diagnostics:
            push_route(state, Route::diagnostics);
            break;
        case NavigationAction::open_audio:
            push_route(state, Route::audio);
            break;
        case NavigationAction::open_about:
            push_route(state, Route::about);
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

static_assert(reduce_impl({Route::home, Overlay::none, Route::home, Route::home},
                          NavigationAction::open_launcher) ==
              NavigationState{Route::launcher, Overlay::none, Route::home, Route::home});
static_assert(reduce_impl({Route::diagnostics, Overlay::none, Route::home, Route::home}, NavigationAction::back) ==
              NavigationState{Route::home, Overlay::none, Route::home, Route::home});
static_assert(reduce_impl({Route::launcher, Overlay::none, Route::home, Route::home}, NavigationAction::back) ==
              NavigationState{Route::home, Overlay::none, Route::home, Route::home});
static_assert(reduce_impl({Route::about, Overlay::system_modal, Route::launcher, Route::home}, NavigationAction::back) ==
              NavigationState{Route::about, Overlay::none, Route::launcher, Route::home});
static_assert(reduce_impl({Route::home, Overlay::none, Route::home, Route::home},
                          NavigationAction::open_notifications) ==
              NavigationState{Route::notifications, Overlay::none, Route::home, Route::home});
static_assert(reduce_impl({Route::notifications, Overlay::none, Route::home, Route::home},
                          NavigationAction::back) ==
              NavigationState{Route::home, Overlay::none, Route::home, Route::home});
}  // namespace

NavigationState reduce_navigation(NavigationState state, NavigationAction action) noexcept {
    return reduce_impl(state, action);
}

}  // namespace nightglass::core

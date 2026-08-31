#pragma once

#include <cstdint>

namespace nightglass::core {

enum class Route : std::uint8_t {
    home,
    launcher,
    settings,
    power_settings,
    clock_settings,
    watchface_settings,
    activity,
    weather,
    connectivity,
    notifications,
    alarm,
    countdown,
    stopwatch,
    diagnostics,
    about,
};

enum class Overlay : std::uint8_t {
    none,
    system_modal,
};

enum class NavigationAction : std::uint8_t {
    back,
    home,
    open_launcher,
    open_settings,
    open_power_settings,
    open_clock_settings,
    open_watchface_settings,
    open_activity,
    open_weather,
    open_connectivity,
    open_notifications,
    open_alarm,
    open_countdown,
    open_stopwatch,
    open_diagnostics,
    open_about,
    show_system_modal,
    dismiss_overlay,
};

struct NavigationState {
    Route route{Route::home};
    Overlay overlay{Overlay::none};
    Route back_route{Route::home};
    Route back_back_route{Route::home};

    friend constexpr bool operator==(const NavigationState &, const NavigationState &) = default;
};

// Pure and bounded: every action produces one of the declared routes and
// system overlays. Back always dismisses a system overlay before changing the
// application route.
[[nodiscard]] NavigationState reduce_navigation(NavigationState state,
                                                NavigationAction action) noexcept;

}  // namespace nightglass::core

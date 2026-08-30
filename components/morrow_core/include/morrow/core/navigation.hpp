#pragma once

#include <cstdint>

namespace morrow::core {

enum class Route : std::uint8_t {
    home,
    launcher,
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
    open_diagnostics,
    open_about,
    show_system_modal,
    dismiss_overlay,
};

struct NavigationState {
    Route route{Route::home};
    Overlay overlay{Overlay::none};

    friend constexpr bool operator==(const NavigationState &, const NavigationState &) = default;
};

// Pure and bounded: every action produces one of the declared routes and
// system overlays. Back always dismisses a system overlay before changing the
// application route.
[[nodiscard]] NavigationState reduce_navigation(NavigationState state,
                                                NavigationAction action) noexcept;

}  // namespace morrow::core

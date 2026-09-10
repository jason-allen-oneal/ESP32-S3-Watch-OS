#!/usr/bin/env python3
"""Guard the first premium interaction-shell slice."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NAVIGATION = (ROOT / "components/nightglass_core/include/nightglass/core/navigation.hpp").read_text()
NAVIGATION_IMPL = (ROOT / "components/nightglass_core/src/navigation.cpp").read_text()
SHELL_HEADER = (ROOT / "components/nightglass_ui/include/nightglass/ui/shell.hpp").read_text()
SHELL = (ROOT / "components/nightglass_ui/src/shell.cpp").read_text()


assert "context_deck" in NAVIGATION
assert "open_context_deck" in NAVIGATION
assert "NavigationAction::open_context_deck" in NAVIGATION_IMPL
assert "Route::context_deck" in SHELL
assert "void render_context_deck()" in SHELL_HEADER
assert "void refresh_context_deck()" in SHELL_HEADER
assert "Touch gesture: quick settings" in SHELL
assert "Touch gesture: notifications" in SHELL
assert "Touch gesture: context deck" in SHELL
assert "dispatch_touch_swipe" in SHELL
assert "install_touch_callbacks(content_host_)" in SHELL
assert "LV_OBJ_FLAG_EVENT_BUBBLE" in SHELL
assert "LV_EVENT_PRESSING" in SHELL
assert "LV_EVENT_PRESS_LOST" in SHELL
assert "LV_OBJ_FLAG_GESTURE_BUBBLE" in SHELL
assert "handle_side_key(bool woke_from_inactive)" in SHELL_HEADER
assert "Side key long press reserved for system menu" in SHELL
assert "kSwipeThreshold = 56" in SHELL
assert "auto *scroller = make_scroller(content_host_)" in SHELL
assert "kCardStep = large ? 128 : 104" in SHELL
assert "kCardHeight = large ? 120 : 96" in SHELL
assert "for (auto index : profile.deck_order)" in SHELL
assert "profile.deck_mask & (1U << index)" in SHELL
assert "lv_label_set_long_mode(widgets.detail, LV_LABEL_LONG_MODE_WRAP)" in SHELL
assert "lv_font_montserrat_18" in SHELL

# Home has no previous route: an edge-start horizontal swipe must open the
# Context Deck before the sub-screen edge-back handlers can run.
home_context = SHELL.index(
    "navigation_.route == nightglass::core::Route::home && !quick_settings_open_"
)
left_edge_back = SHELL.index(
    "touch_start_point_.x <= kEdgeGestureInset &&"
)
assert home_context < left_edge_back

print("Nightglass Context Deck and interaction-shell contract passed")

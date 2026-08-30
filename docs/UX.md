# Obsidian Interface System

Obsidian is Nightglass's AMOLED-first interface: black fields, restrained spectral
accents, high legibility, and no fabricated data. Cyber-goth atmosphere is a
texture, not an excuse for radioactive clutter.

The 410×502 panel has rounded visible corners. Persistent text and interactive
controls stay inside a conservative 28 px top, bottom, and horizontal safe
inset; only full-bleed backgrounds may extend into the clipped corner regions.

## Global navigation

- Watch face is Home.
- Swipe down: Quick Settings.
- Swipe up: notifications.
- Swipe left/right: configurable glance tiles.
- Configurable 24 px edge swipe: Back.
- Short side-button press: Home/launcher according to current context.
- Long press: power and recovery menu.

Touch targets are at least 56x56 px. Applications cannot override system-owned
back, quick settings, notification, modal, theme, or power behavior.

## Screen hierarchy

```text
sleep/AOD -> watch face -> launcher -> application
              |  |  |
              |  |  +-> glance tiles
              |  +----> notification shade
              +-------> quick settings
```

The launcher defaults to a labeled vertical list with configurable favorites.
Unavailable hardware-dependent features are disabled with an explanation; they
never appear as plausible placeholders.

### Current shell slice

The daily-use shell keeps one LVGL screen alive and swaps route content inside
a system-owned host. Home renders the selected built-in face pack with live
RTC time/date, battery, alarm, timer, and motion complications. The launcher
provides Alarm, Timer, Stopwatch, Settings, Diagnostics, and About. Application
routes provide a 56 px touch Back control; Diagnostics uses
a safe-area scroller and runs its 100 ms refresh timer only while visible. A
separate system overlay layer is reserved for future modal UI.

Quick Settings, notifications, glance tiles, edge gestures, and side-button
navigation remain roadmap behavior until their services and input contracts
exist. They are not represented by inactive or misleading controls.

## Classic design tokens

- Background `#000000`
- Surface `#0B0D12`
- Elevated `#141824`
- Divider `#272D3A`
- Primary text `#F4F6FA`
- Secondary text `#A2ABBA`
- Cyan `#63DDE4`
- Ultraviolet `#A78BFA`
- Healthy `#50D890`
- Warning `#FFB454`
- Error `#FF6174`

Grid is 4 px, safe-area side gutter 28 px, standard gaps 8/12/16/24 px, and cards use
12/16 px radii. Shadows and continuous decorative animation are forbidden.

## Theme packages and customization

Users may change watch-face/theme package, complication assignments, launcher
favorites, glance-tile order, quick controls, handedness, text scale, wake
gestures, sound/haptic profile, AOD, and notification privacy without flashing.

Face and shell chrome are one validated package selection so application routes
cannot drift away from the active face. Packages have a format version,
resource limits, compatible complications, and enum-allowlisted layouts,
assets, and chrome. They cannot contain executable code, callbacks, filesystem
paths, arbitrary resources, scripts, or fonts. An invalid package or stored ID
resolves to the built-in Classic face and cyan shell.

Format 3 is implemented as a built-in manifest registry with persistent NVS
selection. Nightglass Classic is the fallback. Revenant Grid v2 adds a
black/gunmetal shell, acid-green accents and borders, themed pressed states,
and its compiled background at low opacity behind all non-home routes and alert
overlays. Semantic healthy, warning, and error colors remain green, amber, and
red. External filesystem loading remains deferred until manifest validation
and decoded-asset limits are enforced.

## Release UX gates

- Offline watch face visible within 2.5 seconds.
- Visible touch response median below 80 ms and p95 below 150 ms.
- Normal transitions sustain at least 30 fps.
- No fake or silently stale metrics.
- Inactive screens stop refresh and sensor subscriptions.
- Large Text and Reduce Motion pass the same navigation tests.
- AOD has its own low-luminance layout and pixel shifting.

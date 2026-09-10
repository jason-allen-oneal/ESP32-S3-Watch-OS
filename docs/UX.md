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

The notification center lists the bounded companion cache, distinguishes live
and offline-cached state, and exposes firmware-owned mark-read and dismiss
actions. Quick Settings opens from the top-edge swipe, notifications open from
the bottom-edge swipe, and either horizontal edge returns to the previous route
without stealing normal in-app scrolling. A horizontal swipe on Home opens the
Context Deck: five live, vertically scrollable cards for the next agenda item,
movement, weather, media, and inbox/phone state. Cards keep a readable title and
wrapped detail instead of compressing the full stack into the panel's bottom
safe area. The short side-button press returns to Home from an app and opens the
launcher from Home; long press remains reserved for the future system
power/recovery confirmation menu.

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

Gesture setup is an on-watch guided flow rather than a generic sensitivity
slider. It visibly separates stationary sensor zeroing from intentional motion
examples, counts each accepted repetition, gives a bounded retry after a missed
sample, and does not replace the saved profile until a clean quiet check passes.
Leaving the screen cancels the unfinished run and retains the prior profile.

Face and shell chrome are one validated package selection so application routes
cannot drift away from the active face. Packages have a format version,
resource limits, compatible complications, and enum-allowlisted layouts,
assets, and chrome. They cannot contain executable code, callbacks, filesystem
paths, arbitrary resources, scripts, or fonts. An invalid package or stored ID
resolves to the built-in Classic face and cyan shell.

Format 5 is implemented as a built-in manifest registry with persistent NVS
selection. Nightglass Classic is the fallback. Revenant Grid v2 adds a
black/gunmetal shell, acid-green accents and borders, themed pressed states,
and a dedicated Quiet Shell background at low opacity behind secondary routes,
Quick Settings, premium sheets, and alert overlays. The mechanical-skull artwork
remains on Home; Home/AOD behavior is unchanged. Quiet Shell uses a fixed
410×502 RGB565 image embedded in the application, not the external asset volume.
Semantic healthy, warning, and error colors remain green, amber, and
red. Its non-overlapping safe-area touch map is declarative package data; the
shell maps only allowlisted actions to system navigation. Declarative weather
icon fields render bounded native shapes rather than font glyphs. Arbitrary-asset
filesystem loading remains deferred. The premium extension now imports bounded
JSON profiles through the phone: Personal complication assignments, palette,
Context Deck order and display flags, using only supported layouts/resources.
See [Premium build 4/5/6](PREMIUM_BUILD_456.md) for scope and physical acceptance.

## Release UX gates

- Offline watch face visible within 2.5 seconds.
- Visible touch response median below 80 ms and p95 below 150 ms.
- Normal transitions sustain at least 30 fps.
- No fake or silently stale metrics.
- Inactive screens stop refresh and sensor subscriptions.
- Large Text and Reduce Motion pass the same navigation tests.
- AOD has its own low-luminance layout and pixel shifting.

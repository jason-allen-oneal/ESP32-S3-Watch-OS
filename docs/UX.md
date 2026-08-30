# Obsidian Interface System

Obsidian is MorrowOS's AMOLED-first interface: black fields, restrained spectral
accents, high legibility, and no fabricated data. Cyber-goth atmosphere is a
texture, not an excuse for radioactive clutter.

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

## Design tokens

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

Grid is 4 px, side gutter 20 px, standard gaps 8/12/16/24 px, and cards use
12/16 px radii. Shadows and continuous decorative animation are forbidden.

## Customization

Users may change watch face, accent theme, complication assignments, launcher
favorites, glance-tile order, quick controls, handedness, text scale, wake
gestures, sound/haptic profile, AOD, and notification privacy without flashing.

Watch faces and themes are validated data packs with format version, resource
limits, compatible complications, and checksums. They cannot contain executable
code. A built-in fallback face and theme always remain available.

## Release UX gates

- Offline watch face visible within 2.5 seconds.
- Visible touch response median below 80 ms and p95 below 150 ms.
- Normal transitions sustain at least 30 fps.
- No fake or silently stale metrics.
- Inactive screens stop refresh and sensor subscriptions.
- Large Text and Reduce Motion pass the same navigation tests.
- AOD has its own low-luminance layout and pixel shifting.

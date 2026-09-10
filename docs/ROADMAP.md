# Nightglass Roadmap

The 2026-09-05 implementation of separately numbered premium backlog items
**4, 5 and 6** is tracked in [Premium build 4/5/6](PREMIUM_BUILD_456.md). This is
source/build work, not installed-watch or release-gate acceptance. Milestone
numbers below are the original project roadmap, not that premium backlog.

1. **Foundation** — native build, 32 MB partitions, health registry, event hub,
   service/app contracts, fallback shell, deterministic artifacts.
2. **Board diagnostics** — PMIC, RTC, touch, display, IMU, optional-actuator
   probe, on-watch diagnostics and USB report.
3. **Modern shell** — Obsidian components, navigation, launcher, quick settings,
   notification shell, fallback watch face.
4. **Watch essentials** — timezone/DST, alarms, persistent timers, stopwatch,
   and visual plus audio system alerts. Haptic feedback remains hardware-dependent.
5. **Power and activity** — measured sleep/wake, battery policy, steps,
   tap/raise wake, activity summaries.
6. **Connectivity** — BLE provisioning and companion protocol, asynchronous
   Wi-Fi/SNTP/weather, notifications and media controls. Dedicated Spotify and
   Discord watch surfaces now broker active media/notification actions through
   the phone without placing provider credentials on the watch.
7. **Audio and storage** — alarms, recorder, playback, SD browser, privacy.
8. **Customization** — external face-pack loading, complication assignment,
   themes, glance tiles, and import/export. The built-in manifest registry and
   persistent face selection already ship in the daily-use slice.
9. **Release hardening** — inactive-slot streaming, rollback health gate, and
   basic safe mode plus a dormant P-256 verifier are implemented. Production
   public-key provisioning, authenticated companion transport/UI, soak tests,
   diagnostic export, and the optional secure-boot plan remain.

Every milestone produces a tagged known-good artifact. A feature is incomplete
until degraded hardware, restart persistence, sleep/resume, and rollback paths
have been exercised.

## Current board-diagnostics slice

RTC, AXP2101 battery telemetry, and QMI8658 motion telemetry passed physical
hardware validation behind the shared hardware service. The P1/P2 haptic path
is electrically controllable but this unit has no responding actuator; ALDO3 is
kept off and the capability is hardware-deferred. SD and audio are assigned to
the dedicated audio/storage milestone rather than blocking the diagnostics
core release.

## Current shell and power slice

The daily shell provides a persistent root, bounded Home/Launcher/App
navigation, a live Context Deck, timezone/DST and 12/24-hour settings, a daily
alarm, countdown, stopwatch, visual/audio system alerts, lifecycle-bound
diagnostics, and persistent selection between built-in declarative watch-face
packs. Top/bottom edge swipes open Quick Settings and notifications, horizontal
edge swipes provide system Back, and the short side button follows the
Home/launcher convention. Every surface uses the rounded panel safe area and
lists only working features.

Brightness, dim, blank, and light-sleep delays are configurable and persisted.
Any touch resets inactivity; the first touch while blank is consumed as
wake-only. GPIO38 touch and GPIO10 side-key wake have passed physical light-
sleep validation. Timer/alarm deadlines arm timer wake. Deep sleep, motion
wake, direct RTC-alarm wake, and rail gating remain deferred.

The QMI8658 gesture slice implements bounded raise, double-twist, shake, and
flick recognition with persistent per-action settings and on-watch diagnostic
counts. Its guided calibration records three examples per gesture, rejects
incomplete runs, requires a clean false-trigger window, and saves a bounded
versioned sensitivity profile for the fitted watch. Actions remain disabled by
default. Raise-to-wake currently keeps CPU-side motion sampling available;
low-power INT1 wake-on-motion remains a later hardware gate.

## Premium interaction pass

The Android companion now exposes a live/stale secure-link state and a
one-tap reconnect action. Link status is persisted only as non-secret display
metadata; reconnect never clears the bonded watch identity or connection
tokens. Once an identity is pinned, a lifecycle receiver restores the service
after phone reboot/app update, and a Bluetooth-state receiver retries the link
immediately after the adapter returns. The Context Deck is a vertically
scrollable surface with readable wrapped details for all five cards, rather
than a fixed stack that clips the bottom card on the 410x502 panel.

The next premium gates are measured deep-sleep/charging behavior, background
reconnection across phone and watch restarts, audio and storage hardware-in-
loop validation, and production OTA transport, signing, and recovery UI. A
manual always-redact notification mode is sufficient; a PIN lock is outside
the product scope.

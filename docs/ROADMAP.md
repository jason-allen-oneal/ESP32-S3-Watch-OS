# Nightglass Roadmap

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
   Wi-Fi/SNTP/weather, notifications and media controls.
7. **Audio and storage** — alarms, recorder, playback, SD browser, privacy.
8. **Customization** — external face-pack loading, complication assignment,
   themes, glance tiles, and import/export. The built-in manifest registry and
   persistent face selection already ship in the daily-use slice.
9. **Release hardening** — inactive-slot streaming, rollback health gate, and
   basic safe mode are implemented. Production signature verifier/key,
   companion transport/UI, soak tests, diagnostic export, and the optional
   secure-boot plan remain.

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
navigation, timezone/DST and 12/24-hour settings, a daily alarm, countdown,
stopwatch, visual/audio system alerts, lifecycle-bound diagnostics, and persistent
selection between built-in declarative watch-face packs. Every surface uses
the rounded panel safe area and lists only working features.

Brightness, dim, blank, and light-sleep delays are configurable and persisted.
Any touch resets inactivity; the first touch while blank is consumed as
wake-only. GPIO38 touch and GPIO10 side-key wake have passed physical light-
sleep validation. Timer/alarm deadlines arm timer wake. Deep sleep, motion
wake, direct RTC-alarm wake, and rail gating remain deferred.

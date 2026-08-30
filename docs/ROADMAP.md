# Nightglass Roadmap

1. **Foundation** — native build, 32 MB partitions, health registry, event hub,
   service/app contracts, fallback shell, deterministic artifacts.
2. **Board diagnostics** — PMIC, RTC, touch, display, IMU, optional-actuator
   probe, on-watch diagnostics and USB report.
3. **Modern shell** — Obsidian components, navigation, launcher, quick settings,
   notification shell, fallback watch face.
4. **Watch essentials** — timezone/DST, alarms, persistent timers, stopwatch,
   sound/haptic feedback.
5. **Power and activity** — measured sleep/wake, battery policy, steps,
   tap/raise wake, activity summaries.
6. **Connectivity** — BLE provisioning and companion protocol, asynchronous
   Wi-Fi/SNTP/weather, notifications and media controls.
7. **Audio and storage** — alarms, recorder, playback, SD browser, privacy.
8. **Customization** — complications, watch-face manifests, themes, glance
   tiles, import/export.
9. **Release hardening** — signed OTA, rollback, safe mode, soak tests,
   diagnostic export, optional secure-boot plan.

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

The first daily shell provides a persistent root, bounded Home/Launcher/App
navigation, a real RTC watch face, battery/charging and motion complications,
an About view, and lifecycle-bound diagnostics. Every surface uses the rounded
panel safe area and lists only working features.

The power supervisor requests 30% active brightness, 8% after 15 seconds, and
a zero-brightness screen blank after 30 seconds. Any touch resets inactivity;
the first touch while blank is consumed as wake-only. GPIO10 side-key input is
debounced and also wakes the screen. CPU light sleep, deep sleep, motion wake,
RTC alarm wake, and rail gating remain disabled pending isolated HIL.

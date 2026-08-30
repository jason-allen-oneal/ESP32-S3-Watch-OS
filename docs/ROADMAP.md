# MorrowOS Roadmap

1. **Foundation** — native build, 32 MB partitions, health registry, event hub,
   service/app contracts, fallback shell, deterministic artifacts.
2. **Board diagnostics** — PMIC, RTC, touch, display, IMU, haptic probe, SD,
   audio loop, on-watch diagnostics and USB report.
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

RTC, AXP2101 battery telemetry, QMI8658 motion telemetry, and bounded haptic
testing are implemented behind the shared hardware service. SD and audio remain
deliberately outside this release and keep milestone 2 open.

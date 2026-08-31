# Nightglass

Nightglass is a modular, offline-first watch firmware for the Waveshare
ESP32-S3 Touch AMOLED 2.06 V1.0. It is a native ESP-IDF/LVGL product, not an
Arduino demo sketch and not a dynamically loaded desktop-style operating
system.

## Product principles

- Reach a functional offline watch face before optional services start.
- Never fabricate status, sensor, weather, connectivity, or battery data.
- Apps use services; services use board drivers; drivers never touch LVGL.
- Only the UI task may call LVGL.
- Inactive apps stop redraws, timers, and sensor subscriptions.
- Customization is data-driven and validated; removable storage is never
  allowed to inject executable code.
- Every release is reproducible, rollback-capable, and hardware-tested.

## Pinned foundation

- ESP-IDF `v5.5.5`
- LVGL `9.5.0`
- Waveshare board BSP `1.0.7` for hardware V1.0
- Target `esp32s3`, 32 MB QIO flash, 8 MB OPI PSRAM

## Layout

- `components/nightglass_core`: result types, health, events, and scheduling contracts
- `components/nightglass_bsp`: board adaptation and hardware probes
- `components/nightglass_services`: single-owner hardware tasks and immutable snapshots
- `components/nightglass_ui`: Obsidian design system and system shell
- `main`: bounded bootstrap only
- `docs`: architecture, UX, hardware support, release gates

## Safety boundary

The known-good Arduino firmware and the full 32 MB recovery image remain
outside this repository. Nightglass must compile and pass partition and artifact
checks before any device write.

## Build and release gate

```bash
./scripts/build.sh
./scripts/verify-release.sh
```

The release gate requires a clean tree, performs a deterministic double build,
validates the partition and ESP32-S3 image formats, enforces the OTA-slot size
boundary, and prints the exact flash-artifact hashes.

The runtime inactive-slot backend, fail-closed signature interface, rollback
health gate, safe-mode policy, and unsigned package tooling are documented in
[`docs/UPDATE_RECOVERY.md`](docs/UPDATE_RECOVERY.md). No update transport or
production signing key/verifier is included yet, so default firmware refuses
OTA installation rather than treating unsigned images as signed.

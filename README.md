# MorrowOS

MorrowOS is a modular, offline-first watch firmware for the Waveshare
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

- `components/morrow_core`: result types, health, events, scheduling, services
- `components/morrow_bsp`: board adaptation and hardware probes
- `components/morrow_ui`: Obsidian design system and system shell
- `main`: bounded bootstrap only
- `docs`: architecture, UX, hardware support, release gates

## Safety boundary

The known-good Arduino firmware and the full 32 MB recovery image remain
outside this repository. MorrowOS must compile and pass partition and artifact
checks before any device write.

Recovery image:

`__RECOVERY_IMAGE_OUTSIDE_REPOSITORY__`

Checksum manifest:

`__RECOVERY_CHECKSUM_OUTSIDE_REPOSITORY__`

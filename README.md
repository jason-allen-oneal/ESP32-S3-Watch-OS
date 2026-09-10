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
export NIGHTGLASS_PROJECT_VERSION=0.2.4
export NIGHTGLASS_SECURE_VERSION=4
./scripts/verify-signed-release.sh build/update-package-0.2.4
```

The release gate requires a clean tree, performs a deterministic double build,
validates the partition and ESP32-S3 image formats, enforces the OTA-slot size
boundary, and prints the exact flash-artifact hashes.

The runtime inactive-slot backend, provisioned P-256 verifier, rollback health
gate, safe-mode policy, native USB installer, and encrypted companion OTA are
documented in [`docs/UPDATE_RECOVERY.md`](docs/UPDATE_RECOVERY.md). Both install
paths accept the same four-file signed package and reject unsigned, same-version,
or rollback-ineligible images before the inactive slot is erased.

Direct watch-cable installation does not use the ROM flasher or the phone:

```bash
python3 scripts/nightglass-usb-update.py build/update-package-0.2.4 \
  --device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...
```

Phone OTA remains available through the Nightglass companion's signed-package
picker. Generic ESP-IDF flash and erase targets stay blocked because they can
overwrite the wrong slot or destroy rollback metadata.

An existing 0.2.2 watch needs a one-time migration before it can receive native
USB updates. `scripts/nightglass-rom-bootstrap.py` installs signed 0.2.4 through
the watch cable while preserving NVS and the valid rollback image; see
[`docs/USB_RELEASE.md`](docs/USB_RELEASE.md) for its exact prerequisites.

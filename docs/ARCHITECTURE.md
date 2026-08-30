# MorrowOS Architecture

## Dependency direction

```text
apps and shell -> service interfaces -> services -> driver interfaces -> BSP
```

Reverse calls are forbidden. Hardware work returns explicit status values.
Constructors perform no I/O, applications do not create private tasks, and
drivers do not know that LVGL exists.

## Boot phases

1. Record reset/wake cause and initialize bounded serial logging.
2. Evaluate boot-failure counters and recovery-button policy.
3. Bring up PMIC, buses, display, touch, RTC, and the fallback UI with deadlines.
4. Validate versioned settings and apply transactional migrations.
5. Start time, alarm, power, input, UI, and health services.
6. Probe IMU, haptics, SD, codec, microphones, and speaker asynchronously.
7. Start BLE, Wi-Fi, SNTP, weather, and update work outside the boot path.
8. Mark a pending OTA image valid only after the sixty-second health gate.

No optional peripheral may prevent the watch face from appearing. Three
consecutive unhealthy boots enter safe mode with custom assets, networking,
audio, SD, and nonessential apps disabled.

## Runtime ownership

- UI/LVGL task: all LVGL calls and foreground app lifecycle.
- Supervisor: power policy, health, watchdog, sleep transitions.
- Service worker: scheduled jobs, low-rate sensors, event delivery.
- Network worker: Wi-Fi, HTTP, SNTP, weather, OTA downloads.
- Storage/logger: coalesced writes, diagnostics, SD operations.
- Audio task: created only while capturing or playing.

Sensor and audio streams use ring buffers. The event hub carries fixed-size
state transitions, never high-rate samples or heap-owning callbacks.

## Application contract

Compiled applications register a descriptor containing stable ID, title, icon,
required/optional capabilities, permissions, routes, optional tile, and
optional complication providers. Their lifecycle is:

```text
create -> enter -> pause/resume -> exit -> destroyView
```

Only the foreground application owns a live screen. Background behavior belongs
to services so alarms, timers, activity, audio, connectivity, and notifications
survive view destruction.

## Persistence

Settings use two versioned NVS slots per namespace with generation number,
payload length, and CRC32. The inactive slot is written and verified before the
active generation changes. Migrations are sequential and preserve the previous
slot until the new firmware passes its health gate.

- NVS: authoritative settings, alarms, deadlines, boot state.
- LittleFS: validated faces, themes, assets, cached weather.
- microSD: recordings, media, exports, long history; never required to boot.
- Firmware: fallback face, theme, icons, diagnostics, and recovery UI.

## Power model

```text
BOOT -> ACTIVE -> DIM -> AMBIENT -> DISPLAY_OFF -> LIGHT_SLEEP
                                      |             |
                                      +-> DEEP_SLEEP (shipping/critical battery)
```

Normal standby begins with light sleep because touch GPIO38 and RTC GPIO39 are
not ESP32-S3 RTC IO. QMI8658 INT1 on GPIO21 can wake deep sleep after its live
interrupt behavior is validated. Networking, audio, amplifier, SD, display,
and sensor features are explicitly gated by the power service.

## Update and recovery

The 32 MB partition table supplies two 6 MB OTA slots, OTA metadata, NVS,
coredumps, and a 19.375 MB asset filesystem. An update must verify board ID,
partition revision, size, SHA-256, and signature before selecting the inactive
slot. Bootloader rollback remains armed until the runtime health gate succeeds.

Secure Boot V2 and flash encryption remain deferred because their eFuse changes
are irreversible. They require a separately approved production-recovery plan.

# Nightglass Architecture

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

## Milestone 2 hardware slice

The current diagnostics release adopts the I2C bus created by the Waveshare
BSP. One pinned hardware-service task is the only Nightglass owner of RTC, PMIC,
and IMU transactions. UI code receives trivially copyable snapshots guarded by
a cross-core critical section and never performs I2C or GPIO work.

- PCF85063 time is sampled once per second and validated before display.
- AXP2101 status, battery voltage, and fuel-gauge percentage are sampled every
  two seconds. Charge current, voltage targets, rails, and shutdown policy are
  untouched; the only PMIC write enables its battery-voltage ADC channel with a
  read-modify-write.
- QMI8658 is reset into known state, identified, configured for 8 g at
  31.25 Hz and 512 dps at 28.025 Hz, and sampled at 25 Hz. At each boot, the
  gyro discards one second of startup data, then collects 500 stationary
  samples (about 20 seconds), rejects moving or
  noisy calibration windows, and stores an in-memory zero-rate bias for each
  axis. Runtime values are bias-corrected, low-pass filtered, and displayed as
  zero inside a +/-0.5 dps deadband. Motion is explicitly a heuristic, not step
  count or orientation. Because constant rotation cannot be distinguished from
  sensor bias without an external reference, the watch must remain still until
  calibration completes; the UI never labels gyro data live before that point.
- GPIO18 starts low. The schematic's P1/P2 motor path and its ALDO3 supply were
  electrically exercised, but this physical unit produced no mechanical
  response and Waveshare does not list an installed actuator. Haptics are
  therefore hardware-deferred and ALDO3 remains disabled. The bounded pulse
  implementation stays dormant for a future fitted actuator.

All I2C operations have a 20 ms deadline. A missing optional peripheral degrades
its card but does not block display startup. The diagnostics view recomputes
sample age independently of service updates so a stalled producer becomes
visibly stale instead of freezing as apparently live data.

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

Power, clock, alarm, timer, and selected-face state are currently stored in
bounded NVS namespaces with validation and safe defaults. Two-slot generation
records with CRC32 remain the hardening target before external face imports or
signed OTA depend on schema migration.

- NVS: authoritative settings, alarms, deadlines, boot state.
- LittleFS: validated faces, themes, assets, cached weather.
- microSD: recordings, media, exports, long history; never required to boot.
- Firmware: fallback face, theme, icons, diagnostics, and recovery UI.

## Power model

```text
BOOT -> ACTIVE -> DIM -> SCREEN_BLANK -> LIGHT_SLEEP
                                             |
                                             +-> DEEP_SLEEP (later shipping mode)
```

The power supervisor steps through configurable active, dim, blank, and light-
sleep states. GPIO38 touch wake and GPIO10 EXT1 side-key wake are armed only
around sleep, then normal touch interrupt behavior is restored after resume.
The first wake touch is consumed before object events. Alarm and countdown
deadlines also arm timer wake.

The side power key is GPIO10 (`SYS_OUT`, active high); GPIO0 remains the boot
strap. Touch IRQ GPIO38 and RTC IRQ GPIO39 are digital-only and can wake light
sleep, while GPIO10 and QMI8658 INT1 GPIO21 are RTC IO candidates for later
deep-sleep wake. Motion wake, direct RTC alarm wake, panel DCS 0x28, deep sleep,
and PMIC rail gating remain disabled until their isolated wake-loop and recovery
tests pass. Networking, audio, amplifier, SD, display, and sensor features are
eventually gated by the power service.

## Update and recovery

The 32 MB partition table supplies two 6 MB OTA slots, OTA metadata, NVS,
coredumps, and a 19.375 MB asset filesystem. An update must verify board ID,
partition revision, size, SHA-256, and signature before selecting the inactive
slot. Bootloader rollback remains armed until the runtime health gate succeeds.

Secure Boot V2 and flash encryption remain deferred because their eFuse changes
are irreversible. They require a separately approved production-recovery plan.

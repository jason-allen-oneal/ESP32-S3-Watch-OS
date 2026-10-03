# Battery implementation status — 2026-10-03

This is a source-only first pass, not a measured battery-life release. No watch
write, signing, current measurement, or physical wake test has been performed.
The older POWER_OPTIMIZATION_SPEC.md includes targets and historical problems;
its current/runtime numbers must not be presented as measured results.

## Implemented

- New-install power defaults: 20% active brightness, 8% dim, dim at 5 seconds,
  blank at 15 seconds. Existing NVS preferences are preserved, not silently
  migrated. AOD and gesture preferences are also preserved.
- Standby RTC sampling: 30 seconds; active sampling remains 1 second. Clock
  time extrapolates from the sample using monotonic elapsed time. RTC freshness
  allows 35 seconds, while a failed read still invalidates the sample. The
  first hardware iteration always reads RTC and battery, even when blank.
- Standby battery sampling: 15 seconds; active sampling remains 2 seconds.
  This stays below AOD's existing 30-second battery freshness limit. Charging
  detection while blank can take up to 15 seconds; no PMIC interrupt is assumed.
- Activity processing is notified after each hardware motion publication.
  Its independent 25/10 Hz schedule is removed; a 1-second fallback preserves
  stale detection, day rollover and persistence retries. Commands also notify
  immediately. Sensor polling rates and the gesture algorithms are unchanged.
  This is not sensor FIFO support or a step-accuracy improvement.
- OTA transport waits indefinitely when idle and retains bounded timeout
  checks during active transfer sessions.
- Weather waits for settings/phone/radio/display notifications or the next
  fetch/freshness deadline. Disabled or blank operation with no freshness
  deadline waits indefinitely. Wi-Fi startup/DHCP still has a bounded retry.
  Snapshot reads update weather age so UI freshness does not depend on polling.
- Corrected the power comment claiming automatic light sleep was active.

## Not implemented: physical validation is required

1. **Connected automatic CPU light sleep.** GPIO38 is currently a normal
   falling-edge touch interrupt. Level wake changes interrupt configuration;
   restoring safe race-free hand-off needs hardware tests. Automatic CPU
   sleep remains disabled, and manual sleep remains forbidden while BLE is
   enabled. BLE modem sleep remains configured. Do not remove that guard.
   The existing manual sleep path still pauses motion sampling, so this pass
   does not claim uninterrupted step tracking during manual sleep.
2. **QMI8658 FIFO / low-power raise-to-wake.** Validate exact chip revision,
   FIFO timestamp/batch behavior and GPIO21 INT1 wiring before changing the
   backend. Compare walking/running counts and gesture traces against the
   existing pipeline. Do not power-gate gyro while the current gyro-dependent
   gesture recognizer needs it.
3. **Adaptive BLE intervals.** Measure negotiated (not merely requested)
   parameters, bonded idle reliability, notification delay and voice/OTA
   throughput. Retain the stable main-crystal sleep clock.
4. **AOD minute-only refresh / diagnostics / USB event handling.** Retain the
   current touch recovery fallback. Diagnostic reduction was not retained
   because existing recovery regression coverage depends on it. Event-driven
   USB handling must preserve connection and update-envelope recovery.
5. **Rail gating and deep sleep.** Need exact board schematic and battery-path
   current measurements. Never gate shared touch/IMU rails speculatively.

## Hardware acceptance matrix

Measure at the battery path with USB disconnected; USB inhibits sleep and
cannot establish real standby current. Record battery capacity, firmware hash,
saved power/gesture/AOD settings and negotiated BLE parameters for each run.

| Case | Required checks |
| --- | --- |
| Blank, BLE disabled / connected / advertising | Average current; notifications; reconnect |
| Gestures disabled / enabled | Wake reliability; gyro current; walking/running counts |
| AOD disabled / enabled | Current; battery freshness fallback; physical wake |
| Active brightness 15 / 20 / 30% | Readability and average current |
| Weather / voice / OTA | Return to idle; no persistent radio/audio PM lock |
| Sleep transitions | 100+ touch cycles, held touch, button, alarm/timer deadlines |
| Bonded overnight | No reboot, disconnect loop or stranded blank display |

Firmware builds and host tests are necessary but cannot replace these gates.
No claimed hours-of-runtime gain should be published before measurements.

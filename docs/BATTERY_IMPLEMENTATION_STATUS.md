# Battery implementation status — 2026-10-03

Signed **0.2.15 / secure15** is installed on **ota_1**, VALID, pending=0.
Native USB installation completed without erasing NVS and preserved 0.2.14 as
its rollback image. The unchanged health gate accepted at uptime 61.543 seconds;
a subsequent live USB query confirmed the accepted version and partition.
Firmware SHA-256: `fa992b5ff204ea48e37f3d834954c5d11419acceb7aaaf359f537caa71f984c8`.
Release checks included host tests, deterministic double build, image/partition
validation and signed-package verification. BLE reconnected and activity/gyro
calibration completed. Existing intermittent touch I2C errors remain visible;
physical tap/swipe acceptance is pending the user's check.

Evidence is stored owner-only in
`/home/rev/projects/hardware/nightglass-evidence/battery-0.2.15-20261003/`.
No battery-current measurement or runtime gain is claimed.
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

## Standby candidate 0.2.16 — implementation complete, HIL pending

- Automatic CPU light sleep is enabled through paired IDF 5.5.5 PM callbacks.
  Sleep is vetoed unless the display is blank, the configured sleep delay has
  elapsed, USB is disconnected, and no touch/wake fault is present. Existing
  BLE modem sleep and connection scheduling are preserved.
- The GPIO38 falling-edge interrupt is masked before arming low-level wake,
  then level wake is disabled and falling-edge IRQ restored after every sleep
  attempt. Already asserted contacts veto sleep; contacts racing entry/exit
  latch a wake-only touch for the power supervisor. Setup/restoration failure
  disables subsequent automatic sleep and reports degraded health.
- The legacy unbounded manual sleep path is bypassed while automatic sleep is
  configured, so hardware/activity/clock task deadlines keep running. This
  prevents the previous indefinite manual-sleep sampling gap, but sensor FIFO
  and hardware step counting are not implemented.
- PM profiling is enabled for this candidate. USB reattachment reports
  AUTO_SLEEP_EVIDENCE and IDF successful/rejected sleep counts plus PM locks.
  Callback duration counters alone are not proof of successful entry; IDF's
  successful sleep counts are the acceptance evidence.
- Raise-only gesture configurations use an accelerometer tilt recognizer
  requiring arm-down arming, real orientation change and a 250 ms settled
  face-up finish. The gyro is power-gated in that configuration. Enabling any
  twist/shake/flick gesture, gyro diagnostics or calibration retains gyro.
  Existing gyro macro recognizers and saved calibration are not removed.
- Host coverage exercises callback races, each GPIO failure boundary, 1,000
  hand-offs, 25/10 Hz raise traces, stationary/impact/gap rejection, and the
  unchanged touch recovery coverage. Physical connected sleep, touch/button,
  tilt false-positive/latency, and walking accuracy remain hardware gates.

## Still pending

1. **QMI8658 FIFO / interrupt sampling.** Validate exact chip revision, FIFO
   timestamp/batch behavior and GPIO21 wiring before changing the backend.
   Current sensor polling rates are unchanged.
2. **Adaptive BLE intervals.** Measure negotiated parameters, bonded idle
   reliability, notification delay and voice/OTA throughput. Retain the stable
   main-crystal sleep clock.
3. **AOD minute-only refresh / diagnostics / USB event handling.** Retain touch
   recovery coverage and USB connection/envelope recovery.
4. **Rail gating and deep sleep.** Need exact schematic and battery-path
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

## 0.2.16 installation and 0.2.17 correction

Signed 0.2.16/secure16 installed on ota_0 and passed the unchanged health gate
at uptime 61.542 seconds. User previously confirmed physical wake/swipe on
0.2.15. No unplugged sleep claim is made for 0.2.16.

Boot exposed an 8,800-byte internal-memory allocation failure for optional CPU
retention. IDF 5.5.5 esp_pm_configure ignores the return from its sleep setup;
a failed CPU-retention init also skips modem sleep configuration in that call.
0.2.17 therefore disables CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP, enforced by
the release verifier, retaining ordinary automatic light sleep without this
optional allocation. Physical sleep/wake/BLE evidence is required after install.

Signed 0.2.17/secure17 installed on ota_1, VALID pending=0, accepted at
uptime 61.543 seconds. Fresh native USB identity confirmed the installed
version and partition. No ALLOCATION_FAILED or CPU-retention error appeared
in the candidate boot. Firmware SHA-256:
`c4393ac165667447b48386c52f5fdb86403f2fb751c8a1f1332fad30f0e48aed`.
Owner-only evidence: `/home/rev/projects/hardware/nightglass-evidence/standby-0.2.17-20261003/`.
Unplugged touch/BLE/sleep-counter acceptance is still pending.

# Wake stability repair — 2026-09-05

**Installed and accepted:** 0.2.11 / secure11, ota_1 VALID pending=0.
the owner confirmed a physical center tap wakes the fully blank display normally.
UART evidence records the touch, display wake, and same valid image afterward;
no reboot occurred. Monitoring is closed and the watch can be disconnected.

## Changes

- Move wake redraws off the overflowing power-task stack onto LVGL.
- Encode SH8601 QSPI sleep/wake commands correctly.
- Allow 100 ms controller recovery before the first initialization writes.
- Bound touch I2C waits and recover from idle NACK through a bounded sensor-bus
  quiet window, verified supported mode handling, and a genuine LVGL read.

Health thresholds, signatures, rollback, and NVS preservation remain intact;
no eFuses changed. Normal hardware polling resumes after recovery. Measured
quiet windows were about 1.3–3.5 seconds; this is a personal-watch repair, not
full long-duration production qualification.

## Evidence

Firmware SHA256:
`5e83c479c95a43acb42b267e03efb099262bb20cf40e7430f055dbcbb884ebb9`

[Investigation and exact installation evidence](~/projects/watch/backups/nightglass-wake-20260905-pbdibkns/REPORT.md)

The build and focused checks passed. At the owner's instruction, no broad/repeated
suites were run for the final changes. Source was integrated with baseline hash
guards and backups, preserving unrelated WIP. No commit or push was made.

# Upper-circle alignment — accepted 0.2.13

The personal watch is running signed **0.2.13 / secure13 on ota_1, VALID, pending=0**. This follows the owner's wrist-photo feedback about aligning the step and weather information.

Both icons now use 44×28 frames at y=100. Their values use centered 68×26 labels at y=132, sharing a baseline. The footprint geometry is centered within its frame. The skull and Quiet Shell artwork, AOD, and previous touch/wake fixes are unchanged.

Independent layout/native-font and implementation review found no blockers. Native LVGL host previews with sample data confirmed normal and Large Text moon/rain/storm fit; these are not device screenshots. One target build passed. The on-device updater verified inactive-slot readback. The signed native USB installer observed exact pending 0.2.13 identity, unchanged one-minute boot-health acceptance, and a fresh accepted identity, then closed its serial observer.

- Firmware: 3,128,368 bytes.
- SHA-256: `e3272a89f0966b6322053ee536ed630bff07b841a574b933e11e6acec87cc1fd`.
- Build snapshot: `~/projects/watch/nightglass-dial-alignment-20260905`.
- [Full evidence](~/projects/watch/backups/nightglass-dial-alignment-20260905/REPORT.md).
- [Native before/after preview](~/projects/watch/backups/nightglass-dial-alignment-20260905/preview/README.md).
- [Accepted device identity](~/projects/watch/backups/nightglass-dial-alignment-20260905/installation-health-0.2.13.json).
- [Source integration receipt](~/projects/watch/backups/nightglass-dial-alignment-20260905/source-integration-0.2.13.json).

Only two reviewed source files were integrated after hash guards, with originals backed up and unrelated WIP preserved. No NVS erase, separate asset write, key/eFuse change, raw flash, commit, or push occurred. On-wrist visual acceptance, physical touch qualification, and long-term soak results are not claimed for this update.

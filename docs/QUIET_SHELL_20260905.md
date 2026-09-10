# Quiet Shell — installed September 5, 2026

Signed Nightglass **0.2.12 / secure12** is accepted on **ota_0**, VALID,
pending=0. This supersedes the earlier signing-input blocker. the owner authorized
the flash, then explicitly requested the successful signing retry.

The dedicated 410×502 RGB565 image gives secondary screens a restrained
gunmetal/green perimeter and empty dark center. It is embedded in the app,
including for Quick Settings, premium sheets, and alerts. Home keeps its
existing skull artwork; AOD, native controls, and the 0.2.11 wake fixes remain
unchanged. No user settings/pairing reset, NVS erase, key change, eFuse change,
or separate asset-volume write occurred.

One target build and focused schema/wake checks passed. Independent source,
asset, deployment, and wrapper reviews found no blockers. The approved retry
used the existing frozen build, without another build or broad test suite.

- Firmware size: 3,128,384 bytes.
- Firmware SHA-256: `7238cd9a9d11f0557cc8b00c92dfed0e2e526e8abbdc82fe9af068b9cfcd376b`.
- Readback verification, exact new pending version, unchanged 60-second health
  gate, and fresh accepted running identity all passed.
- USB installer exited successfully and closed the port.
- Ten reviewed source/test/asset files were copied into the canonical project
  after before-hash guards, with recoverable backups. Unrelated WIP remains.
- This is not on-wrist visual/touch acceptance or long-term qualification.

Build snapshot: `~/projects/watch/nightglass-shell-20260905`.

Evidence and backups:
`~/projects/watch/backups/nightglass-shell-20260905/REPORT.md`.
The directory also contains `installation-health-0.2.12.json`,
`installation-0.2.12.log`, `artifacts-0.2.12.json`, and
`source-integration-0.2.12.json`.

Original artwork, exact generation prompt, and code-based concept previews:
`docs/concepts/revenant-shell-v1/`.

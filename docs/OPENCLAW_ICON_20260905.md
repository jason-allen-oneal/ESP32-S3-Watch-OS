# OpenClaw face icon — accepted 0.2.14

The personal watch is running signed **0.2.14 / secure14 on ota_0, VALID, pending=0**. the owner requested a better OpenClaw icon on the existing face.

The icon is a 54×54 antialiased monochrome adaptation of OpenClaw's official mascot, preserving its silhouette, eyes, pupils, antennae, and claws. It uses a 2,916-byte embedded A8 mask and the existing voice-health tint. The 88×58 display slot, 112×74 OpenClaw action zone, and navigation route remain unchanged. No runtime SVG decoder, animation, scaling, or separate pixel buffer was added. Upstream source, commit/hash, regeneration command, and MIT license are preserved with the asset.

Independent implementation/deployment review and actual-descriptor native LVGL previews passed. All three state colors are clear, transparency preserves the background, and every native image-box hit resolves to the unchanged action zone. These are sample host-rendering/hit-resolution results, not device screenshots or physical-touch qualification. A compiler enum-type error was fixed with two separate flag-removal calls; the target build then completed incrementally without a clean rebuild or broad suite.

The on-device updater verified inactive-slot readback. The native USB installer observed exact pending 0.2.14 identity, unchanged one-minute health acceptance, and a fresh accepted identity, then closed its serial observer.

- Firmware: 3,131,296 bytes.
- SHA-256: `24af9026efdc18ba5c5a04575a2e29f96b5870e0777601a17adab1b6b3f224e0`.
- Build snapshot: `~/projects/watch/nightglass-openclaw-icon-20260905`.
- [Full evidence](~/projects/watch/backups/nightglass-openclaw-icon-20260905/REPORT.md).
- [Before/after native preview](~/projects/watch/backups/nightglass-openclaw-icon-20260905/preview/before-after-native.png).
- [Device acceptance](~/projects/watch/backups/nightglass-openclaw-icon-20260905/installation-health-0.2.14.json).
- [Source integration](~/projects/watch/backups/nightglass-openclaw-icon-20260905/source-integration-0.2.14.json).

Two modified files and six new source/asset/provenance/license files were integrated after hash guards, with originals backed up and unrelated WIP preserved. The upper-circle alignment, skull/Quiet Shell artwork, AOD, and previous wake fixes remain intact. No NVS erase, raw flash, separate asset write, key/eFuse/account change, commit, or push occurred. On-wrist visual acceptance, physical touch qualification, and long soak are not claimed for this update.

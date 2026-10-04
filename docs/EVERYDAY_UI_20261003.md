# Everyday UI implementation — 0.2.25 candidate

Implements the approved non-Home direction in real LVGL/Android code. All Home/face renderer functions were compared with accepted39df4f5 and are identical. Face selection, custom face import/export and profile persistence are untouched.

- Today/glances retain live data, configured ordering/visibility and readable cards; cards open their corresponding agenda/phone, activity, weather, media and notification destinations.
- Quick settings uses a two-column Quiet / Power / Phone / Settings grid, then brightness, watch sound and Bluetooth controls. Quiet selection is visibly accented. Power opens existing settings: no unimplemented battery-saver toggle.
- Notifications move privacy controls below the list, shorten status chrome and compact real message rows while retaining target-bound Read/Dismiss, detail, reply and privacy behavior. No fictional unread count or unsupported clear-all command.
- Voice keeps hold/release unchanged. During finishing/uploading/processing/playback the first-viewport primary area becomes Cancel/Stop. Recording stays visible so hiding a pressed control cannot terminate recording. No fake playback progress, replay file retention or phone audio routing.
- Media retains large transports and real progress, simplifies volume-step labels and moves secondary content under More media controls. The protocol only exposes volume steps, so no misleading absolute slider. Artwork remains on its existing supported content surface.
- Companion Watch page adds direct appearance/complication/glance-order entries into existing customization sections. Draft/apply, confirmed state, imports/backups and actual native preview remain. No invented favorites editor or replacement face.

Host checks passed. Cached Java21/Gradle8.9 Android assemble, tests and lint passed. Native LVGL off-device fixtures cover13renderer families in normal/Large Text with minimum56pixelbuttons and font pagination; fixtures stub service values/callbacks and are not physical acceptance. Firmware release/signing/install acceptance and phone installation to be recorded separately.

## Verified outcome / remaining installation

Firmware release gate passed sourcee16d542, candidate0.2.25 secure25, image
3a2dde13c8bc8b919e53586f12ccaf7e86a2eb4c15d7c85715463a702a1503a5.
Protected signer and independent signed-package verification passed.
USB installer exited1 before opening a port: watch by-id symlink absent;
only Samsung phone remained attached. No firmware transfer started.
The supported phone package picker did not complete an alternate transfer.
Watch remains on its previous firmware until cable connection/install is verified.

Companion palette follow-up6c3f37e built with99tests, zero failures/errors/skips,
and lint passed. Installed in place with matching certificate, no data clear.
Visually checked Watch tab black/neutral/lime styling and new entries.
Glance order shortcut was exercised and opened the actual editor.
No appearance Apply command, face/profile reset or Gateway configuration edit.
Evidence: nightglass-evidence/everyday-ui-20261003 outside repository.

# Everyday UI implementation — 0.2.25 candidate

Implements the approved non-Home direction in real LVGL/Android code. All Home/face renderer functions were compared with accepted39df4f5 and are identical. Face selection, custom face import/export and profile persistence are untouched.

- Today/glances retain live data, configured ordering/visibility and readable cards; cards open their corresponding agenda/phone, activity, weather, media and notification destinations.
- Quick settings uses a two-column Quiet / Power / Phone / Settings grid, then brightness, watch sound and Bluetooth controls. Quiet selection is visibly accented. Power opens existing settings: no unimplemented battery-saver toggle.
- Notifications move privacy controls below the list, shorten status chrome and compact real message rows while retaining target-bound Read/Dismiss, detail, reply and privacy behavior. No fictional unread count or unsupported clear-all command.
- Voice keeps hold/release unchanged. During finishing/uploading/processing/playback the first-viewport primary area becomes Cancel/Stop. Recording stays visible so hiding a pressed control cannot terminate recording. No fake playback progress, replay file retention or phone audio routing.
- Media retains large transports and real progress, simplifies volume-step labels and moves secondary content under More media controls. The protocol only exposes volume steps, so no misleading absolute slider. Artwork remains on its existing supported content surface.
- Companion Watch page adds direct appearance/complication/glance-order entries into existing customization sections. Draft/apply, confirmed state, imports/backups and actual native preview remain. No invented favorites editor or replacement face.

Host checks passed. Cached Java21/Gradle8.9 Android assemble, tests and lint passed. Native LVGL off-device fixtures cover13renderer families in normal/Large Text with minimum56pixelbuttons and font pagination; fixtures stub service values/callbacks and are not physical acceptance. Firmware release/signing/install acceptance and phone installation to be recorded separately.

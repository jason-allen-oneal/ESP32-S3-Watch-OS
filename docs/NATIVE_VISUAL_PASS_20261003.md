# Native visual pass — candidate 0.2.19 / secure 19

Ports the owner-approved balanced reference into ESP-IDF/LVGL. The native implementation adapts to real supported capabilities: Sound remains sound/DND plus codec checks, rather than inventing a recorder; media artwork/queue/output remains the existing content surface. Sample preview data is not compiled into firmware.

- Compact headers, true-black reading/tool pages, neutral dividers, borderless secondary actions and selective existing-profile accent. Existing Home skull art and AOD behavior retained.
- OpenClaw response, microphone mark and hold/release primary control in the first viewport. Duration, spoken replies, paging, cancellation and confirmed suggestions retained. Response pages are fitted with the real native font, including long words/newlines/UTF-8 and Large Text.
- Three large media transports; live play/pause indicator; volume next; seek, stop, restart, phone launch and content handoff retained below. Native progress uses the profile accent.
- Native countdown progress ring and large time with duration/start/pause/reset; independent stopwatch preserved.
- Quiet left-aligned grouped Settings and reordered Apps. Activity/weather summaries ahead of optional configuration; larger notification, reply, call and sound controls. Existing callbacks and target-binding retained.

## Verification before installation

A temporary native host renderer compiled the repository's LVGL 9.5 source and extracted the actual shell primitives/renderers. Thirteen renderer families ran in both normal and Large Text modes; their buttons passed the 56×56 minimum target check. Native-font response paging passed wide-word, multiline and UTF-8 cases. Actual services and callbacks were stubbed only in this off-device rendering harness. This is not a physical display capture or a service interaction test. Target firmware uses its existing RGB565 configuration; off-device captures use XRGB8888.

The four main screen native render contact sheets are in `concepts/visual-pass-20261003/native-normal.png` and `native-large.png`. Checked the native font geometry and corrected paging/action overlap and row alignment. The earlier 416-case browser check applies to the design reference, not to this firmware. Host suite and target build are checked again at the release gate. The Discord contract was updated solely for the approved short Message header, while retaining the navigation callback guard and all interaction/security assertions.

Read-only supported USB identity probe: version=0.2.18, secure=18, pending=0, ota_0, ota_state=2 (VALID). Native signed USB update will use the inactive slot and existing 60-second boot health gate; no NVS/bond reset, erase, key/eFuse changes or separate assets-volume write.

Installation is pending. Accepted identity and release hashes must be recorded after the signed install. Physical appearance/touch acceptance and another owner voice turn are not established by off-device checks.

## Installed and accepted

Signed native USB installation completed successfully. The exact pending 0.2.19 / secure19 boot passed the existing 60-second health gate (`essential_ok=1`, `safe_mode=0`) and emitted `OTA_HEALTH_ACCEPTED state=VALID pending=0`. A subsequent independent supported status probe confirmed `version=0.2.19 secure=19 pending=0 partition=ota_1 state_io=ESP_OK ota_state=2`. RTC/clock telemetry remained valid. USB installer exited 0 and released the port. No NVS erase or separate assets-volume write occurred.

- Firmware source commit: `1d6290a2431fabf259bfec2166cd4edf1ec61a2a`.
- Firmware: 3,136,960 bytes; SHA-256 `6b52dd6b485f2937a85788454f73e5eb546103fb511bfae74fad249fb86bd8f2`.
- Evidence: `/home/rev/projects/hardware/nightglass-evidence/native-visual-pass-20261003/` (release gate, signed package, installation log, fresh accepted identity, native renderer scripts/config).
- Off-device captures were taken at the timer-driven flush, separately from pagination stress checks, to avoid the temporary host fixture's double-refresh capture artifacts. The Large Text contact sheet was corrected; this documentation-only capture update does not change installed firmware.
- No phone is currently attached via ADB, so another post-update voice turn has not been independently exercised. The owner confirmed voice worked before this UI update; its capture/release/cancel semantics and service configuration were retained.
- On-wrist appearance and touch acceptance remain for the owner to judge on the actual watch; boot acceptance is not a claim of that visual review.

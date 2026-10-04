# Software polish — 0.2.26

Owner requested all four follow-through areas. Home/face/ambient renderer functions compare unchanged against the accepted 0.2.25 source; custom-face import, selection and persistence are untouched.

## Implemented

- About uses the running ESP application descriptor for firmware and secure version. Authenticated BLE status queries retain their existing 22-byte ACK and optionally precede it with bounded identity/health/error information (0x36). Older clients ignore the extension; older watches still work with the new companion.
- Companion saves update progress/outcomes across activity/process lifecycles. Validation/reboot is not labeled installed. Expected post-reboot identity plus cleared pending verification is required for success; another running version is reported explicitly. Actual rejection details replace ambiguous numeric codes when the firmware supports them. Interrupted process-local transfers say to reselect the signed package.
- Inbox groups by app, shows local watch unread count/state, and marks Details read without launching the phone. Read IDs are RAM-only and survive same-boot companion cache replay; changed alert content becomes unread again. Online Details opens the existing full-message content sheet directly (privacy permitting); offline cached previews remain. Target-bound supported actions/replies/dismissals remain available. No invented arbitrary app actions.
- Today respects the configured card visibility/order. Its first enabled next-up card surfaces timer/alarms in progress, active media, and otherwise the earlier scheduled alarm/calendar event. Labels and tap destinations follow the same current context. Alarm weekday is shown. Existing media glance stays intact.
- Voice retains full bounded paginated response text and concise spoken excerpt, with readable Listening/Sending/Thinking/Playing/Reply received states. Completed non-streaming speech can be replayed locally from its existing bounded RAM allocation. No file retention or new TTS request. Buffer ownership moves to the audio worker for replay and is wiped on cancellation, link loss, update preparation, new capture, or reboot. Streaming replies are not retained by this change. Replay is disabled when unavailable or busy.

## Checks / deployment

Host tests passed including weekly alarm-deadline cases. 102 Android unit tests and lint passed. Replay ownership tests execute extracted production paths under AddressSanitizer/UBSan with controlled audio queue: repeat, duplicate replay prevention, immediate enqueue failure, cancellation, secure wipe. Firmware build passed. Native LVGL fixtures passed for 16 renderer families in normal/Large Text, with minimum 56px controls and bounded reply pagination. Large Text notification status/count overlap was corrected and visually rechecked. Fixtures use service stubs, not hardware acceptance. Deterministic release verification, signing and OTA deployment are tracked below when complete.

These checks are not physical usability, listening or unplugged battery validation.

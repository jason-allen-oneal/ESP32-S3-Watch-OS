# Performance and premium-quality review — 2026-10-03

## Scope and evidence

Read-only review of installed-source firmware 0.2.23 (9a1ecdb), ESP-IDF/LVGL configuration, Android connection/voice paths, current native-render contact sheet, prior touch logs, and implementation/release documentation. No settings, firmware, phone app, or signing configuration changed. This is a prioritized source/design review, not an exhaustive line-by-line audit or fresh physical benchmark. The phone is not attached for profiling. Post-0.2.23 passive serial capture returned no output, so absence of errors is not established.

Preserve raise-only gesture selection, saved NVS/bonding, notification-sound opt-in, smooth buffered speech, bounded audio retention, existing artwork, and signed inactive-slot updates. The native black/ivory/accent design is already a strong base; premium means reliable response and coherent interaction before adding decoration.

## Ranked opportunities

| Priority | Opportunity | Evidence | Proposed approach / acceptance |
| --- | --- | --- | --- |
| P0 | Establish real idle power and latency baselines | USB vetoes normal automatic light sleep; no current/runtime gain has been measured. Existing UI metrics measure software rendering, not touch onset or panel scanout. | Record unplugged discharge and successful sleep counts, lock residency, display-on time, recovery count, and wake reason. Measure idle, raise-only, scrolling, voice and reconnect separately. Use external current measurement if available; do not infer mA from percentages. |
| P0 | Resolve touch recovery stalls rather than masking them | Prior live sleeping touch failures led to ~3.5-second recovery windows while hardware polling was paused. 0.2.23 suppresses sleeping timer probes but is not physically proven to eliminate all faults. | Determine whether remaining failures originate in controller sleep, bus ownership, timeout, or real IRQ behavior. Keep bounded recovery and honest health reporting. Acceptance: repeated sleep/wake/scroll cycles without multi-second stalls or lost raises. |
| P1 | Reduce unnecessary UI work | shell.cpp refreshes many labels/styles on route timers; route changes destroy/recreate the content tree, and notifications can trigger a rebuild. Every ordinary route gets a 120 ms content-opacity fade. | Add value/sequence-based updates, defer noncritical updates during active scrolling, preserve scroll position across data changes, and retain selected expensive screens only within a bounded memory budget. Compare fade off/on; use small local transitions only if measurements support them. Do not indiscriminately cache every route. |
| P1 | Improve scrolling through measured rendering tuning | Existing 30 ms refresh interval, eight-row RGB565 DMA buffers, software rotation/byte swapping, two software draw units. 0.2.23 adds a second buffer; physical benefit remains unverified. | Profile drawing vs transfer vs input stalls per route. Evaluate buffer sizes, draw-unit count and rotation/conversion cost individually using existing LVGL/ESP-IDF facilities. Larger buffers or higher refresh are experiments, not defaults: retain SRAM, BLE and rollback margins. Target sustained 30 fps first (~33 ms/frame), with frame p95 and input-to-visible response measured separately. |
| P1 | Remove avoidable standby wakeups | hardware.cpp currently publishes RTC every second in both active and blank states, despite older documentation describing 30-second standby reads. Blank motion polling remains 100 ms; clock freshness accepts 35 seconds. | Consider restoring slower blank RTC reads with monotonic extrapolation and alarm/time-change tests. Measure each change. Longer-term investigate QMI8658 FIFO/interrupt-assisted motion only after verifying physical wiring and sensor behavior; never trade away working raise-to-wake by simply reducing polling. |
| P1 | Make voice latency understandable and controllable | Current full buffering fixes starvation; acknowledged BLE throughput was below the 8 kB/s mu-law playback consumption. Gateway spoken excerpt is bounded to 800 characters; audio is bounded to 60 seconds. High connection priority begins before synthesis. | Show precise Listening / Sending / Thinking / Preparing audio / Playing states, immediate text availability where possible, and one clear cancel action. Consider moving high-priority BLE to actual transfer. Offer a short spoken summary with full text, or explicitly labeled continuation. Do not re-enable streaming until measured delivery has margin above consumption or a safe adaptive buffering design is verified. Avoid promising higher loudness without amplifier/speaker limits. |
| P2 | Coherent premium interaction | Native captures already have good typography, hierarchy and large primary actions, but lower controls can be partly visible and several workflows still require long scrolls. A route fade is not evidence of responsiveness. | Preserve distinct app layouts and compact titles. Put one primary action in the first viewport; move advanced options to a focused secondary sheet. Use consistent selected/pressed/disabled states, a subtle scroll-position cue, and scroll restoration. Test normal/Large Text, long content, offline, empty, error and rapid Back. Preserve all working functions. |
| P2 | Phone/watch continuity and honest status | Link freshness/reconnect machinery exists; source documentation has version/status drift. Transfer completion reports delivery, not necessarily audible playback completion. | Present separate phone, watch and Gateway status when needed, without permanent technical clutter. Add explicit playback receipt/status if protocol supports it. Test app kill, phone reboot, Bluetooth toggle, range loss, denied permissions and interrupted voice/update; never clear pairing as routine recovery. |
| P2 | Memory/storage hygiene and maintainability | Active OpenClaw speech is Gateway-generated, handled in bounded byte arrays and wiped after queueing. VoiceReplySynthesizer has temporary WAV code but no construction/call site was found. Discord sharing intentionally creates temporary attachments. shell.cpp is 5,246 lines; BLE service 1,262. | Remove or clearly isolate the unused local TTS path; add cold-start expiry and hard count/size bounds for any supported temporary attachments (handler cleanup does not survive process death). Keep OpenClaw voice RAM-only. Extract UI primitives/route controllers and connection/voice state machines incrementally, backed by existing lifecycle tests; no rewrite for its own sake. |

## Premium visual direction

Keep true black, ivory text, restrained accent, and the existing mechanical Home identity. Strengthen precision: aligned baselines, consistent spacing, stable-width timing values, readable metadata, quiet dividers, and obvious state feedback. Prefer task-specific screens to identical cards. No perpetual animation, blur/shadows, gratuitous borders or filler copy. No simulated haptics: this unit has no verified actuator. A good pressed state and immediate acknowledgement are more useful than decorative motion.

## Recommended implementation sequence

1. Capture a baseline and reproduce remaining touch/scroll faults on 0.2.23.
2. Apply selective label/style updates, preserve scrolling, and benchmark the route fade. These are the best first performance/polish changes.
3. Restore verified standby RTC scheduling, then use measured sleep-lock evidence to choose the next battery change.
4. Improve voice progress/cancel/summary states without changing the working buffered audio pipeline; tighten temporary-attachment lifecycle cleanup.
5. Refine secondary sheets, status states and phone/watch continuity; run a complete native and physical acceptance pass.
6. Only then investigate larger display buffers, interrupt-driven motion or a different audio transport/encoding. Keep these separate experiments with known-good rollback images.

## Existing solutions preflight

Use the pinned, already-integrated LVGL 9.5 partial rendering, invalidation, scroll and event mechanisms and ESP-IDF 5.5.5 PM profiling before introducing new frameworks. Android already provides the existing GATT callbacks and lifecycle machinery. No new paid service, UI framework, proprietary SDK, or custom rendering engine is justified by this review.

## Acceptance scorecard (targets, not results)

- Scrolling: per-route frame median/p95, long-frame count and visible input latency; target a consistent 30 fps before pursuing 60 fps.
- Wake: repeated unplugged raises/touches/buttons, no false-trigger storm or multi-second recovery interruption.
- Power: repeatable mostly-screen-off discharge comparison with identical brightness, BLE and raise settings; successful sleep entry and wake reasons recorded.
- Voice: stable long playback, prompt cancel, correct owner/session binding, explicit excerpt/continuation, no accumulating OpenClaw audio files.
- Memory: internal DMA low-water mark and largest free block survive repeated navigation, voice and reconnect stress; no route-cache growth.
- UX: every route checked with normal/Large Text and offline/empty/error states on the real panel; controls remain reachable and scroll position is not unexpectedly reset.
- Release: existing host, build, signed-package, inactive-slot and reboot health gates remain mandatory; they do not replace physical quality checks.

## Authorized first implementation — candidate 0.2.24

Owner requested implementation. First pass suppresses unchanged label text across
the shell, removes full-content route fades, retains notification-list scroll
position during live refresh, and restores 30-second blank RTC reads (1 second
active). Motion polling, raise-only policy, audio buffering, pairing and NVS
remain unchanged. Production-boundary tests cover redundant label calls and
scroller selection; existing hardware tests check active/standby cadence.
Physical improvement and battery gain still require measurement. Remaining
review items are subsequent work, not claimed implemented by this candidate.

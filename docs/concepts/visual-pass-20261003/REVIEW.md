# Nightglass: full watch visual pass — 2026-10-03

## Deliverable and boundary

A source-based audit of all current shell renderer families, plus an interactive 26-family design reference. This is a proposed visual direction, not installed firmware, native render captures, or a completed physical usability review. The user confirms OpenClaw voice works; retain its behavior while improving presentation. Phone companion redesign is outside this pass except where faithful previews must eventually match.

The complete renderer inventory is in inventory.json. Wrappers (render_route/render_home/render_pack_home) share the underlying family treatment. Classic, Personal and Revenant Home retain their distinct layouts; mechanical-skull artwork remains unchanged. The browser previews use the existing Quiet Shell image, not replacement artwork.

## Diagnosis grounded in current source

- Launcher: 17 flat rows; OpenClaw appears at index 12. Every destination has the same visual weight.
- OpenClaw: hold-to-speak starts at scroller y=408, after response paging and duration controls; previous/next are 48 px high and spoken-reply controls are 48 px high. Primary interaction competes with settings.
- Media: transport controls are followed by many secondary commands; artwork/queue/output is a further combined destination. Simplify first-screen hierarchy without removing actions or inventing provider support.
- Phone: call controls, agenda, ring/stop, camera and notifications share a long mixed-purpose page. Current ringing/active-call state should own the first screen.
- Alarm: four 54 px increment buttons span a busy horizontal row; enable is below the editing controls. Time editing deserves its own focused surface.
- Activity: configuration and reset are mixed into the daily summary; reset is prominent rather than secondary.
- Shared make_button uses the same 18 px font, rounded rect and bordered treatment for many action types. Contrast, density and action hierarchy should be semantic, not uniform.
- Existing project rules specify a 410×502 panel, 28 px safe inset and 56×56 px minimum controls; retain these and verify all rows rather than shrinking controls to fit.

## Visual system

True black base; neutral gunmetal surfaces; bright ivory primary text; legible muted metadata; acid-green emphasis only for the next meaningful action. Amber/red retain warning/error meaning. Preserve Revenant's mechanical identity on Home; quiet secondary chrome does not need a border around every control. No new continuous animation, blur, shadow effects or web-only gradients in firmware.

Use existing native Montserrat assets initially: 14 metadata, 18/20 body, 26 route titles, 36–48 hero values where they fit. Validate the native glyphs and measured extents before treating browser font rendering as authoritative. Sentence case for ordinary labels; uppercase only sparingly for metadata.

Shared primitives: safe header/back, metadata/status line, hero summary, list row, one dominant action, secondary sheet, confirmation, loading/empty/offline/error message. Minimum targets remain 56 px; primary targets 64–82 px. Cap each row to actual content width, not a sum of independently hardcoded widths.

## Whole-interface plan

| Family | First-screen emphasis | Secondary surfaces |
|---|---|---|
| Home / Classic / Personal / Revenant | Retain actual artwork and distinct face geometry; improve complication hierarchy where needed | Appearance editor, glance navigation |
| AOD | Sparse existing clock/date, low-luminance behavior and pixel shifting | No fake status or perpetual redraw |
| Launcher | Everyday favorites and clear labels | All apps; Diagnostics/About under Settings |
| Context Deck | One readable live card at a time; card opens its app | Existing reorder/hide settings |
| OpenClaw | Speak control and recording/response state | Voice duration, spoken replies, paging, confirmed suggestions |
| Media / Spotify | Track identity and three large transport actions | Seek, volume, queue, artwork, output handoff |
| Notifications / Discord | Source, sender, readable preview | Detail, provider-supported reply/actions, deliberate clear-all |
| Message / rich content | Readable body, obvious next action | Attachments/handoff, extra actions; preserve target binding |
| Phone / agenda | Live call state or phone status | Ring phone, camera, agenda and existing call capabilities |
| Weather | Temperature/conditions plus freshness | Details, refresh and network settings |
| Activity | Real daily steps/progress | Goal, units, calibration and deliberate reset |
| Alarm | Time, repeat, enabled state | Focused time editor; quiet hours under Sound |
| Timer | Stable large time, start/pause | Duration, secondary reset; completed alert |
| Stopwatch | Stable elapsed digits, start/pause | Laps, secondary reset |
| Audio | Recording/playback state and stop | Recordings/storage; unavailable capability state |
| Settings | Human groups and current selected values | Preserve every existing setting |
| Watch faces / premium profile | Faithful face selection and confirmed state | Draft/apply distinction, display flags |
| Gestures | Current assignments | One-step calibration; unchanged quiet-window and cancel gates |
| Power / clock / connectivity | Clear selected values and actual state | Advanced settings and recovery; no invented capabilities |
| Diagnostics / About | Quiet capability/version summary | Technical details/export |
| Global overlays | One clear decision, generous touch targets | Preserve confirmations, alerts, Back and system gestures |

## Implementation order and acceptance

1. Shared native tokens/primitives and geometry audit; keep routes and callbacks intact.
2. OpenClaw, Media/Spotify, Launcher and global Quick Settings/notifications.
3. Timer/Alarm/Stopwatch, Phone/agenda, Weather/Activity and Audio.
4. Settings families, customization, gestures, diagnostics and About; Home/AOD checks.
5. Native render coverage, normal/Large Text, long labels, no-data/offline/loading/error, and actual watch touch/scroll/swipe review. Install only signed firmware through the established release/update path; never erase or raw-flash to achieve a visual change.

No firmware changes or device installation performed in this design-review pass. Mockup buttons are local presentation controls, not phone/watch commands. Existing functionality must not be removed to simplify a screen. A native target build and host checks cannot alone establish premium appearance or usable physical gestures.

## Browser reference verification

Local Playwright/Chromium checked 26 families × 4 states × 2 viewport widths = 208 screen/state/viewport cases, with Large Text enabled. Verified safe lower-edge geometry, no horizontal document overflow, functional route/state navigation and preview-only action feedback; no JavaScript errors. Inspected the exported desktop preview image. The delivered inline widget uses the same controls and layout without the embedded texture to stay compact; its actual host frame was not independently inspected. Native fonts, watch clipping, physical touch and firmware behavior remain unverified by these browser checks.

## Owner correction: less chrome and copy

Remove redundant app titles, eyebrow subtitles, hero slogans and descriptive filler from watch screens. Use the content and controls to establish context. Retain only actionable state, necessary values/units, readable messages and control labels. Explanatory design notes and sample-data disclaimers belong outside the watch preview, not on the product screen. This correction overrides the earlier title/subtitle-heavy reference direction.

## Balanced revision: distinct layouts, not empty templates

Owner correction supersedes the title-free variant. Keep compact navigational titles, task-specific values and useful state; remove slogans, repeated introductions and explanatory product copy. This revision introduces distinct conversation/microphone, media transport, circular timing/activity, weather summary, notification list/detail, grouped settings and capability-summary compositions. Sample data remains labeled outside the watch. Local/offline-capable tools remain usable when the phone is offline. All changes here are design-reference artifacts, not firmware modifications.

### Balanced-revision verification

416 browser cases checked: 26 screen families × 4 states × 2 viewport widths × normal/Large Text. Verified 56 px minimum native-reference control dimensions, safe footer placement and absence of horizontal document overflow. Navigation from Apps to OpenClaw and local voice/timer preview state transitions passed; zero JavaScript errors. Visually inspected the four-screen contact sheet (OpenClaw, Spotify, Timer, Settings), now saved as preview.png. Actual hosted inline frame remains independently unverified; firmware/display/touch verification has not been performed.

## Native implementation follow-through

Owner approved the balanced revision and requested installation. Candidate 0.2.19 ports the direction into real LVGL firmware; see ../../NATIVE_VISUAL_PASS_20261003.md and native-normal.png/native-large.png. The earlier sections record the design-review stage and do not describe the subsequent firmware implementation. The native Sound screen intentionally retains its supported audio/DND controls rather than the preview's conceptual recorder. Native installation acceptance remains separately recorded.

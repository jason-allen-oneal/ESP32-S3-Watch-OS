# Premium build: items 4, 5 and 6

Source implementation, 2026-09-05. The initial implementation/build-only results
below are historical. The subsequent explicitly requested watch installation
succeeded and passed its boot-health gate; see
[the installation evidence](PREMIUM_FLASH_20260905.md) for the exact installed
artifact, pre-install correction, and remaining physical checks. The companion
was subsequently installed and its watch-profile readback verified; see
[the companion installation record](PREMIUM_COMPANION_20260905.md). This is not
full production/HIL acceptance.

## 4. Customization and phone control center

The Android launcher opens a daily-use control center with sample face previews,
connection status, editing, display controls and backup tools. Existing setup and
recovery remain under Connections & advanced setup, including old shortcuts.

- Classic and Revenant are retained. Personal has three independently assigned,
  tappable complications: battery, steps, weather, timer, next alarm, notifications,
  distance or phone connection.
- Six accent presets; additional readable RGB colors can be imported. Semantic
  warning/error colors are preserved. All five Context cards can be reordered or
  hidden, with at least one enabled. Large Text increases card height.
- External versioned JSON profiles are imported/exported with the phone document
  picker. Limits: 8 KB, 24-character ASCII name, known layouts/enums, bounded JSON
  nesting, readable accent. No scripts, custom fonts, paths or arbitrary assets.
  See [the importable Glacier pack](../companion/face-packs/glacier-personal.json).
- Import first replaces a **phone draft**, after confirmation. Applying is a
  separate action. “Saved on watch” requires a matching save acknowledgment.
  Offline or older-firmware drafts are never presented as applied.
- The profile is committed as one NVS blob before publication. Existing face
  selection is migrated without a boot-time rewrite. The new BLE characteristic
  is appended to preserve v1 handles, with standard Service Changed signaling.

Backup scope is appearance, complications, Context Deck and the three display
flags. This is **not a full device backup**: alarms, activity history, other power
settings, Wi-Fi, bonds, update keys and OpenClaw credentials are not exported.
Arbitrary image/filesystem face manifests and launcher/quick-control editors
remain outside this constrained pack format.

## 5. Display, accessibility and timing

- AOD is off by default. Enabling it makes the normal blank deadline enter a
  separate black ambient layer at 2% requested brightness, retaining the current
  route underneath. The existing wake-only first-touch handling is preserved.
- Clock/date update once per minute, honor 12/24-hour time and move through a
  9-by-9 pattern, up to 12 px each way. Normal route refresh pauses. This reduces
  static-pixel exposure; it is not measured luminance or a panel-lifetime claim.
- AOD yields to blank/sleep at 15% battery, missing/invalid telemetry or samples
  older than 30 seconds. The supervisor rechecks every five seconds in ambient.
  AOD consumes more battery than a blank screen.
- Large Text uses the spacious Personal home layout, larger menu/reader fonts,
  taller Context cards and shorter OpenClaw pages.
- Reduce Motion disables the cancellable 120 ms opacity transition, elastic
  scrolling and momentum. Existing touch/sleep and PSRAM stack guards remain.
- Logs now report bounded 64-sample software input-to-render and render-cycle
  distributions. These are **instrumentation**, not a passed physical touch
  latency, panel scanout FPS or 30 fps/80 ms/150 ms release gate.

## 6. Deeper apps and confirmed OpenClaw actions

Artwork / Queue / Output in Media or Spotify requests metadata, a 40-by-40 RGB565
bitmap when available, up to four exposed queue entries, supported session volume
controls and a handoff to the player to choose an output device. Queue IDs,
session and track identity are rechecked before action. Availability depends on
the Android media session. This is not Spotify Web API/Connect device discovery.

Conversation + Actions is a scrollable reader for up to eight MessagingStyle
messages from the live notification, with sender names and attachment type
labels. Actual non-authenticated, non-RemoteInput provider actions are offered;
reactions appear **only if the notification exposes a reaction action**. Opening
the conversation/attachments uses its original PendingIntent. Existing quick
reply and Discord voice-share flows remain. Full Discord history, generic
server-side reactions and attachment viewing stay in Discord. No Discord user
token, self-bot integration or remote attachment fetch is introduced.

Rich content is capped at 4 KB in explicit PSRAM buffers and transferred atomically
with token/target/kind binding, contiguous offsets and CRC32 over the existing
pinned encrypted BLE link. A dedicated bounded phone queue prevents notification
churn from dropping middle fragments. Requests/actions expire after 60 seconds;
navigation away, privacy changes, notification removal and disconnect scrub the
reader as applicable. Forward action sequences and live provider ownership are
checked. No message/artwork history is stored on disk. CRC is transfer integrity,
not a substitute for link authentication.

OpenClaw can suggest only a 60–3600 second timer, navigation to Spotify/Phone/Inbox,
or ring-phone. Suggestions are inert until a review tap and separate confirmation
within eight seconds on the watch, bound to the completed session/response and
single use. Existing timers cannot be overwritten. Markers are omitted from
spoken replies. Gateway read-only permissions/tool restrictions are unchanged;
no model-generated URL, code or arbitrary tool call is executed.

## Validation and handoff

Final local validation on 2026-09-05:

| Check | Result |
| --- | --- |
| ESP-IDF firmware build | Pass; 2,710,848 bytes, 57% of the smallest app slot free |
| Complete host suite | Pass |
| Premium C++ ASan + UBSan run | Pass |
| Android unit tests | 81 passed, including 15 new premium tests |
| Android lint | 0 errors, 40 warnings; warnings are not suppressed by a new baseline |
| Debug APK assembly | Pass; 8,625,688 bytes |
| Physical installation / acceptance | Not performed |

Final artifact SHA-256 (not signature or release acceptance):

```text
nightglass.bin  1473522da2dabc16c6067d5e5727c966888d41ce7fc7c13aba58819e98005c23
app-debug.apk  8786825e6201b277ee332f8f2bc6e2594bd945da5eebad21bc92e4376640bb10
```

```sh
./scripts/test-host.sh
NIGHTGLASS_PROJECT_VERSION=0.2.4 NIGHTGLASS_SECURE_VERSION=4 ./scripts/build.sh
cd companion/android
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
ANDROID_HOME=~/.cache/nightglass/android-sdk \
~/.cache/nightglass/gradle-8.9/bin/gradle --no-daemon \
  :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

New C++/Kotlin tests share exact profile/document golden vectors and cover profile
round-trips, malformed/deep/oversized imports, face/display combinations, bounded
content, truncated/reordered/duplicate transfers, CRC failures, stale targets,
action replay/expiry, AOD cutoff/shift and restricted suggestion grammar. Existing
host, voice, crypto, touch, navigation, release and companion tests remain.

Artifacts: `build/nightglass.bin` and
`companion/android/app/build/outputs/apk/debug/app-debug.apk`. The firmware binary
is a build artifact, **not a signed release package**. Physical acceptance remains:

1. Existing boot-health/rollback qualification; bonded-phone upgrade/discovery;
   legacy firmware compatibility; save/read-back and restart persistence.
2. Repeated dim/AOD/blank wake on every route, wake-only first touch, alarms/calls
   interrupting ambient and low-battery fallback without a stranded display.
3. Real readability/clipping, Large Text/Reduce Motion navigation, physical
   latency/FPS and battery/luminance measurements. No emulator is installed here.
4. Actual Spotify/Discord provider capabilities, phone lock/permissions, changing
   sessions/notifications and disconnect mid-transfer.
5. OpenClaw suggestion confirmation/cancellation and active-timer protection.

Other premium groups (power hardware qualification, production update/recovery,
audio/storage acceptance and remaining protocol hardening) are not completed by
these changes.

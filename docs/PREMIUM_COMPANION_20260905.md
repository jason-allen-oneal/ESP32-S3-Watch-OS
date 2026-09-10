# Premium companion installation, 2026-09-05

The updated companion is installed on the Galaxy S23 and has reconnected to the
already-pinned watch. The premium characteristic was discovered, the read-only
profile query was acknowledged, and the returned profile was validated and
stored as `confirmed` on the phone. No appearance changes were applied.

## Installed artifact and preservation

- Package: `dev.nightglass.companion`, version name `0.1.0`, version code `1`
  (unchanged; identify this build by the APK hash).
- APK: `companion/android/app/build/outputs/apk/debug/app-debug.apk`.
- Size: 8,626,290 bytes.
- SHA-256: `d22c9a6f91aebb056d1e2ede00ffe1a491f6d96522a429ea874e7660104296f7`.
- Final package update time reported by Android: 2026-09-05 13:00:10 EDT.
- Installation used `adb install -r`; no uninstall, data clear, permission reset,
  Bluetooth unpair, phone reboot or Gateway change was performed.
- Android retained the original application data directories/first-install time
  and Bluetooth/notification grants. Encrypted OpenClaw preferences and weather
  preferences had identical SHA-256 values before and after installation.
- The existing pinned watch authorized the new companion connection. No new
  pairing prompt, QR scan or credential provisioning was required.

## Settings-sync correction found on hardware

The first installed companion connected securely and reached OpenClaw, but did
not receive a premium profile. Subsequent instrumentation confirmed that the
premium characteristic was present: a stale service cache was not the observed
cause. The profile query was waiting in the general 32-frame queue, where
reconnect/notification traffic can discard its oldest frames.

Profile queries and saves now use their own eight-frame queue, ahead of rich
content and ordinary synchronization; OTA and voice retain their priority.
Disconnect wipes pending profile frames rather than replaying settings writes.
The final installed build sent opcode 113 (`0x71`) and received GATT status 0,
followed by a validated watch profile. The previously absent `confirmed` entry
then appeared without applying the phone draft.

The companion also performs one read-only query per authorized connection. A
strict nonce-zero profile response can establish support if a future Android
service cache omits the optional characteristic. This fallback is tested but
was not required on the final observed connection. Notifications now explicitly
require the current GATT client, outbound characteristic and link generation.

## Validation and live observations

- 83 Android unit tests passed, including two new discovery-response tests.
- Lint: 0 errors, 40 warnings, without a new suppression baseline.
- APK assembly and signature verification passed.
- Host suite passed; the extended companion lifecycle/queue contract also passed.
- `git diff --check` passed.
- Live log: premium characteristic present, authorized link ready, two-byte
  profile query acknowledged, and the profile returned to the phone.
- OpenClaw health reported `configured=true internet=true reachable=true
  fatal=false state=HEALTHY` at 13:00:29 and again at 13:01:30 EDT.
- Initial voice-health writes caused reconnect retries during startup; the
  connection subsequently remained authorized through the later health check.
  This is not a claim of zero reconnects or a completed voice round-trip.

The control-center activity launched, but the phone locked during verification;
this is not a completed visual/layout review. Profile write/acknowledgment,
physical watch gestures, media/notification actions, AOD measurements and voice
round-trips remain separate physical acceptance checks. See
[watch installation evidence](PREMIUM_FLASH_20260905.md) and
[feature/provider limits](PREMIUM_BUILD_456.md).

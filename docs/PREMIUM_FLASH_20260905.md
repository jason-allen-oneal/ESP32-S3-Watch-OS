# Premium watch installation, 2026-09-05

The premium 4/5/6 firmware was signed, installed over the watch cable, and
accepted by the unchanged on-device 60-second health gate. This is a personal
working-tree build, not completion of the clean-commit production release/HIL
campaign. Existing unrelated work was preserved; no Git commit was made.

The phone was connected later the same day. Its companion update, preserved
credentials/pairing and successful premium-settings readback are documented in
[PREMIUM_COMPANION_20260905.md](PREMIUM_COMPANION_20260905.md). References below
to the unavailable phone describe the earlier watch-flashing phase.

## Exact installed artifact

- Package: `build/update-package-0.2.4-premium-456`.
- Firmware: Nightglass 0.2.4, secure version 4; 2,710,848 bytes.
- Firmware SHA-256:
  `4bddf722c5e4b6f9a01fb0c10c97360041b4acb67f2020eef64c396aca528bf5`.
- ELF SHA-256:
  `c23c109d5d263ccadf37463d47fcd6edbd83622d72a067531c123f4aa2d613e6`.
- Manifest payload SHA-256:
  `f44e36b6a0f9f33f3859cb61fc01d61719253925c85114a898429d82f4555f6a`.
- Source base: `ebaaace32fbe4dd975ebf33615dd700c9c7ff887` plus existing WIP
  and the pre-install timing correction below.

## Verification before installation

The complete host suite, firmware build, resolved/generated release settings,
embedded app identity, partition geometry, runtime asset validator, and
independent signed-package verifier passed. Repeated incremental builds produced
identical firmware/artifact hashes. Premium protocol and FT3168 host tests also
passed AddressSanitizer and UndefinedBehaviorSanitizer.

Android's fresh `:app:testDebugUnitTest --rerun-tasks` run passed all 81 tests
with zero failures/errors. Lint reported zero errors and 40 warnings; debug APK
assembly passed. No phone was available through ADB, so the companion APK was
not installed in this turn. The installed watch alone does not prove the new
phone control center or rich-content features end to end.

Pre-install review corrected the FT3168 reset-release settling delay from
50 ms to 100 ms before configuration. The manufacturer's
[FT3168 v0.6 datasheet, table 3-5](https://files.waveshare.com/wiki/common/DATA_SHEET_FT3168.pdf)
specifies at least 70 ms to reporting readiness. Tests now require that minimum
before any configuration read/write. This is not proof that the timing mismatch
caused every earlier idle-read failure; some idle read errors were still observed
after installation. No health threshold, signature requirement, or rollback
protection was relaxed.

## Device evidence

Target:
`/dev/serial/by-id/<device-id>`.
Fresh pre-install normal boot identified ESP32-S3 v0.2, 32 MiB flash, 8 MiB
PSRAM, and the existing valid 0.2.2 / secure 2 application:

```text
OTA_BOOT running=ota_1@0x00620000 configured=ota_1@0x00620000 state=2 pending=0 rollback_possible=0
```

The guarded one-time ROM bootstrap verified the signed package, hardware and
layout, wrote only inactive `ota_0`, read back and compared the complete image,
then committed its inactive OTA record as NEW. NVS was byte-compared unchanged
through the transaction and the valid `ota_1` image/selection record was retained.
The existing owner-only 32 MiB recovery backup also passed its stored checksum.
No bootloader, partition table, assets, NVS, or eFuse provisioning was performed.
Normal application startup still performs its usual NVS bookkeeping.

The new boot and acceptance were recorded in `build/premium-flash-20260905.log`:

```text
App version:      0.2.4
Secure version:   4
ELF file SHA256:  c23c109d5...
OTA_BOOT running=ota_0@0x00020000 configured=ota_0@0x00020000 state=1 pending=1 rollback_possible=1
TOUCH_CONFIG chip=FT3168 id=0x03 mode=0x00 factory_tuning=1
TOUCH_LIVENESS controller=1 input=1 irq_registered=1 reads=1
USB_UPDATE_READY protocol=1 signed_only=1 max_chunk=230 raw_flash=0
USB_UPDATE_BOOT version=0.2.4 secure=4 pending=1 state=PENDING_VERIFY
OTA_HEALTH_ACCEPTED state=VALID pending=0
Direct bootstrap accepted: ota_0 is VALID, rollback was preserved, and native signed USB updates are ready
```

Acceptance occurred at firmware uptime 61.438 seconds. The phone reconnected
and reported OpenClaw availability during startup. This does not establish a
voice round-trip or continuous phone-link reliability.

The additional 120-second observation in `build/premium-postflash-20260905.log`
completed without a reset or crash. All six native USB status queries succeeded.
The display blanked at 122.048 seconds of uptime and the USB-attached sleep
guard inhibited MCU light sleep at 152.028 seconds; USB remained responsive.
Touch liveness was observed again at 71.958 and 141.968 seconds, with internal
free memory 40,410 bytes and largest block 8,192 bytes, but repeated idle I2C
errors also remained. No physical `TOUCH_CONTACT` was captured in these logs.
The phone link disconnected at 100.658 seconds and was not seen reconnecting
within this observation window. Do not call this a passed physical wake/swipe
test or an end-to-end companion acceptance.

## Remaining physical acceptance

- Physical touch/wake/swipe confirmation on this exact installed build.
- Companion installation, bonded upgrade/discovery and read-only profile
  readback passed in the subsequent phone session linked above. Profile writes,
  restart persistence, real media/notification actions and OpenClaw confirmation
  still require physical acceptance.
- AOD/battery/display measurements and the remaining production HIL campaign.
- Intermittent idle touch I2C errors remain observable; a successful health gate
  must not be reported as exhaustive touch reliability proof.
- The pre-existing unreadable LittleFS volume remains untouched. Built-in assets
  are in use, as on the earlier working firmware.

See [PREMIUM_BUILD_456.md](PREMIUM_BUILD_456.md) for feature/provider limitations.

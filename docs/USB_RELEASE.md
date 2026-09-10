# USB release safety

## Supported direct installer

Nightglass supports signed, application-only updates over the watch's native
USB Serial/JTAG cable:

```bash
python3 scripts/nightglass-usb-update.py /absolute/path/to/signed-package \
  --device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...
```

The package directory must contain exactly `firmware.bin`, `manifest.json`,
`manifest.payload`, and `manifest.sig`. Supplying that command is approval to
install; there is no redundant prompt. The host claims the stable serial path,
refuses a competing serial monitor, issues no modem-control/reset commands,
verifies the package before connecting, and sends the same signed update
protocol used by the companion. It never invokes the ROM loader or writes a
flash address from the host.

On the watch, a fixed-buffer USB receiver unwraps `NGU1` length/CRC envelopes
and feeds `UpdateService`. The service verifies the signature and monotonic
version before erasing, writes only the inactive slot, enforces sequential
offsets, validates the ESP image and embedded descriptor, reads back its full
SHA-256, and calls `esp_ota_set_boot_partition`. The next boot remains rollback
pending until the 60-second health gate accepts it. USB and BLE sessions are
bound to their originating link; neither transport can hijack or abort the
other's active session.

This operation does not address NVS, the bootloader, partition table, OTA
metadata directly, coredumps, or assets. Watch bonds, the pinned phone identity,
settings, and calibration remain in NVS. Phone/OpenClaw tokens never leave or
change on the phone.

Generic ESP-IDF write targets (`flash`, `erase_flash`, component/partition
flash targets, DFU flash, and the LittleFS flash target) remain visible because
ESP-IDF generates them, but Nightglass attaches a failing dependency to them.
They do not pass through signature verification or the on-device rollback
state machine.

ESP-IDF 5.5.5's host `otatool.py` is also unsuitable for this contract:

- `write_ota_partition` is a raw partition write. It does not call
  `esp_ota_begin`, `esp_ota_end`, or ESP-IDF's application-image validator.
- `switch_ota_partition` changes only `ota_seq` and its CRC. It leaves the old
  `ota_state` bytes in place instead of selecting the target as
  `ESP_OTA_IMG_NEW`.
- A host read of otadata can determine the bootloader's configured candidate,
  but cannot prove the actually running slot after image-validation fallback.
- Entering the ROM loader resets the watch. If the running image is
  `ESP_OTA_IMG_PENDING_VERIFY`, that reset can cause the bootloader to mark it
  aborted before the sixty-second health gate accepts it.

Normal direct updates stream through the running watch. The one-time migration
below is the bounded exception for older firmware without a USB receiver.
Writing inactive bytes and then using `otatool.py switch_ota_partition` is not
equivalent: that tool does not set the required new-image rollback state.

## One-time USB migration from 0.2.2

An existing Nightglass 0.2.2 watch can receive the signed 0.2.4 package directly
over its watch cable, without using the phone. Activate the pinned ESP-IDF
Python environment, capture a fresh `OTA_BOOT` line, then run:

```bash
python3 scripts/nightglass-rom-bootstrap.py /absolute/path/to/signed-0.2.4-package \
  --device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...-if00 \
  --running-evidence 'OTA_BOOT running=ota_1@0x00620000 configured=ota_1@0x00620000 state=2 pending=0 rollback_possible=1'
```

The evidence must describe the current boot; do not substitute the example if
the actual line differs. This migration requires the exact 32 MiB ESP32-S3
layout, valid 0.2.2/secure-version 2 in `ota_1`, and a signed
0.2.4/secure-version 4 image. It refuses pending verification, an unexpected
partition table, Secure Boot, or flash encryption.

The migration attaches and checks the flash, writes and reads back only the
inactive `ota_0` image, then commits one inactive OTA metadata sector with the
`NEW` state. It compares NVS before and after the transaction and preserves the
existing valid `ota_1` selection record for rollback. It resets once, probes the
new USB receiver, and waits for `OTA_HEALTH_ACCEPTED` after the 60-second gate.
Do not reset or unplug the watch during that gate. Subsequent updates can use
either native USB or the companion OTA package picker.

After an unsuccessful candidate rolls back, the old valid image may report
`rollback_possible=0` because the other slot is aborted. The migration supports
that retry: it verifies the valid `ota_1` metadata and retains it as the new
candidate's rollback target.

The USB RX task uses PSRAM for its stack and queues all flash operations to the
internal-RAM update worker. Its 1 KiB RX and 2 KiB TX rings stay internal for
interrupt access; RX holds the host's complete three-frame window. The initial
candidate exhausted internal RAM during radio startup, so its larger buffers
and internal RX stack must not be restored. A pending release now also requires
healthy USB receiver startup before the acceptance gate can succeed.

The USB-only change was insufficient on a later boot. The UI's measured
16 KiB stack now also uses PSRAM, while DMA buffers and the update worker
remain internal. Ordinary allocations above 1 KiB prefer PSRAM. The release
gate requires PSRAM instruction and read-only-data XIP plus external-task
support, so UI-side NVS writes do not disable the cache backing that stack.
`BOOT_MEMORY` and periodic `RUNTIME_MEMORY` report the remaining internal
capacity; `ALLOCATION_FAILED` reports a failed request's size and capabilities.

The board uses an FT3168, even though BSP 1.0.7 instantiates its generic FT5x06
coordinate reader. Nightglass resets the touch controller after that generic
initialization to restore factory tuning, verifies the FT3168 identity, and
writes/reads back active power mode before registering LVGL input. The
[FT3168 datasheet](https://files.waveshare.com/wiki/common/DATA_SHEET_FT3168.pdf)
describes idle-mode I2C communication restrictions after another slave uses
the shared bus; the
[board vendor's register reference](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/blob/main/examples/arduino/libraries/Arduino_DriveBus/src/touch_chip/Arduino_FT3x68.h)
and mode setter specify device ID `0xa0=3` and active mode `0xa5=0`.
`TOUCH_CONFIG`, periodic `TOUCH_LIVENESS`, and actual `TOUCH_CONTACT` records
distinguish successful initialization, sustained reads, and physical input.
Active-mode touch power consumption remains to be measured; do not restore
monitor mode without shared-bus idle/wake validation.

The 2026-09-05 pre-install review increased the delay after reset release from
50 ms to 100 ms before any FT3168 configuration reads/writes. Datasheet v0.6,
table 3-5 specifies a 70 ms minimum to reporting readiness; the old 50 ms came
from the vendor example. Host tests reject configuration before that minimum.
This corrects the timing mismatch but does not itself prove the cause of the
earlier idle-read failures. The touch health freshness and rollback gates are
unchanged, and hardware acceptance is still required.

## Read-only diagnostics

### Latest personal-watch installation (2026-09-05)

Signed **0.2.14 / secure14** is installed on **ota_0**, VALID, pending=0, and
passed the unchanged health gate. It replaces the rough OpenClaw mark with a
54×54 antialiased adaptation of the official mascot while preserving the
existing voice-health colors, tap target, circle alignment, backgrounds, and
wake fixes. Native rendering/transparency/hit-resolution checks, the target
build, signed readback, and fresh accepted identity passed. See
[OpenClaw icon installation evidence](OPENCLAW_ICON_20260905.md).
On-wrist visual/touch acceptance for this icon update is not claimed.

### Previous upper-circle alignment installation (2026-09-05)

Signed **0.2.13 / secure13** was installed on **ota_1**, VALID, pending=0, and
passed the unchanged health gate. It centers the upper-circle step/weather
icons above values on matching rows. Native LVGL normal/Large Text previews,
one target build, signed readback, exact pending version, health acceptance,
and a fresh accepted identity passed. All artwork and wake fixes remain
unchanged. See [alignment installation evidence](DIAL_ALIGNMENT_20260905.md).
On-wrist visual/touch acceptance for this alignment update is not claimed.

### Previous Quiet Shell installation (2026-09-05)

Signed **0.2.12 / secure12** was installed on **ota_0**, VALID, pending=0, and
passed the unchanged health gate. It adds dedicated Quiet Shell secondary-screen
artwork while retaining the Home/AOD and 0.2.11 wake fixes. Native USB readback,
exact pending version, health acceptance, and fresh accepted identity were
confirmed. No NVS erase or separate asset-partition write occurred. See
[Quiet Shell installation evidence](QUIET_SHELL_20260905.md).
On-wrist appearance/touch acceptance for this artwork update is not claimed.

### Previous wake-repair installation (2026-09-05)

Signed 0.2.11 / secure11 is installed on ota_1 and passed the unchanged health
gate. Physical blank-to-center-tap wake was confirmed without reboot. See
[the accepted wake repair and evidence](WAKE_STABILITY_20260905.md). This
supersedes the earlier 0.2.4 installation status below.

### Previous premium installation (2026-09-05)

The signed premium 4/5/6 build was installed into `ota_0` and passed the unchanged
60-second boot-health gate. Its firmware SHA-256 is
`4bddf722c5e4b6f9a01fb0c10c97360041b4acb67f2020eef64c396aca528bf5`.
NVS and the valid 0.2.2 rollback slot were preserved. See
[the exact evidence and remaining limitations](PREMIUM_FLASH_20260905.md).
This supersedes the installation status below, not the remaining physical/HIL
qualification or the observation of intermittent idle touch-read errors.

### Historical failed bench candidate (2026-09-04)

The latest signed candidate is `build/update-package-0.2.4-touch-recovery`,
SHA-256 `9f8ebb7c54d72a3ed9f987850e2e0e6239c4fa3866cd15024d70198a561fd64f`.
It is **not accepted or ready for release**. On hardware it verified FT3168
identity/active-mode readback, started the UI, radio, and native USB receiver,
and reconnected to the phone/OpenClaw. Internal free memory was 30,294 bytes
at USB startup and 17,134 bytes with the UI started but the main task still
allocated. No allocation failure occurred on that boot.

Idle touch reads remained intermittent: errors at about 21.8, 41.8, and
51.8 seconds, with a successful read in between. At 61.4 seconds the last
successful read exceeded the 25-second limit, so the unchanged health gate
requested rollback. A subsequent normal boot confirmed valid
0.2.2/secure-version 2 in `ota_1`. NVS was byte-compared and preserved through
the migration transaction. The `usb-touch-final`, `usb-touch-ramfix`, and
`usb-touch-ft3168` candidates also failed; none is a successful release.

Next evidence needed: a physical tap/swipe during a controlled candidate boot
to distinguish dormant-controller I2C behavior from failure of the actual
touch/IRQ/UI path. Do not treat an idle register read as proof of a working
touchscreen, or weaken the acceptance gate to force this candidate valid.

The pre-existing LittleFS asset volume is still unreadable. Built-in assets
are used, and the volume has not been erased as part of this repair.

### Capturing the running slot

Every boot emits one bounded, non-sensitive line on the USB log:

```text
OTA_BOOT running=ota_0@0x00020000 configured=ota_0@0x00020000 state=2 pending=0 rollback_possible=1
```

This is authoritative for the running partition and does not require a reset.
Capture it during an ordinary boot or from an already-open serial monitor. Do
not reset a watch whose line reports `pending=1`.

An existing owner-only 32 MiB baseline image can be inspected offline:

```bash
python3 scripts/inspect-ota-state.py /absolute/path/to/full-flash.bin \
  --partition-table build/partition_table/partition-table.bin \
  --candidate build/nightglass.bin
```

The inspector validates the full-read size and partition-table bytes, decodes
both redundant otadata sectors using ESP-IDF's CRC/state rules, reports the
boot candidate and inactive candidate, and validates the candidate's embedded
Nightglass app descriptor. It prints no NVS contents. Its
`actual_running_slot=UNKNOWN_WITH_OFFLINE_READBACK` result is intentional; use
the live `OTA_BOOT` line for that fact.

Acquiring a new full-flash readback still requires entering the ROM loader and
therefore a reset. Do it only after the live log has reported `pending=0` and
the sixty-second health gate has completed. Preserve the readback with mode
`0600` and a SHA-256 manifest outside this repository as required by the ESP32
baseline.

## Release artifacts

`scripts/verify-release.sh` now includes `build/assets.bin` in both deterministic
build hash passes. It also reruns the runtime WAV catalog validator and requires
the LittleFS image to be exactly `0x1360000` bytes, matching the reviewed
`assets` partition. The asset image remains separately provisioned and is not
part of an application OTA.

Production coredumps are disabled. Retaining task stacks and companion-derived
text in the flash coredump partition is an unnecessary privacy risk. The
partition remains reserved in revision 1 so disabling coredumps does not alter
flash geometry or strand deployed devices.

The release gate checks the resolved local `sdkconfig` and generated
`build/config/sdkconfig.h`, not only `sdkconfig.defaults`. It requires unsigned
development OTA and the audio boot HIL trigger off, flash/UART coredumps off,
the native USB automatic-light-sleep guard on, 98,304 bytes of internal reserve,
and an eight-row display transfer chunk. This prevents a stale ignored
`sdkconfig` from silently weakening production settings.

## Transport framing and recovery

USB commands use the existing update payloads (maximum 244 bytes) inside a
bounded envelope: magic `NGU1`, little-endian 16-bit payload length, payload,
then little-endian IEEE CRC32 over the payload. RX is owned exclusively by the
update task. USB logs may share TX; complete response envelopes are written in
one driver call and the host parser resynchronizes around ordinary log bytes.

The host sends at most three data frames before draining acknowledgements,
leaving headroom in the four-entry device queue. After a timeout it queries the
watch's exact received offset and never blindly replays ambiguous data. A cable
loss has a 30-second reconnect grace; 120 seconds without an active-session
command aborts and leaves the running slot untouched.

Firmware older than the first USB-capable release cannot receive this protocol.
Use its existing signed OTA updater or, for the supported 0.2.2 installation,
the one-time USB migration above. Raw `idf.py app-flash`, full flash, and
`otatool.py switch_ota_partition` are not substitutes.

## Hardware-in-loop release test (after the blockers are resolved)

1. Confirm the stable `/dev/serial/by-id/...` target, ESP32-S3 revision, 8 MiB
   PSRAM, and 32 MiB flash.
2. Capture `OTA_BOOT` and require `pending=0`; resolve the inactive slot from
   the reported running label, never from a cached offset.
3. Run `scripts/verify-release.sh`; record the application and `assets.bin`
   SHA-256 values.
4. Send a correctly signed image through the on-device transport. Interrupt one
   transfer and prove the active slot still boots.
5. Complete a transfer, read back the inactive slot, and compare exactly the
   declared image bytes to the release hash before selecting it.
6. Reboot once and require `state=1 pending=1` for the new running slot.
7. Force an essential-health failure and prove rollback to the prior slot.
8. Repeat with healthy display, touch, power, clock, NVS, and UI; wait at least
   sixty seconds and require `state=2 pending=0`.
9. Leave USB attached through the normal inactivity interval and verify native
   USB remains probeable.

Secure Boot, flash encryption, and eFuse anti-rollback remain separate,
irreversible production decisions requiring explicit approval.

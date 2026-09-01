# USB release safety

## Current status

Nightglass does **not** have a production USB deployment command. This is a
deliberate fail-closed boundary, not missing shell glue.

Generic ESP-IDF write targets (`flash`, `erase_flash`, component/partition
flash targets, DFU flash, and the LittleFS flash target) remain visible because
ESP-IDF generates them, but Nightglass attaches a failing preflight dependency
to them. The repository does not claim those upstream targets are removed.
`scripts/preflight-app-flash.sh` performs read-only target, recovery-image,
partition, resolved-config, and application-image checks; it never writes the
watch. Bypassing the CMake guard with raw esptool is outside the reviewed
release path.

ESP-IDF 5.5.5's host `otatool.py` is unsuitable for a rollback-protected
release:

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

Consequently, no repository script writes an OTA slot or otadata from the host.
Writing the inactive bytes and then using `otatool.py switch_ota_partition`
would look successful while silently bypassing rollback.

## Read-only diagnostics

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

## Required implementation before USB deployment can ship

The safe path is an on-device transport that feeds the existing
`UpdateService`:

1. provision a reviewed public verification key for the dormant P-256 verifier;
2. authenticate and frame the USB transport without sharing the logging stream;
3. pass the signed manifest to `begin_update` before erasing anything;
4. stream bounded sequential chunks through `write`;
5. call `finish`, which validates SHA-256, the ESP image, embedded descriptor,
   and then calls `esp_ota_set_boot_partition` on the inactive slot;
6. reboot and observe `NEW` become `PENDING_VERIFY`;
7. prove automatic rollback on failed health and acceptance only after the
   sixty-second healthy gate.

The repository currently has no production verification key or isolated USB
data channel, so steps 1-2 are hard blockers. Generating a key or changing USB
ownership without a recovery plan is outside this change.

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

# Update and recovery

Nightglass has an ESP-IDF inactive-slot backend, ECDSA P-256 verifier, native
USB installer, authenticated BLE companion transport, and recovery supervisor.
Selecting a signed package in either supported installer is sufficient approval
for the configured personal-watch build: after the image is validated, the
inactive slot is selected and the watch reboots automatically. There is no
second INSTALL/RESTART prompt on the watch. Signature verification and rollback
remain required for both transports.

The direct installer and the reason raw `esptool`/`otatool` deployment remains
blocked are documented in [USB_RELEASE.md](USB_RELEASE.md).

| Install path | Data link | Authorization | Slot behavior |
|---|---|---|---|
| Direct USB | Native USB Serial/JTAG cable to watch | Physical access plus valid signed manifest | `UpdateService` writes and verifies the inactive slot |
| Companion OTA | Bonded, encrypted, identity-pinned BLE | Authorized phone plus valid signed manifest | The same `UpdateService` path |

## Install contract

`UpdateService` accepts a fixed-size `UpdateManifest`, its detached signature,
and a sequential stream. Before `esp_ota_begin`, it checks:

- manifest format 1;
- board ID `nightglass-waveshare-esp32s3r8-2.06-v1.0`;
- partition ID `nightglass-32m-r1` and revision 1;
- nonzero declared size no larger than the discovered inactive OTA slot;
- a version different from the running image;
- a secure-version value strictly greater than the running app descriptor;
- a nonzero SHA-256; and
- a signature over the canonical manifest payload, including the image digest.

The service discovers the next OTA partition through ESP-IDF and additionally
requires an app/OTA subtype distinct from the running partition. Each write is
sequential, nonempty, at most 16 KiB, and unable to exceed the declared image
size. Completion requires the exact byte count, a constant-time SHA-256 match,
ESP-IDF image validation, and an embedded project/version/secure-version match.
Only then is the inactive partition selected for the next boot. The
authenticated transport applies the slot and performs a controlled reboot after
the final status notification; choosing the signed package was the approval.

A verifier implementation can be installed with
`set_signature_verifier()` and must have static lifetime. The interface returns
`verified`, `rejected`, or `unavailable`. A rejected signature is never
bypassed. The optional unsigned-development Kconfig switch bypasses only the
`unavailable` state, emits an explicit warning, and reports
`unsigned_development` rather than `verified`.

## Package artifacts

Build with an explicit release identity, then create and verify the signed
four-file package:

```bash
export NIGHTGLASS_PROJECT_VERSION=0.2.4
export NIGHTGLASS_SECURE_VERSION=4
python3 scripts/verify-partitions.py partitions.csv
./scripts/build.sh
./scripts/sign-update.sh build/nightglass.bin build/update-package-0.2.4
python3 scripts/verify-signed-package.py build/update-package-0.2.4
```

The signed package contains `firmware.bin`, `manifest.json`, the byte-exact
`manifest.payload`, and `manifest.sig`. An unsigned package declares a null
algorithm/file and `verified: false`; it is only an intermediate signing
artifact and neither supported installer accepts it.
The private signing key is external to the repository. The matching public key
is provisioned in release firmware and stored in `config/ota-public-key.hex`.

The deterministic release gate also hashes and validates `build/assets.bin`.
That LittleFS image is a separate artifact and is never implicitly written by
an application update.

## Boot health and rollback

Each boot increments `bad_boots` in the `ng_recovery` NVS namespace. The counter
is cleared only after the essential `nvs`, `display`, `touch`, `power`, `clock`,
and `ui` health records remain acceptable through a 60-second gate. Touch is
accepted only when the controller has answered a real read within the preceding
25 seconds and both the LVGL input and GPIO38 interrupt were registered; handle
allocation alone is not health evidence. Existing
display, power, and clock degraded modes are accepted because they retain the
watch shell and core time/power function; absent or failed health is not.
Optional radio, update-transport, and update-verifier health remains visible in
the service registry, but does not pin the rapid-reboot counter or safe mode.
This lets a watch recover when radios are disabled; direct USB installation
continues to work without BLE in both normal and safe mode.

When the bootloader reports `ESP_OTA_IMG_PENDING_VERIFY`, the gate calls
`esp_ota_mark_app_valid_cancel_rollback()` only after those checks pass and the
boot is not in safe mode. Failure or safe mode calls
`esp_ota_mark_app_invalid_rollback_and_reboot()`. If the gate task cannot start,
the image is never accepted, so ESP-IDF rollback remains armed.

Three consecutive unconfirmed boots enter safe mode. Holding the GPIO10 side
key continuously for the 1.2-second boot sampling window also requests safe
mode. A brief normal press incurs no delay beyond the first released sample.
An image in ESP-IDF's one-shot `ESP_OTA_IMG_PENDING_VERIFY` state is allowed one
normal 60-second health-gate attempt even if the old image already reached the
safe-mode threshold. The handoff depends only on bootloader state, so it also
works for upgrades initiated by older firmware. Failed health still rolls the
image back, and holding GPIO10 still forces safe mode and rollback.
Safe mode still starts the display, power, clock, fallback watch-face service,
and shell, but leaves optional sensor hardware, audio, activity, BLE, Wi-Fi,
weather, and the audio boot self-test stopped. A dedicated recovery UI does not
exist yet.

## Security boundary

This protects inactive-slot selection from malformed metadata, oversized or
truncated streams, accidental cross-board images, digest mismatch, and an
installed verifier's signature rejection. ESP-IDF rollback protects the next
boot until the runtime health gate accepts it.

The companion transport is encrypted and pinned to the bonded phone. Direct USB
trusts physical cable access but still requires a valid signed package; its CRC
framing provides corruption detection, not authorization or confidentiality.
Nightglass does **not** provide hardware-backed anti-rollback, Secure Boot V2,
flash encryption, or physical-attacker resistance. The
`secure_version` comparison is software-only because no eFuse anti-rollback
policy is enabled. Enabling secure boot, flash encryption, or burning eFuses
requires a separate production recovery plan and explicit approval.

## Production verifier provisioning

The firmware includes an ECDSA P-256 verifier for DER-encoded signatures over
the SHA-256 digest of `manifest.payload`. It is installed only when
`CONFIG_NIGHTGLASS_OTA_P256_PUBLIC_KEY_HEX` contains an uncompressed SEC1
public point (`04 || X || Y`, exactly 130 hexadecimal characters). Empty or
malformed configuration leaves updates disabled before `esp_ota_begin()` can
erase the inactive slot. A malformed configured key installs a rejecting
verifier sentinel rather than appearing absent, so unsigned-development mode
cannot bypass provisioning failure.

Provisioning validates the SEC1 point against the P-256 curve during boot and
publishes an `update_crypto` health record. Empty configuration is reported as
degraded/disabled; malformed or off-curve configuration is a failed health
state. Once any key text is configured, crypto initialization and verification
errors are signature rejections rather than `unavailable`, so development
unsigned mode cannot bypass a broken provisioned verifier.

Nightglass currently accepts both low-S and high-S mathematically valid ECDSA
signatures because mbedTLS does. Signature bytes are never used as an update
identity; the canonical manifest and image SHA-256 are authoritative. Release
packaging should emit low-S signatures for interoperability and stable
artifacts.

The matching private key is a release credential. It must be created and held
outside the repository by an approved secure credential facility. Do not place
it in `sdkconfig`, build logs, shell arguments, URLs, or package metadata.
Nightglass does not generate or provision that credential automatically.

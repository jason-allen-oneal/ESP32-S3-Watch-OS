# Update and recovery

Nightglass now has an ESP-IDF OTA backend and recovery supervisor. It does not
yet have an update screen, BLE/Wi-Fi transfer protocol, signing key, or concrete
signature algorithm. The production-default build therefore refuses every OTA
image before erasing the inactive slot: no `SignatureVerifier` is installed and
`CONFIG_NIGHTGLASS_OTA_ALLOW_UNSIGNED_DEVELOPMENT` is disabled.

USB release status and the reason raw `esptool`/`otatool` deployment is blocked
are documented in [USB_RELEASE.md](USB_RELEASE.md). In particular, the pinned
host `otatool.py` does not create the `ESP_OTA_IMG_NEW` state required by the
rollback bootloader.

## Install contract

`UpdateService` accepts a fixed-size `UpdateManifest`, its detached signature,
and a sequential stream. Before `esp_ota_begin`, it checks:

- manifest format 1;
- board ID `nightglass-waveshare-esp32s3r8-2.06-v1.0`;
- partition ID `nightglass-32m-r1` and revision 1;
- nonzero declared size no larger than the discovered inactive OTA slot;
- a version different from the running image;
- a secure-version value no lower than the running app descriptor;
- a nonzero SHA-256; and
- a signature over the canonical manifest payload, including the image digest.

The service discovers the next OTA partition through ESP-IDF and additionally
requires an app/OTA subtype distinct from the running partition. Each write is
sequential, nonempty, at most 16 KiB, and unable to exceed the declared image
size. Completion requires the exact byte count, a constant-time SHA-256 match,
ESP-IDF image validation, and an embedded project/version/secure-version match.
Only then is the inactive partition selected for the next boot. The backend
does not reboot automatically.

A verifier implementation can be installed with
`set_signature_verifier()` and must have static lifetime. The interface returns
`verified`, `rejected`, or `unavailable`. A rejected signature is never
bypassed. The optional unsigned-development Kconfig switch bypasses only the
`unavailable` state, emits an explicit warning, and reports
`unsigned_development` rather than `verified`.

## Package artifacts

Verify the reviewed partition geometry and package a built app without signing:

```bash
python3 scripts/verify-partitions.py partitions.csv
python3 scripts/package-update.py build/nightglass.bin build/update-package
```

The package contains `firmware.bin`, `manifest.json`, and the byte-exact
`manifest.payload` a release signer would sign. An unsigned package declares a
null algorithm/file and `verified: false`; it will not install under the default
firmware policy. `--signature` also requires `--signature-algorithm` and copies
the detached signature, but this packaging step does not claim or prove that
the watch has a compatible verifier. No private or public production key is
stored in this repository.

The deterministic release gate also hashes and validates `build/assets.bin`.
That LittleFS image is a separate artifact and is never implicitly written by
an application update.

## Boot health and rollback

Each boot increments `bad_boots` in the `ng_recovery` NVS namespace. The counter
is cleared only after the essential `nvs`, `display`, `touch`, `power`, `clock`,
and `ui` health records remain acceptable through a 60-second gate. Existing
display, power, and clock degraded modes are accepted because they retain the
watch shell and core time/power function; absent or failed health is not.

When the bootloader reports `ESP_OTA_IMG_PENDING_VERIFY`, the gate calls
`esp_ota_mark_app_valid_cancel_rollback()` only after those checks pass and the
boot is not in safe mode. Failure or safe mode calls
`esp_ota_mark_app_invalid_rollback_and_reboot()`. If the gate task cannot start,
the image is never accepted, so ESP-IDF rollback remains armed.

Three consecutive unconfirmed boots enter safe mode. Holding the GPIO10 side
key continuously for the 1.2-second boot sampling window also requests safe
mode. A brief normal press incurs no delay beyond the first released sample.
Safe mode still starts the display, power, clock, fallback watch-face service,
and shell, but leaves optional sensor hardware, audio, activity, BLE, Wi-Fi,
weather, and the audio boot self-test stopped. A dedicated recovery UI does not
exist yet.

## Security boundary

This protects inactive-slot selection from malformed metadata, oversized or
truncated streams, accidental cross-board images, digest mismatch, and an
installed verifier's signature rejection. ESP-IDF rollback protects the next
boot until the runtime health gate accepts it.

It does **not** currently provide an authenticated transport, a provisioned
verification key/algorithm, confidentiality, hardware-backed anti-rollback,
Secure Boot V2, flash encryption, or physical-attacker resistance. The
`secure_version` comparison is software-only because no eFuse anti-rollback
policy is enabled. Enabling secure boot, flash encryption, or burning eFuses
requires a separate production recovery plan and explicit approval.

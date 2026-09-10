# Nightglass OTA signing custody

## Production trust anchor

- Algorithm: ECDSA P-256 with SHA-256; signatures are DER encoded and
  normalized to low-S.
- The firmware contains only the uncompressed SEC1 public point from
  `config/ota-public-key.hex`.
- The operational private key is passphrase-encrypted PKCS#8 sealed with
  `systemd-creds --with-key=host+tpm2` at
  `/etc/credstore.encrypted/nightglass-ota-signing-key`.
- The private key is exposed only to the isolated `nightglass-sign.service`
  process. The service has no network namespace, no home-directory access,
  no capabilities, no core dumps, and a strict writable state directory.
- The passphrase is entered through a local graphical/host-owned secure prompt
  and streamed through an owner-only FIFO. It is never accepted as a command
  argument, environment variable, file in the repository, or chat message.

## Encrypted recovery copies

All copies below are the same passphrase-encrypted PKCS#8 object and must stay
mode `0600` inside an owner-only directory:

- Local: `~/projects/watch/backups/nightglass-ota-signing-key-recovery-20260831.pk8`
- `homepi`: `/home/jason/nightglass-recovery/nightglass-ota-signing-key-recovery-20260831.pk8`
- `mac`: `/Users/r3v3n4n7/nightglass-recovery/nightglass-ota-signing-key-recovery-20260831.pk8`

The three copies were checksum-compared after provisioning. The passphrase is
not stored with any copy.

## Signing procedure

1. Build from a clean reviewed commit with explicit
   `NIGHTGLASS_PROJECT_VERSION` and a strictly increasing
   `NIGHTGLASS_SECURE_VERSION`.
2. Run `scripts/verify-signed-release.sh <new-output-directory>`; it performs
   the host tests, deterministic release gate, signing, and independent package
   verification for that exact embedded identity.
3. Enter the signing passphrase in the local secure prompt.
4. Require the final verifier and printed firmware hash to pass before either
   transport.
5. Preserve the signed manifest, image hash, source commit, secure version,
   and HIL evidence together. Never treat signature bytes as the release ID.

## Rotation and recovery

- Compromise or suspected disclosure: stop signing immediately and quarantine
  the operational credential and every recovery copy.
- Normal rotation requires a bridge firmware signed by the old key that trusts
  the new public key. A lost old key instead requires the reviewed app-only USB
  recovery path.
- Do not burn Secure Boot, flash-encryption, or anti-rollback eFuses as part of
  key rotation. Those are separate irreversible provisioning decisions.
- A replacement release must use a secure version strictly greater than the
  running image. Equality and downgrade are rejected before OTA slot erase.

## Explicit boundaries

- Both native USB and companion OTA require the same detached signature and
  canonical manifest. Companion OTA additionally binds the transfer to an
  encrypted, pinned BLE session; it does not make a rooted phone trustworthy.
- Native USB trusts physical cable access and normally streams through the
  running firmware's inactive-slot verifier and rollback state machine.
- A watch running firmware older than the USB receiver may use the guarded,
  signed one-time ROM bootstrap documented in `USB_RELEASE.md`. That procedure
  writes only the inactive app slot and inactive OTA-selection sector, preserves
  NVS and the current rollback image, and then hands control back to the normal
  health gate. Subsequent releases may use either native USB or companion OTA.
- Production release evidence covers both transports, rollback, interruption,
  and forced-health-failure behavior on the physical watch.

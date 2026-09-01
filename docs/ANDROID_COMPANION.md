# Nightglass Android companion

The minimal Android companion is in `companion/android`. It targets Android 8+
and uses the platform BLE, notification-listener, and media APIs.

## User flow

1. Install the debug/release APK, open Nightglass, and grant Bluetooth and
   notification-posting permissions.
2. Tap **Connect / pair Nightglass** and accept Android's bond prompt. The
   foreground service scans only for the Nightglass service UUID and maintains
   the connection. Enter the random six-digit passkey displayed by the watch.
   The current watch requires authenticated LE Secure Connections and pins the
   resolved phone identity after that visible pairing. After subscribing,
   Android reads the status characteristic and pins the watch address only when
   the watch reports its exact `connected_encrypted`, bonded, peer-pinned
   authorization state for that connection.
3. Tap **Grant notification access** and enable Nightglass in Android settings.
   Eligible notifications are relayed with the protocol's 24/48/96-byte bounds.
   Ongoing notifications, group summaries, and the companion's own notification
   are excluded. Content and notification-key mappings are memory-only.
4. Media actions from the watch are dispatched through Android's media-key
   routing. Dismiss removes the matching notification; mark-read invokes its
   content intent when available.

After installing the peer-pinning firmware over an older bonded build, remove
the old Android Bluetooth bond and pair once more. The watch deliberately will
not convert a pre-existing bond into an identity pin without a newly displayed
passkey. Clearing only the Android app's pinned address is not sufficient; the
system bond must also be removed before pairing again.

**Reset and re-pair watch** presents an explicit destructive confirmation and
must be used while the authorized watch is connected. It sends the watch-side
clear command first and reports success or clears Android's local pin only
after the watch returns the GATT write acknowledgement. The watch then permits
exactly one repeat-pair key replacement from that same peer identity; arbitrary
repeat-pair requests never delete keys. It deliberately refuses an offline
local-only reset, which would leave the two sides with contradictory
allowlists. If the authorized phone is permanently unavailable, recovery
requires a separately controlled USB service/recovery operation; there is no
unauthenticated radio reset.

Call control frames bind each command to a random call session, a state
generation, and a wrap-safe command sequence. Microphone control is explicit
mute/unmute rather than a toggle, so a delayed or duplicated command cannot
invert the state of another call.

## Residual security and privacy boundaries

There is no application-layer message MAC or Android-Keystore-to-watch secret
bootstrap in this release. Adding one safely requires a visible confirmation
and recovery/reset flow on both devices; deriving or silently exchanging a key
over an existing link would merely decorate the same trust boundary. The
current boundary is authenticated BLE Secure Connections, phone/watch identity
pinning, Android app sandboxing, exact frame validation, and replay windows.

Notification bodies remain RAM-only and are never logged or persisted by
Nightglass. A service-level redaction/lock policy can scrub the live cache, but
it is not enabled by default until the watch has an actual PIN/privacy control.
The companion process necessarily has notification-listener access, so a
compromised companion UID, rooted phone, physical watch debug access, or
unencrypted flash/NVS extraction is outside the protection provided here.

## Provisioning

Android implements the firmware's phone-to-watch provisioning frames:

* `0x20`: `version, opcode, ssid_len, pass_len, ssid, passphrase`
* `0x21` (exactly 14 bytes): `version, opcode, flags, units, refresh_min_le16, latitude_e6_le32, longitude_e6_le32`. Flags bit 0 enables weather and bit 1 marks a configured location; units are 0 imperial or 1 metric.
* `0x22`: `version, opcode`, clearing the watch's runtime Wi-Fi configuration.
* `0x23` (exactly 18 bytes): version, opcode, metric/day flags, reserved byte,
  observed Unix time, snapshot age seconds, signed temperature/apparent-temperature tenths,
  weather code, and unsigned wind-speed tenths. All multibyte fields are
  little-endian.

SSID is bounded to 32 printable-ASCII bytes, passphrase to 64, refresh to
15–360 minutes, latitude to
±90,000,000 microdegrees, and longitude to ±180,000,000. It must use the same
encrypted-write characteristic as other phone-to-watch frames and must be
accepted only on an encrypted bonded link. The app does not persist the
passphrase, clears its UI field after sending, and never logs frame content.
Firmware validates these bounds before applying settings. The app queues GATT
writes so the Wi-Fi and weather frames are sent in order.

The phone stores only the non-secret weather coordinates, units, and refresh
interval. While the encrypted BLE link is ready it fetches bounded HTTPS
Open-Meteo responses using whichever Internet transport Android currently has,
sends an immediate snapshot after reconnect, and refreshes at the configured
15–360 minute interval. It does not log locations, URLs, responses, or payloads.
Direct watch Wi-Fi is optional rather than required for weather.

The watch keeps Wi-Fi credentials only in RAM and requires provisioning again
after reboot. Persistent secret storage remains disabled until encrypted NVS
and a device-key policy are available.

## Build

Use JDK 17 and Android SDK 35:

```text
gradle :app:testDebugUnitTest :app:assembleDebug
```

No signing secrets belong in the repository. Production builds should use a
keystore supplied through the build host's secure credential facility.

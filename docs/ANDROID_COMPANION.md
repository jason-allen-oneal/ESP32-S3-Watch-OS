# Nightglass Android companion

The minimal Android companion is in `companion/android`. It targets Android 8+
and uses the platform BLE, notification-listener, and media APIs.

## User flow

1. Install the debug/release APK, open Nightglass, and grant Bluetooth and
   notification-posting permissions.
2. Tap **Connect / pair Nightglass** and accept Android's bond prompt. The
   foreground service scans only for the Nightglass service UUID and maintains
   the connection. The current watch uses LE Secure Connections “Just Works”;
   encryption is provided, but the pairing is not MITM-authenticated.
3. Tap **Grant notification access** and enable Nightglass in Android settings.
   Eligible notifications are relayed with the protocol's 24/48/96-byte bounds.
   Ongoing notifications, group summaries, and the companion's own notification
   are excluded. Content and notification-key mappings are memory-only.
4. Media actions from the watch are dispatched through Android's media-key
   routing. Dismiss removes the matching notification; mark-read invokes its
   content intent when available.

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

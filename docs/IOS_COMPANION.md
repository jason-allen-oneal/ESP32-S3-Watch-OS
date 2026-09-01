# Nightglass iPhone companion

The iOS target in `companion/ios` is a small CoreBluetooth **central** for the
Nightglass custom GATT service. It scans only for the Nightglass service,
connects, discovers the three protocol-v1 characteristics, reads/subscribes to
status, and subscribes to watch-to-phone commands.

## Supported today

- Service: `7a3b4001-6b6f-4f72-726f-772d6e696768`
- Status (`...4002`): encrypted read/notify; the app validates the five-byte
  frame and protocol version.
- Phone-to-watch (`...4003`): encrypted write with response. The adapter sends
  the firmware-defined Wi-Fi and network/weather settings frames.
- Watch-to-phone (`...4004`): notify. The adapter parses media/dismiss/read
  command envelopes without logging bytes or notification content.

Bonding and link encryption are enforced by the firmware's characteristic
permissions. iOS owns the pairing UI and key storage. Current watch firmware
uses authenticated LE Secure Connections with a six-digit passkey displayed on
the watch; an iOS port must support that flow and the watch-side identity pin.

## Settings and Wi-Fi provisioning boundary

Provisioning uses the existing encrypted inbound characteristic:

- `0x20`: `[1, 0x20, ssidLen, passwordLen, ssid, password]`, with 1–32 SSID
  bytes and at most 64 password bytes;
- `0x21`: exactly 14 bytes containing flags (enabled/location configured), units
  (`0` imperial, `1` metric), refresh minutes, and signed latitude/longitude in
  millionths, with all multibyte fields little-endian;
- `0x22`: `[1, 0x22]` clears runtime Wi-Fi credentials.

The password uses `SecureField`, is cleared from view state immediately after a
send attempt, and is never logged or persisted by the app. CoreBluetooth writes
with response, although protocol v1 does not yet provide a semantic result
frame distinguishing accepted/rejected settings; the UI reports GATT receipt,
not “saved” or “connected.”

The watch keeps Wi-Fi credentials only in RAM and requires provisioning again
after reboot. Persistent secret storage remains disabled until encrypted NVS
and a device-key policy are available.

## iOS notification and media constraints

An ordinary iOS app cannot read the device-wide notification center or mirror
all notifications from other apps. Notification Service Extensions can inspect
only notifications belonging to their own app, and only within Apple's
extension lifecycle. UserNotification APIs do not grant global notification
listener access comparable to Android's Notification Listener service.

Apple Notification Center Service (ANCS) and Apple Media Service (AMS) are BLE
profiles consumed by a paired **accessory**. They are not general APIs that let
this companion app harvest all iPhone notifications or control every media app.
If Nightglass firmware later implements ANCS/AMS directly and satisfies Apple's
platform/accessory requirements, that is a separate accessory-side adapter and
security review; this custom companion must not claim it provides those flows.

Consequently, protocol-v1 media, dismiss, and mark-read commands are parsed but
not executed on iOS. The UI does not promise notification mirroring or global
media control. App-owned notifications or app-owned playback could be added
later, with narrowly scoped behavior stated in the UI.

## Privacy and validation

- Never log characteristic payloads, notification text, Wi-Fi credentials, or
  stable device identifiers.
- Keep notification data ephemeral; do not add analytics or background capture.
- BLE testing requires a signed build on a physical iPhone. Simulator behavior
  is not hardware-in-the-loop validation.
- Generate the project from `project.yml` with XcodeGen, then build in Xcode 15
  or newer. The current macOS node has Command Line Tools but no selected full
  Xcode installation, so this target is source-complete but not locally built.

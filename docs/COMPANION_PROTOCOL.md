# Nightglass companion protocol v1

Nightglass exposes one encrypted custom BLE GATT service. The watch remains a
bounded display/control endpoint; phone operating-system access stays in a
companion adapter.

## Security and privacy

- Pairing uses bonded LE Secure Connections. The current screen has no passkey
  ceremony, so pairing is encrypted but uses Just Works and is not MITM
  authenticated. A future passkey UI can raise that guarantee without changing
  the protocol.
- Notification and command characteristics require link encryption.
- Notification content is never written to logs or NVS. The six-item inbox is
  RAM-only and disappears on reboot.
- Text fields are bounded and converted to printable ASCII for the current
  embedded font set.

## Platform boundary

- Android requires a companion app with Notification Listener permission and a
  MediaSession adapter.
- iPhone can use a companion app implementing this service. Direct ANCS/AMS is
  a separate adapter and is not claimed by this protocol.
- Until a phone adapter is installed, the watch truthfully reports advertising,
  paired, connected, or unavailable states; it does not fabricate content.
- Bluetooth defaults off so the watch can enter manual light sleep. Enabling
  companion mode keeps the display power policy active but intentionally
  inhibits manual light sleep until the radio is disabled.

## GATT service

Service UUID: `7a3b4001-6b6f-4f72-726f-772d6e696768`

- Status `...4002`: encrypted read/notify. Five bytes: protocol version, link
  state, inbox count, encrypted flag, bonded flag.
- Phone-to-watch `...4003`: encrypted write of a complete bounded frame.
- Watch-to-phone `...4004`: encrypted notification carrying media and inbox
  actions.

All multibyte integers are little-endian. Upsert frames contain version `1`,
opcode `1`, notification ID, category, app/title/body lengths, then the three
strings. Opcodes `2` and `3` remove one or clear all notifications. Outbound
opcodes `0x10`, `0x11`, and `0x12` represent media, dismiss, and mark-read
actions.

Provisioning opcodes are accepted only on the encrypted phone-to-watch
characteristic. `0x20` carries bounded SSID/password lengths followed by their
bytes, `0x21` carries weather enable/location/unit/refresh settings, and `0x22`
clears the runtime Wi-Fi credential. Credentials are never returned over GATT or
written to logs. Just Works pairing protects against passive interception but
does not provide MITM authentication; a passkey ceremony remains required for
hostile-radio environments.

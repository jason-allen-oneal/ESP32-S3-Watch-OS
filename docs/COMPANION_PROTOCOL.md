# Nightglass companion protocol v1

The optional [premium extension](PREMIUM_PROTOCOL.md) adds appearance profiles
and bounded richer media/conversation snapshots without changing v1 status or
existing characteristic handles.

Nightglass exposes one encrypted custom BLE GATT service. The watch remains a
bounded display/control endpoint; phone operating-system access stays in a
companion adapter.

## Security and privacy

- Pairing uses bonded LE Secure Connections with a random six-digit passkey
  displayed on the watch. GATT reads, writes, and notifications require an
  authenticated encrypted link. The Android companion pins the successfully
  authenticated device address for subsequent reconnects. The watch also pins
  the peer's resolved NimBLE identity address in NVS, but only after a new
  passkey-display pairing completes as authenticated and bonded. A different
  bonded identity is disconnected before any application frame is accepted.
- Upgrading an already bonded protocol-v1 watch does not silently grandfather
  that bond into the new watch-side allowlist. Remove the phone's old bond and
  pair again so the displayed-passkey bootstrap can create the identity pin.
- This protocol does not claim an application-layer MAC. BLE Secure Connections
  plus the bidirectional identity pins protect the radio link, but compromise
  of the Android companion process/UID, phone root, watch firmware, or BLE key
  storage remains in scope for a future authenticated application session.
- Peer reset opcode `0x25` is accepted only when the callback connection handle,
  resolved peer identity, and authorization generation still match immediately
  before the NVS clear. It clears the watch identity pin before Android clears
  its local address pin; Android reports success only after the GATT write ACK.
  The command arms one RAM-only repeat-pair permit for that same identity.
  Without that explicit authenticated reset, repeat-pair requests never delete
  bond keys. Offline/local-only reset is refused. The user must then remove the
  Android system bond before pairing again.
- Notification and command characteristics require link encryption.
- Notification content is never written to logs or NVS. The six-item inbox is
  RAM-only and disappears on reboot.
- The service exposes a runtime notification privacy policy. Selecting
  `always_redact` scrubs cached app/title/body details and disables replies
  immediately; selecting `show_details` requires a fresh companion sync and
  never resurrects text that was scrubbed. There is no PIN lock in the product
  surface; physical RAM/debug access and a deliberately unlocked display remain
  residual privacy risks.
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

- Status `...4002`: authenticated encrypted read/notify. Six bytes: protocol
  version, link state, inbox count, encrypted flag, bonded flag, watch-side peer
  identity-pinned flag. Android does not mark the link ready or persist its
  address pin until an exact status read proves `connected_encrypted` with all
  three authorization flags set.
- Phone-to-watch `...4003`: encrypted write of a complete bounded frame.
- Watch-to-phone `...4004`: encrypted notification carrying media and inbox
  actions.

All multibyte integers are little-endian. Upsert frames contain version `1`,
opcode `1`, notification ID, category, app/title/body lengths, then the three
strings. Category bit 7 marks a newly posted alert; cache synchronization and
reconnect replay leave it clear so the watch updates silently. Opcodes `2` and
`3` remove one or clear all notifications. Opcode `0x04` carries bounded media
state (available/playing flags plus title and artist). Outbound opcodes `0x10`,
`0x11`, and `0x12` represent media, dismiss, and open-on-phone actions.
Notification category bit 6 means Android exposed one unambiguous free-form
inline reply action.

Reply opcode `0x13` contains sequence, opaque notification handle, random
request nonce, one-byte text length, and 1-96 printable ASCII bytes. Android
deduplicates the nonce before invoking `RemoteInput`; it returns opcode `0x24`
with status, handle, and nonce. The watch permits one in-flight reply, clears it
on disconnect or after 15 seconds, and reports only "sent to phone" because a
notification action cannot prove downstream SMS/email delivery. Notification
handles are random, RAM-only, collision-checked, and invalidated on listener
resynchronization.

Notification audio requires the matching companion build that sets category
bit 7 for live posts. Older version-1 companions remain wire-compatible for
notification display, but intentionally produce silent notifications because
they cannot distinguish a live post from reconnect/cache replay.

Provisioning opcodes are accepted only on the encrypted phone-to-watch
characteristic. `0x20` carries bounded SSID/password lengths followed by their
bytes, `0x21` carries weather enable/location/unit/refresh settings, and `0x22`
clears the runtime Wi-Fi credential. Credentials are never returned over GATT or
written to logs.

Opcode `0x23` carries the phone weather proxy snapshot in exactly 18 bytes:
`version`, `opcode`, flags (metric bit 0, day bit 1), zero reserved byte,
observed Unix epoch `u32`, snapshot age seconds `u16`, temperature tenths `i16`, apparent-temperature
tenths `i16`, WMO weather code `u16`, and wind-speed tenths `u16`. All integers
are little-endian. The watch rejects unknown flags, nonzero reserved data,
invalid ranges, observations older than six hours, observations more than five
minutes in the future, unit mismatches, and data older than its current source.

## Phone integrations

- `0x05` carries at most three agenda events for the next 24 hours, with bounded
  times, title, and location. Descriptions, attendees, and accounts stay local.
- `0x06` carries phone battery percentage and charging/power-save flags.
- `0x07` adds media seekability and bounded position/duration. Media commands
  6/7 seek by 15 seconds when Android advertises seek support; command 8 stops
  the active session and command 9 restarts it from the beginning when seek is
  supported.
- `0x08` carries generic call state without number/contact identity. The exact
  frame adds a random nonzero call-session ID and a nonzero state generation;
  idle state carries zero for both. Outbound `0x14` is exactly 11 bytes and
  echoes session ID and generation with a nonzero 16-bit command sequence.
  Android accepts only the forward half of the wrap-safe sequence space for the
  current session/generation, consuming a sequence before invoking Telecom.
  Commands are answer, reject, explicit mute, and explicit unmute; there is no
  replay-sensitive mute toggle and no call audio over BLE. Legacy four-byte
  call commands are rejected.
- Outbound `0x15` starts/stops a bounded 30-second phone ring, launches the
  system camera intent, or opens the installed Spotify (`command=4`) or
  Discord (`command=5`) app. Unknown command values are rejected by the
  companion parser.

These frames retain the bonded, pinned, encrypted GATT boundary and MTU bounds.
Logs contain opcode, length, and status, not private payload content. Missing
Android calendar/telephony permission produces empty or unavailable state.
All other watch action frames require exact lengths, known command values,
nonzero handles/sequences, and a wrap-safe forward 8-bit sequence window on
Android, so duplicate GATT deliveries cannot repeat their side effects.

## Signed update frames

Both the encrypted BLE installer and native USB installer feed the same bounded
update state machine. Request opcodes are `0x30` begin, `0x31` sequential data,
`0x32` finish/apply, `0x33` abort, and `0x34` status query. The watch replies
with opcode `0x35` in an exact 22-byte frame:

| Bytes | Field |
|---:|---|
| 0 | protocol version `1` |
| 1 | status opcode `0x35` |
| 2-9 | session `u64` |
| 10 | update state |
| 11 | signature state |
| 12 | result code |
| 13 | acknowledged request opcode |
| 14-17 | expected image bytes `u32` |
| 18-21 | received image bytes `u32` |

All integers are little-endian. Byte 13 is zero on BLE for compatibility with
deployed companions and echoes `0x30`-`0x34` on USB so the host can correlate a
pipelined response with its request. Native USB wraps each payload in `NGU1`, a
little-endian `u16` payload length, and IEEE CRC32; that envelope is transport
framing, not a replacement for package signature verification.

## OpenClaw voice frames

Voice request frames use opcodes `0x40`-`0x43` and the existing bounded
8 kHz mu-law capture contract. The codec byte on `0x40` keeps codec `1` in its
low six bits and carries per-turn destination flags in the high bits: bit `7`
is the **spoken replies** opt-in and bit `6` is **Discord voice reply**. With
both bits clear, the existing text-only contract is unchanged. A Discord voice
reply is capped at 60 seconds; the matching companion converts it to a
short-lived WAV and opens Discord's user-confirmed share composer rather than
calling a Discord API or choosing a channel. Non-default flags require the
matching companion build. A text response uses `0x45`-
`0x48` and remains capped at 2,048 printable bytes. When the spoken-reply
opt-in is set,
the phone may append an audio response with a distinct random response ID:

- `0x4A`: 20-byte begin (`session`, `response_id`, total mu-law bytes, CRC32,
  codec `1`, rate `8`), capped at 96,000 bytes.
- `0x4B`: data with a 32-bit byte offset and up to 230 payload bytes.
- `0x4C`: 18-byte end binding the exact byte count and CRC32.

The watch accepts audio only after the matching text response completed, checks
the CRC and exact offsets, buffers it in bounded PSRAM, and routes it through
the existing PA-gated audio owner. It wipes the buffer on completion,
cancellation, disconnect, malformed data, or failed playback. TTS failure,
muted/DND audio, transport exhaustion, and unavailable hardware leave the text
response intact; no provider credential or audio file crosses the watch
boundary.

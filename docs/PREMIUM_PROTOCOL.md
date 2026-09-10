# Premium companion extension, revision 1

Additive to companion v1; all multi-byte integers below are little-endian unless
specified. The existing six-byte authorization status and existing characteristic
handles remain unchanged. The appended, authenticated-readable capability UUID is
`7a3b4005-6b6f-4f72-726f-772d6e696768`, with bytes `01 01 07` (version, revision,
profile/display/rich-content feature bits). After normal authorization, the app
uses its presence as the initial support signal and sends one read-only `01 71`
query. An exact validated nonce-zero profile response can also prove support if
Android still caches the pre-upgrade characteristic list. Other extension writes
and actions remain disabled until support is established. Legacy firmware may
reject the query without resetting the bond. Notifications must belong to the
current GATT client, outbound characteristic and authorized link generation;
an ACK, malformed frame or write-response nonce does not establish support.
Profile queries/saves have a separate eight-frame bounded queue, ahead of rich
content and ordinary phone/notification synchronization (OTA and voice retain
priority). Notification overflow or replacement of a rich document cannot drop
a settings request. Disconnect wipes all pending profile frames; reconnection
queries the watch without automatically replaying settings writes.

## Appearance profile

`01 71` queries the saved profile. A profile write is exactly 48 bytes with opcode
`70`; its response uses `71` and echoes the request nonce after NVS commit.
Query replies use nonce zero. User writes require a nonzero nonce. Reserved and
unused name bytes must be zero. No runtime in-memory struct is persisted directly.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 1 | Protocol 1 |
| 1 | 1 | `70` write or `71` response (hex) |
| 2 | 4 | Request nonce; zero only in query response/storage |
| 6 | 1 | Face: 0 Classic, 1 Revenant, 2 Personal |
| 7 | 1 | Flags: bit 0 AOD, bit 1 Large Text, bit 2 Reduce Motion |
| 8 | 3 | Accent RGB, in **R, G, B** order |
| 11 | 3 | Three complication IDs, each 0–7 |
| 14 | 5 | Unique permutation of all five card IDs, 0–4 |
| 19 | 1 | Enabled-card bit mask, nonzero, at most 31 |
| 20 | 1 | Name length, 1–24 |
| 21 | 24 | Printable ASCII name, zero-padded, not all spaces |
| 45 | 3 | Zero |

Complication IDs: battery, steps, weather, timer, next alarm, notifications,
distance, phone. Card IDs: Next up, Move, Weather, Media, Inbox. Accent RGB must
satisfy `3R + 6G + B >= 960` to reject near-black imported text colors.

## Rich content transaction

The watch allocates one token-bound request. Kind 1 is media (target zero); kind 2
is a current visible notification (target its notification ID). In each frame
the first two bytes are version 1 and the hexadecimal opcode below.

| Opcode | Direction | Payload after version/opcode |
| --- | --- | --- |
| `72` | Watch → phone | token u32, kind u8, target u32 |
| `73` | Phone → watch | token u32, kind u8, target u32, total u16 |
| `74` | Phone → watch | token u32, contiguous offset u16, 1–236 bytes |
| `75` | Phone → watch | token u32, CRC32 u32 |
| `76` | Watch → phone | token u32, action u8, index u8, target u32, sequence u8 |
| `77` | Phone → watch | token u32, echoed sequence u8, result u8 |

Frames never exceed negotiated ATT payload or 244 bytes. Total content is 1–4096
bytes. CRC32 is standard reflected IEEE (`0xedb88320`, initial/final XOR
`0xffffffff`). Begin must match requested kind/target/token; data must be contiguous;
only a complete matching checksum publishes content. Superseded/late fragments
are ignored without resetting a healthy bond. Each request expires after 60 s.

The completed document has this layout:

1. Version u8 = 1, kind u8, action count u8 (0–7), artwork flag u8 (0/1).
2. Body length u16, followed by 1–1600 printable ASCII/newline bytes (no NUL).
3. Each action: action ID u8, index u8, caption length u8 (1–60), ASCII caption.
4. If artwork is present, exactly 3200 bytes: 40×40 RGB565, little-endian pixels.
   Artwork is allowed only for media. No trailing bytes are accepted.

Media action IDs: 1 select queue index 0–3; 2 output volume down; 3 output volume
up; 4 open the originating player on the phone to choose an output. Actions 2–4
use index zero. Notification IDs: 5 invoke a provided non-RemoteInput,
non-auth-required notification action (index 0–3); 6 open the original
conversation/attachment PendingIntent (index zero).

Result 0 means handoff to the app succeeded, not that an external provider
completed the operation or that Android displayed its activity. Results 1–5 mean
missing, unsupported, authorization required, stale/invalid or failed. Actions
require current token/target, an actually offered action, live matching provider
identity and a nonzero forward 8-bit sequence (half-range wrap rule). A repeated
request cannot reset replay/expiry state. Normal user operation requests a new
random token when reopening content. Response images/messages are memory-only.

## Inert OpenClaw suggestion suffix

A completed response may end with exactly one newline and one of:
`[WATCH:TIMER:60]` through `[WATCH:TIMER:3600]`, `[WATCH:OPEN:SPOTIFY]`,
`[WATCH:OPEN:PHONE]`, `[WATCH:OPEN:INBOX]`, `[WATCH:PHONE:RING]`.

No leading-zero/signed timer, duplicate marker, suffix text, URL or other action
is accepted. The UI binds the suggestion to the completed session and response
fingerprint. It requires review plus confirmation within eight seconds, prevents
reuse, and never automatically executes suggestions. Timer start is an atomic
worker-side idle check. Gateway permission mode remains read-only. These markers
are not commands on the BLE transport and are not read aloud.

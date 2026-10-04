# Progressive watch voice playback

The first buffered ElevenLabs implementation plays successfully, but the owner
reported a long text-to-audio delay. Phone GATT logs showed audio-data writes
spaced roughly 120–360 ms apart. The watch's idle connection requests 40–60 ms
intervals with peripheral latency 4; full-clip buffering made that delay audible.

This change keeps authenticated Gateway `talk.speak` and its existing protected
ElevenLabs key unchanged. It does **not** stream synthesis from the provider:
the Gateway still returns the complete bounded mu-law clip. It streams the
Bluetooth-to-speaker leg instead, and temporarily requests Android HIGH
connection priority during spoken reply generation/transfer. Balanced priority
is restored after terminal delivery/cancellation, retaining normal idle policy.

## Wire and playback

- Codec marker 1 retains the old buffered path. Marker 2 selects progressive
  playback; both contain raw 8 kHz mu-law, not different audio encodings.
- Opcode `0x4d` carries session, response ID, sequential byte offset, up to 226
  sample bytes, and CRC32 covering the entire header and payload. Every chunk
  is checked before publication to the audio worker. Authenticated encrypted
  BLE, authorization-generation fencing and GATT write acknowledgments remain.
- Playback begins after min(2048, total) verified bytes arrive: about 256 ms of
  audio, **not a guaranteed 256 ms wall-clock latency**. The worker copies only
  published ranges under the response mutex and feeds the existing mono I2S
  path in 64 ms output blocks. It keeps the existing bounded 96,000-byte PSRAM
  allocation; this is progressive consumption, not an unbounded queue or ring.
- Missing data waits at most five seconds per read and thirty seconds for the
  entire progressive playback. Cancel, link loss, ownership mismatch and invalid
  chunks stop output. The final whole-clip CRC/end is still required; already
  played chunks were independently checked, so an invalid end cannot retroactively
  change what was played. No replay/resume of a cancelled turn is supported.
- Allocation ownership stays with the producer until start and with the audio
  worker until its cleanup callback. Producer access and free are synchronized;
  cancellation marks the stream failed rather than freeing an in-use buffer.
- The companion streaming APK must not be installed before compatible firmware.
  Legacy companions still work with the new firmware through codec marker 1.

## Verification / installation

Production progressive-reader host tests exercise unpublished/partial ranges,
verified-end gating, revoked ownership, cancellation, and starvation with mutex
and task adapters. Wire tests reject corruption of each binding/payload/CRC
field. Companion tests check MTU bounds, ordered offsets, checksums and terminal
ownership. These do not establish physical BLE throughput or audible continuity.

Release candidate: firmware 0.2.20 / secure20. Build, signed-release gate,
installation, health acceptance and physical latency results must be recorded
before calling this installed or proven. At implementation time only the phone
is connected by USB; the watch connection and signed firmware install remain
pending. Existing app data, bonds, settings, routes and voice credentials are
preserved; no VPS or phone VPN is added.

## Notification sound preference

Owner requested an opt-in, not permanent suppression. Companion Settings →
Notifications & phone features → Notification sounds is a persistent checkbox,
off by default. Newly posted notifications carry the audible-alert bit only
when opted in. Notification cards continue to relay; reconnect replay remains
silent. This setting does not change global watch volume/mute, spoken replies,
alarms, timers or phone sounds. Live UI/persistence verification follows install.

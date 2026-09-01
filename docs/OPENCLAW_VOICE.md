# Nightglass OpenClaw Voice

## First release contract

Nightglass voice is a foreground, push-to-talk feature. It is not a wake-word
listener and it never records while its app is not visible.

- Press and hold to record; release submits; cancel discards.
- Capture defaults to 60 seconds. The selectable limits are 30, 60, 120 and
  300 seconds; five minutes is the hard maximum.
- The watch captures 16 kHz mono PCM16 and converts it in memory to 8 kHz
  G.711 mu-law for BLE transport (8 KiB/s, 2,400,000-byte maximum).
- The watch closes the codec/I2S session before it starts BLE upload.
- Audio is never written to watch flash, Android storage, logs, analytics, or
  crash reports. All buffers are wiped after success, failure, cancellation,
  timeout, disconnect, alarm/call interruption, screen exit, or OTA activity.
- The first release renders a bounded plain-text response on the watch.
  Automatic spoken replies are intentionally deferred.

## Trust boundary

The existing encrypted, authenticated, bonded and pinned Nightglass BLE link
is the only watch transport. The watch stores no OpenClaw URL, token, device
identity, agent selection, model selection, session key, or tool policy.

The Android companion pairs through OpenClaw's official voice-node bootstrap
and must reject credentials whose scopes are not exactly:

- `operator.read`
- `operator.talk`

The companion stores its device identity and token with Android Keystore,
requires verified `wss://` or managed private ingress, and hardcodes the Talk
session policy. Watch speech cannot modify that policy.

Clearing setup in the companion wipes only its local encrypted credential. It
does not revoke the already-issued Gateway device token. Full deauthorization
requires explicit device revocation in OpenClaw as well as clearing the local
companion setup; the companion reports this distinction rather than claiming
that a local clear performed remote revocation.

## BLE protocol

Every voice turn uses a random nonzero 32-bit session identifier. The watch
records first, then sends a begin frame, sequenced data frames, and an end
frame binding the exact encoded byte count and CRC32. The phone grants a small
receiver-credit window and acknowledges the highest contiguous byte offset.

- Maximum ATT frame: 244 bytes.
- Maximum encoded request: 2,400,000 bytes (300 seconds at 8 kHz mu-law).
- Maximum returned UTF-8 text: 2,048 bytes.
- One capture/upload/response turn at a time.
- Late frames, stale acknowledgements and stale responses are rejected.
- A disconnect cancels the turn; it is never replayed after reconnect.

## OpenClaw session

The companion owns one memory-only encoded turn at a time. It decodes mu-law
to 24 kHz PCM16 in five-second chunks, wiping each chunk after the acknowledged
Gateway append instead of allocating a whole-turn PCM copy. It then uses the
official Gateway Talk protocol:

- `talk.session.create`
- `talk.session.appendAudio`
- `talk.session.close`
- correlated `talk.event` messages

Mode is realtime gateway relay with `brain=agent-consult`. The first release
uses final transcript/text events and ignores response audio. OpenClaw's
existing exact-action voice confirmation remains authoritative for
consequential actions; the companion never parses or manufactures a
confirmation itself.

## Release gates

1. Codec and protocol host tests cover bounds, mutation and stale sessions.
2. Watch capture/upload survives packet loss, timeout and disconnect without
   retaining audio or starving AMOLED/BLE DMA.
3. Voice-node bootstrap succeeds over verified private WSS and proves exact
   read/talk scopes, persistence and revocation.
4. Lost-network capture is discarded rather than replayed.
5. Exact-action confirmation HIL proves block, later confirmation, refusal,
   replay rejection, expiry and parameter binding.
6. A 300-second microphone/BLE/display soak completes without heap corruption,
   reboot, PA leakage, audio persistence or content logging; 30/60/120-second
   turns also stop at their configured limits.
7. Provider processing and transcript/session retention are visible in the
   OpenClaw app before recording.

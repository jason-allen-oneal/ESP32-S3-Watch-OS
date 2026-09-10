# Nightglass OpenClaw Voice

## End-to-end contract

Nightglass voice is a foreground push-to-talk feature:

`watch mic -> encrypted BLE -> Android companion -> verified WSS -> Gateway host -> local whisper.cpp -> transcript -> existing agent/OAuth model -> bounded text -> phone -> watch`

No OpenClaw plugin, core patch, or additional agent is required. The companion
uses only stock Gateway RPCs and dedicated `main`-agent sessions, inheriting
the main agent's existing ChatGPT OAuth route, workspace, skills, tools, and
privileges.

- Press and hold to record; release submits; cancel discards.
- Capture defaults to 60 seconds. Selectable limits are 30, 60, 120 and 300
  seconds; five minutes is the hard maximum.
- The watch captures 16 kHz mono PCM16 and converts it in memory to 8 kHz
  G.711 mu-law for BLE transport (8 KiB/s, 2,400,000-byte maximum).
- The watch closes the codec/I2S session before BLE upload begins.
- Watch and phone raw-audio byte arrays are memory-only and wiped after
  success, failure, cancellation, timeout, disconnect, alarm/call
  interruption, screen exit, or OTA activity. The JVM/OkHttp base64 and
  WebSocket text serialization is immutable and can remain in managed heap
  until garbage collection; it is never intentionally logged or written by
  the companion.
- The Gateway stages the WAV as managed inbound media for transcription. Its
  configured media-retention policy governs that host-side copy.
- Local whisper.cpp receives the audio. On success, the Gateway removes the
  audio attachment from the model prompt and supplies the transcript to the
  existing OAuth-backed model. If transcription fails, raw audio bytes remain
  host-local; the main-agent session receives no usable audio and the request
  requires the fixed failure response `Voice transcription failed.`
- The watch renders the bounded plain-text response in fixed, word-aware pages;
  it never auto-scrolls the answer. Previous/Next controls keep each page
  stable while reading.
- Spoken replies are optional and default off. When enabled on the watch,
  Android uses its local platform TTS engine to synthesize at most the first
  480 response characters, converts the result to 8 kHz G.711 mu-law, and
  sends at most 12 seconds (96,000 bytes) over the same authenticated BLE
  link. The watch buffers that bounded stream in PSRAM, plays it through the
  existing PA-gated speaker owner, and wipes it after playback or cancellation.
  Text remains the fallback if TTS, transport, audio policy, or hardware is
  unavailable. No provider credential, recording, or TTS file is retained.
- Discord voice-note replies are a separate watch destination. They reuse the
  encrypted capture/upload path but never enter OpenClaw; the companion hands
  the verified recording to Discord's user-confirmed Android share composer,
  where the user selects the conversation and sends it.

## Trust and authorization boundary

The existing encrypted, authenticated, bonded and pinned Nightglass BLE link
is the only watch transport. The watch stores no OpenClaw URL, token, device
identity, agent selection, model selection, session key, or tool policy.

Provision the Android companion with stock `openclaw qr --voice-node`. That
profile initially issues exactly `operator.read` and `operator.talk`. On the
first voice turn, the companion uses the stock approval-bound
`device.scopes.requestUpgrade` and `device.scopes.waitUpgrade` flow to request
`operator.write`. OpenClaw requires an upgrade to retain the existing scopes,
so the final accepted profile is exactly:

- `operator.read`
- `operator.talk`
- `operator.write`

The companion rejects every other profile, including any token carrying
`operator.admin`, approvals, pairing, questions, or secret access. The upgrade
must be approved from the Control UI or another already-authorized device; the
phone cannot approve its own request.

`operator.write` is currently Gateway-wide rather than session-scoped. The
companion code calls only the three stock session/chat methods listed below, but
the credential itself is not cryptographically limited to one session. This is
the remaining platform boundary of the plugin-free design.

The Gateway routes every watch conversation to a dedicated session owned by
the existing `main` agent. The session intentionally inherits the main agent's
OAuth model, workspace, skills, tools, and privileges. No separate agent or
deny-all tool policy is created. Each session is created with
`permissionMode: "read-only"`; every `chat.send` uses compare-and-swap
assertions requiring that session mode and no session-level tool override. This
does not remove the main agent's configured tools; it prevents an unexpected
session-policy mutation from being accepted silently.

The companion stores its device identity and token with Android Keystore and
requires verified `wss://` or managed private ingress. Clearing setup wipes
only the phone's encrypted credential; full deauthorization also requires
revoking the device in OpenClaw.

## BLE protocol

Every voice turn uses a random nonzero 32-bit session identifier. The watch
records first, then sends a begin frame, sequenced data frames, and an end frame
binding the exact encoded byte count and CRC32. The phone grants a small
receiver-credit window and acknowledges the highest contiguous byte offset.

- Maximum ATT frame: 244 bytes.
- Maximum encoded request: 2,400,000 bytes (300 seconds at 8 kHz mu-law).
- Maximum returned UTF-8 text: 2,048 bytes.
- Maximum spoken reply: 96,000 bytes (12 seconds at 8 kHz mu-law).
- One capture/upload/response turn at a time.
- Late frames, stale acknowledgements and stale responses are rejected.
- A disconnect cancels the turn; it is never replayed after reconnect.

## Stock Gateway session flow

The phone expands the complete bounded mu-law turn into an 8 kHz mono PCM16
WAV, attaches it as base64 `audio/wav`, and then uses:

- `sessions.create` with `agentId=main` and a phone-generated, strictly
  validated key under
  `agent:main:dashboard:*`; the returned key is validated and stored in
  the companion's encrypted credential record
- `chat.send` with `deliver=false`, `permissionMode=read-only` policy CAS, no
  session tool overrides, and a unique idempotency/run key
- correlated `chat` events for bounded deltas and the final assistant snapshot
- a bounded server-side run timeout plus acknowledged `chat.abort` when the
  watch cancels an active run or the client exits after submission without a
  confirmed terminal event; cleanup retries once over a fresh authenticated
  socket when the turn connection is unusable
- another `sessions.create` when the companion's **Start new watch
  conversation** button is pressed; the new validated key atomically replaces
  the prior active-session pointer

Normal turns reuse the stored key so follow-up questions retain context. A new
conversation gets a new session and leaves the old transcript intact; the phone
only overwrites its encrypted active-session pointer. The dedicated session
does not select a separate provider or bypass ChatGPT OAuth.

## Gateway-host local Whisper configuration

The Gateway must have one explicit audio-capable CLI model and no other
audio-capable entry, so configured cloud transcription cannot become a
fallback. Existing non-audio media entries must be preserved. For the current
`rev` host, the verified binary and model paths are:

- `/usr/bin/whisper-cli`
- `~/projects/models/whisper/base.en/ggml-base.en.bin`

The reviewed merge template contains only the local Whisper media settings:

`gateway/openclaw-nightglass-config.example.json5`

Its effective audio portion is shown below. Do not replace an existing
`tools.media.models` array without preserving its non-audio entries, and do not
retain any other entry whose capabilities include `audio`.

```json5
{
  tools: {
    media: {
      models: [
        {
          type: "cli",
          command: "/usr/bin/whisper-cli",
          args: [
            "-m",
            "~/projects/models/whisper/base.en/ggml-base.en.bin",
            "-otxt",
            "-of",
            "{{OutputBase}}",
            "-nt",
            "{{AttachmentPath}}",
          ],
          capabilities: ["audio"],
          maxBytes: 5000000,
          timeoutSeconds: 360,
        },
      ],
      audio: {
        enabled: true,
        preferredModel: "cli:/usr/bin/whisper-cli",
        maxBytes: 5000000,
        timeoutSeconds: 360,
      },
    },
  },
}
```

This configuration is an activation-time Gateway change. Inspect and validate
the live configuration, preserve every existing media entry, and obtain
separate approval before applying it or restarting/reloading any service.

## Activation and release gates

1. Build and test the Android companion without installing it.
2. With explicit approval, merge and validate the local Whisper configuration.
   Verify watch sessions resolve to the existing `main` agent and inherit its
   intended ChatGPT OAuth model, tools, skills, workspace, and privileges.
3. Generate a stock `--voice-node` setup QR and scan it in the companion.
4. Start one voice turn, approve the exact `read+talk+write` scope upgrade from
   another authorized OpenClaw surface, and confirm the rotated token persists.
5. Verify the dedicated session appears under `agent:main:dashboard:*` and
   the **Start new watch conversation** button creates a different session key
   while leaving the previous transcript intact.
6. Capture one known phrase and prove locally that whisper.cpp produced the
   transcript before the OAuth-backed model received the text-only turn.
7. Prove the bounded text response returns through phone and BLE to the watch.
8. Prove cancellation, lost network, reconnect, stale/duplicate events,
   event-sequence gaps, oversized replies, and empty replies fail closed without
   replay or an orphaned Gateway run.
9. Complete 30/60/120/300-second microphone/BLE/display runs without heap
   corruption, reboot, PA leakage, audio persistence, or content logging.
10. Revoke the device and prove subsequent health/voice attempts fail; then
    reprovision and repeat one successful turn.

APK installation, credential approval, Gateway configuration, service reload,
and firmware flashing are separate operational gates and are never implied by
a successful repository build.

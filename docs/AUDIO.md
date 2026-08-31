# Audio slice

Audio is controlled by `CONFIG_NIGHTGLASS_AUDIO_ENABLED`. When enabled,
`AudioService` owns the bounded I/O surface and uses the
Waveshare BSP’s shared I2C/I2S wiring:

| Signal | GPIO |
| --- | ---: |
| I2C SDA / SCL | 15 / 14 |
| I2S MCLK / BCLK / WS | 16 / 41 / 45 |
| I2S DOUT / DIN | 40 / 42 |
| amplifier enable | 46 |

The service configures 16-bit, 16 kHz, mono I2S, and uses the ES8311
output and ES7210 input paths. GPIO46 is a Nightglass-owned, active-high
physical PA gate: it is driven low and read back before boot continues, remains
low in light sleep through a sleep pull-down, and the codec is deliberately
configured with `pa_pin = -1` so its driver cannot take ownership of that pin.

Codec mute return values are not treated as proof of safety. The PA gate is
the output safety barrier. A fixed POD cue queue and sleeping owner task
are armed at boot; codec, I2S DMA, microphones, and the temporary PSRAM
streaming buffer are created only while a queued operation runs. Normal watch
operation therefore does not pay codec, DMA, amplifier, or microphone power
cost. Capture transfers 2,048 bytes; playback streams through the same bounded
2,048-byte buffer. The manual test transfers 64,000 bytes over two seconds;
notification cues are shorter, and embedded overrides are capped at five
seconds. LVGL and BLE paths only enqueue fixed commands; the owner task performs
all codec and I2S work under a no-light-sleep PM lock and
forces GPIO46 low before and after every operation. The service reports actual
transferred bytes; it does not claim that a codec register write or a completed
transfer proves physical audibility or microphone population.

One blocked owner task persists; it holds no codec, I2S, DMA, PA, or PM-lock
resource between cues. No unbounded queue or recording storage exists, and no
samples are transmitted. Diagnostics are published through the existing health
registry and `AudioSnapshot` counters. The catalog provides distinct generated
message, call, calendar, email, alarm, timer, success, warning, and manual-test
cues. Optional WAV files under `components/nightglass_services/assets/` replace
matching generated cues when they are PCM16, mono, 16 kHz, and at most five
seconds. Interfaces and I2S DMA channels are owned by Nightglass and released
after every operation. If PA-low readback or channel release cannot be proven,
audio locks until reboot rather than asserting a safe state.

## Hardware uncertainty

The maintained Waveshare 1.0.7 BSP and its example identify ES8311 output and
an ES7210 input codec. Nightglass follows the board's verified mono transport
and first populated ES7210 capture lane; it does not assert that multiple
physical microphones are present or independently routed. GPIO46 is also an ESP32-S3
strapping pin, so software can prove its low state only after early boot;
physical reset behavior remains an HIL requirement. The vendor BSP convenience
audio factories use hidden static ownership, so Nightglass creates the
equivalent ES8311/ES7210 interfaces directly and owns their lifetime. Audio
remains a bounded diagnostic feature rather than a background recorder or
voice assistant.

## User settings

The Sound & DND screen stores watch output volume, master mute, and Do Not
Disturb in the `ng_audio` NVS namespace. Volume is adjustable from 0-100% in
10% steps; codec gain remains capped at the hardware-proven -8 dB ceiling.
Master mute and 0% suppress every cue. Do Not Disturb suppresses phone
notification/call/email/calendar cues while leaving alarm, timer, and explicit
test playback available. Policy is checked when a cue is queued and again by
the owner task, so enabling mute or DND also suppresses stale queued cues.

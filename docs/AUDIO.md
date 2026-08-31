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

The service configures 16-bit, 16 kHz, two-channel I2S, and uses the ES8311
output and ES7210 input paths. GPIO46 is a Nightglass-owned, active-high
physical PA gate: it is driven low and read back before boot continues, remains
low in light sleep through a sleep pull-down, and the codec is deliberately
configured with `pa_pin = -1` so its driver cannot take ownership of that pin.

Codec mute return values are not treated as proof of safety. The PA gate is
the output safety barrier. Codec, I2S DMA, microphones, a temporary internal
DMA buffer, and a one-shot worker are created only after an explicit Audio-app
action; normal watch operation therefore does not pay codec, DMA, amplifier, or
microphone power cost. Each test transfers 2,048 bytes (within the public
4,096-byte maximum), runs off the LVGL task, uses a no-light-sleep PM lock, and
forces GPIO46 low before and after every operation. The service reports actual
transferred bytes; it does not claim that a codec register write or a completed
transfer proves physical audibility or microphone population.

No audio task persists after a test, no unbounded queue or recording storage
exists, and no samples are transmitted. Diagnostics are published through the
existing health registry and `AudioSnapshot` counters. The Audio app provides a
short 440 Hz speaker test and a bounded microphone RMS sample. Interfaces and
I2S DMA channels are owned by Nightglass and released after each diagnostic
operation. If PA-low readback or channel release cannot be proven, diagnostics
lock until reboot rather than asserting a safe state.

## Hardware uncertainty

The maintained Waveshare 1.0.7 BSP and its example identify ES8311 output and
an ES7210 input codec. The BSP source does not document two onboard digital
microphones, their channel/slot selection, or a microphone power/enable GPIO.
Therefore this change provides a two-channel I2S contract and uses the BSP
codec definitions directly, but does not assert that both physical microphones
are present or independently routed. The first hardware pass must verify the
exact board revision, I2C addresses, I2S slot order, amplifier behavior,
reset/sleep silence, and input channel mapping. GPIO46 is also an ESP32-S3
strapping pin, so software can prove its low state only after early boot;
physical reset behavior remains an HIL requirement. The vendor BSP convenience
audio factories use hidden static ownership, so Nightglass creates the
equivalent ES8311/ES7210 interfaces directly and owns their lifetime. Until
that pass, audio remains a bounded diagnostic feature rather than a background
recorder or voice assistant.

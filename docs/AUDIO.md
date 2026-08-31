# Audio slice

Audio is deliberately **disabled by default** (`CONFIG_NIGHTGLASS_AUDIO_ENABLED=n`).
When enabled, `AudioService` owns the bounded I/O surface and uses the
Waveshare BSP’s shared I2C/I2S wiring:

| Signal | GPIO |
| --- | ---: |
| I2C SDA / SCL | 15 / 14 |
| I2S MCLK / BCLK / WS | 16 / 41 / 45 |
| I2S DOUT / DIN | 40 / 42 |
| amplifier enable | 46 |

The service configures 16-bit, 16 kHz, two-channel I2S, opens the ES8311
output path, and starts output muted. Each capture/play call is capped at
4,096 bytes; no audio task, unbounded queue, recording storage, or automatic
playback is created. Diagnostics are published through the existing health
registry and `AudioSnapshot` counters.

## Hardware uncertainty

The maintained Waveshare 1.0.7 BSP and its example identify ES8311 output and
an ES7210 input codec. The BSP source does not document two onboard digital
microphones, their channel/slot selection, or a microphone power/enable GPIO.
Therefore this change provides a two-channel I2S contract and uses the BSP
input factory, but does not assert that both physical microphones are present
or independently routed. Enablement requires bench verification of the exact
board revision, I2C addresses, I2S slot order, amplifier behavior, and input
channel mapping. No flash or hardware test is part of this change.

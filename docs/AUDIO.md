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

The service configures 16-bit, 16 kHz, two-channel I2S, opens the ES8311
output and ES7210 input paths, and starts output muted. Codec/I2S interfaces
are initialized at boot when this option is enabled, but no samples are
transferred until an explicit Audio-app action. Each capture/play call is
capped at 4,096 bytes; no audio task, unbounded queue, recording storage, or
automatic playback is created. Diagnostics are published through the existing
health registry and `AudioSnapshot` counters. The Audio app provides a short
440 Hz speaker test and a bounded microphone RMS sample. The output path is
re-muted after every test tone; no recording is stored or transmitted.

## Hardware uncertainty

The maintained Waveshare 1.0.7 BSP and its example identify ES8311 output and
an ES7210 input codec. The BSP source does not document two onboard digital
microphones, their channel/slot selection, or a microphone power/enable GPIO.
Therefore this change provides a two-channel I2S contract and uses the BSP
input factory, but does not assert that both physical microphones are present
or independently routed. The first hardware pass must verify the exact board
revision, I2C addresses, I2S slot order, amplifier behavior, and input channel
mapping. Until that pass, audio remains a bounded diagnostic feature rather
than a background recorder or voice assistant.

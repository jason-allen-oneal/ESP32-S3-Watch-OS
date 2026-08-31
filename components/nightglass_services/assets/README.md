# Notification sound overrides

Drop optional WAV overrides in this directory using one of these bounded names:

`notification.wav`, `call.wav`, `calendar.wav`, `email.wav`, `alarm.wav`,
`timer.wav`, `success.wav`, or `warning.wav`.

Files must be RIFF/WAVE PCM format 1, mono, signed 16-bit little-endian,
16 kHz, and no longer than five seconds (160,000 data bytes). Invalid files
are rejected at runtime and the matching generated cue is used instead.

Embedded sound files become part of the application image. They are not
committed by default; verify licensing before distributing any sound asset.

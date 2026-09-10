# OpenClaw watch mark

Static monochrome adaptation of the OpenClaw mascot from `ui/public/favicon.svg` in https://github.com/openclaw/openclaw at commit `0229a108fee749327b2cc60c4d228299dfe2f889`. Original source SHA-256: `74e1fb7542675f68aafe502576f7c857c3f3620d07095e910515e3c05d62ab2e`. OpenClaw Foundation MIT license: `OPENCLAW-LICENSE.txt`.

Original body, claws, antennae, eye and pupil paths are retained. Animation and gradient are removed for a small single-color status icon. The SVG is a grayscale coverage mask (white is ink, black is transparent). The 54×54 A8 asset is embedded in firmware, centered in the existing 88×58 slot, and tinted by existing voice-health state. No runtime SVG decoder, scaling, or additional pixel buffer is used.

Regenerate with ImageMagick 7 from this directory:

```sh
magick -background black openclaw_mark_v1.svg -filter Lanczos -resize 54x54 -colorspace gray -depth 8 gray:openclaw_mark_v1.a8
```

A8 size: 2,916 bytes. SHA-256: `b7278908cfaed5bb73427df632d7a7303df9ab6262818d18dd677bb66385db88`.

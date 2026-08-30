# Nightglass Watch-Face Packs

Watch faces are versioned, declarative data packs. The system owns time,
battery, alarms, motion, input, and power behavior; a pack can only select
firmware-known resources and place native UI fields.

Nightglass Classic is pack ID `0` and the guaranteed fallback. Selection is
persisted by ID in NVS. An unknown ID, invalid schema, unsupported version, or
unknown resource resolves to Classic.

## Format 2

Each built-in `FacePack` declares:

- stable numeric ID, slug, name, and format version;
- one firmware-supported layout (`classic` or `full_background`);
- a complication capability bitmask and OLED palette;
- a background `FaceAsset` enum value;
- bounded `FaceTextSlot` records for fixed captions and live system fields;
- bounded `FaceActionSlot` records for firmware-known actions.

Text slots contain a field enum, safe-area rectangle, one of four compiled
font styles, alignment, and palette role. Fixed captions may contain text;
live fields may not. Every text rectangle must stay inside the 28-pixel safe
inset on the 410x502 panel. Actions and assets are enums, not callbacks,
scripts, module names, URLs, or filesystem paths. The validator limits packs
to 24 text slots and four action slots.

`full_background` currently allows only the compiled Revenant Grid v2 asset.
That allowlist is deliberately narrow. A future import transport must retain
the same validation, fixed decoded-size limits, checksum verification, and
Classic fallback before external data can reach this structure.

## Built-in packs

### Nightglass Classic

The legible system fallback with live time, date, battery, and motion state.
It does not depend on an image asset or pack metadata renderer.

### Revenant Grid v2

The approved full mechanical skull/chassis image is a 410x502 background with
empty instrument bays. Native LVGL overlays provide:

- battery percentage in the top arc and voltage/charge state in the power bay;
- a steps bay that explicitly reads `N/A / NO COUNT` because Nightglass has no
  validated step-counter implementation;
- current QMI8658 movement/calibration state in the activity bay (never a fake
  heart-rate value);
- large local time, RTC availability, day, and date;
- actual alarm and countdown state;
- a full-face, firmware-owned touch target that launches Apps.

All text remains native and is updated from current service snapshots. Missing
or stale inputs are shown as unavailable instead of being synthesized.

## Asset format and reproduction

The production background is RGB565 (`410 * 502 * 2 = 411,640` bytes) embedded
in the application image. LVGL reads it directly; firmware does not load or
decode the approximately 2 MB source PNG at runtime. The exact 410x502 PNG is
kept as a visual production artifact.

Run:

```sh
scripts/convert-revenant-grid-v2.sh
scripts/convert-revenant-grid-v2.sh --check
```

The converter pins the source SHA-256, applies fixed ImageMagick resize and PNG
settings, emits little-endian RGB565 deterministically, verifies dimensions
and byte count, and requires no network or credentials.

## License and provenance

`components/nightglass_ui/assets/source/revenant_grid_v2_concept.png` is
original imagery generated for Nightglass with built-in ImageGen. A
user-supplied image was used as a
composition/material reference during generation; it is not included in this
repository. The generated background was then reduced locally by the checked-in
deterministic conversion script.

No Facer artwork, watch-face package, code, font, or image asset is embedded.
The original generated image and its deterministic derivatives are Nightglass
project assets and are governed by the repository's distribution terms; there
is no bundled third-party art license.

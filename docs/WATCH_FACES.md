# Nightglass Watch-Face Packs

Watch faces are versioned, declarative data packs. A pack selects a supported
layout, palette, and complication set; it cannot execute code, create tasks, or
access hardware. The system owns time, battery, alarms, motion, input, and
power behavior.

The built-in registry is the first transport. A later asset-filesystem loader
will apply the same schema, resource limits, checksum validation, and fallback
rules to imported packs.

## Format 1

Each pack declares a stable ID and slug, display name, format version,
firmware-supported layout, OLED palette, and optional owned image assets with
fixed decoded-size limits. Unknown or invalid packs fall back to Nightglass
Classic. The selected pack ID is stored in NVS.

## Built-in packs

### Nightglass Classic

The legible system fallback with live time, date, battery, and motion state.

### Revenant Grid

An original cyber-gothic instrument face with stacked time, toxic-green
accents, top date/battery rail, alarm/motion/timer complications, and an owned
biomechanical skull ornament. All dynamic text remains native LVGL.

Visual research references:

- palette direction: https://www.facer.io/watchface/hNGd5xGkI8
- information hierarchy: https://www.facer.io/watchface/i8E0kZi4s5
- gothic focal motif: https://www.facer.io/watchface/qITpGrH8Kb

No third-party art is shipped. The Revenant skull and layout are original
Nightglass assets, generated for this project and reduced to RGB565A8 for the
embedded renderer.

# Revenant quiet shell — design assets

Created for the owner's September 5, 2026 request for cleaner backgrounds on the watch's other screens.

**Design preview only. No firmware source, configuration, signing, installed device, or selected theme was changed.** The original home-face artwork stays untouched. This is not a device screenshot or a claim of on-watch acceptance.

## Files

- `revenant-shell-source.png`: original built-in ImageGen output, copied without altering it.
- `revenant-shell-410x502.png`: downscaled opaque PNG at native watch resolution.
- `revenant-shell-410x502.rgb565`: little-endian RGB565; 410 × 502; stride 820; exactly 411,640 bytes. Asset preparation only, not embedded into firmware.
- `preview.html`: local interactive comparison of Apps, Timer, and Quick Settings; open this file in a browser. Uses the sibling PNG and the project's existing home PNG. Requires the repository-relative assets to remain in place.
- `screen-preview.png`: browser-rendered gallery of proposed screens; sample values and approximate browser typography, not LVGL screenshots.
- `PROMPT.txt`: exact prompt used with the built-in ImageGen tool, not the fallback CLI.

## Design

A thin blackened gunmetal perimeter, sparse dim acid-green corner lights, and an almost-black open center. Text, controls, icons, values, and status remain native UI objects, never baked into the image. Opaque #080B09 cards preserve reading contrast. The mechanical skull remains exclusive to Home.

Preview geometry follows current shell code: 410 × 502, 28 px safe inset, header back target 56 × 56, standard menu controls 338 × 64. The quiet art is shown at 64/255 opacity, within the current chrome schema's maximum; the current art comparison uses 36/255, and no background on Quick Settings. All displayed values are examples. Browser font metrics and rendering differ from Montserrat/LVGL. No watch functionality is implemented in the preview; only its background-comparison controls are interactive.

## Future integration scope

Use a separate allowlisted chrome-only asset and keep the Home/AOD asset and behavior unchanged. Route background insertion needs to cover Quick Settings and premium early-return paths as well as normal app routes and alert overlays. Keep resource bounds, static flash storage, opaque cards, navigation, and wake behavior intact. Do not load/decode a full PNG into runtime memory. The raw asset adds about 402 KiB of flash before metadata, if integrated later. Retain the existing 1–64 chrome opacity bound and home-asset validation.

The default preview uses the existing acid-green theme, but the watch supports custom accents; the frame's baked corner color should not override dynamic semantic or accent colors.

## Provenance and preparation

Style reference: `components/nightglass_ui/assets/source/revenant_grid_v2_concept.png` in this checkout. No external art was imported. Generated output preserved at:
`~/.openclaw/agents/main/agent/codex-home/generated_images/01a07440-28c2-7300-9490-fbe8741159f6/exec-66651f3d-99ed-40e5-827d-e286af861740.png`.

Downscaling followed the installed project's ImageMagick workflow: sRGB, Lanczos, 410x502, stripped PNG metadata. Raw output uses the same R5/G6/B5 bit packing and little-endian order as `scripts/convert-revenant-grid-v2.sh`.

Review: independent source/UI audit completed before art production; final preview review and focused browser checks are recorded in `VERIFICATION.md`. No broad build/test campaigns or device access are part of this design task.

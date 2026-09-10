# Design verification — 2026-09-05

Scope: local artwork and code-based design preview only. No firmware build, flash, signing, device connection, behavior test, or on-wrist acceptance.

## Completed by the primary agent

- Visually inspected the original generated background and desktop/mobile screen galleries.
- Verified the native PNG is 410 × 502, opaque sRGB RGB.
- Verified little-endian RGB565 output is exactly 411,640 bytes (410 × 502 × 2).
- Browser viewport 1400 × 820: all three watch stages were exactly 410 × 502, all background images loaded, no horizontal page overflow.
- Switched to Current background: all existing 410 × 502 images loaded; Apps and Timer were 36/255 opacity and Quick Settings was 0. A first immediate image read occurred before loading completed; a subsequent completed-load read confirmed all three assets. No missing resource persisted.
- Switched back to Quiet frame: all three new images loaded, active mode state updated correctly.
- Browser viewport 390 × 844: all three stages scaled to approximately 358 × 438, all images loaded, no horizontal page overflow. The complete vertically stacked gallery and footer were captured and visually inspected.
- Saved `screen-preview.png`, `current-comparison.png`, and `mobile-preview.png`.

Browser review indicates no visible label or control clipping in these three sample screens. Typography is approximate browser Arial, not native Montserrat; visual matching is not pixel-exact and cannot establish hardware usability.

## Independent review and limits

A read-only specialist audited the current native implementation before art production and confirmed the safe inset, native dimensions, palette overrides, opacity bound, raw image format, existing background reuse, and Quick Settings/premium early-return gaps. These constraints informed this design.

Independent visual sign-off was unavailable: the same reviewer's image and HTML tool reads failed with a native hook relay timeout; a second transcript-only reviewer reported that images were omitted from the fork. Neither reviewer claimed to have seen or approved the images. The visible screenshot inspections above were performed by the primary agent. This does not block delivery as an explicitly labeled design concept; no firmware or device acceptance is claimed.

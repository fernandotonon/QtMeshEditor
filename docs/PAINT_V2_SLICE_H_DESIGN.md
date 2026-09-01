# Paint v2 Slice H — Brush presets & colour palettes (#551)

Two quality-of-life features every serious paint app has: saving a whole brush
configuration for one-click reuse, and curated colour swatches.

Parent epic: #543.

> **Tablet pressure/tilt is deliberately NOT in this slice.** The issue bundles
> it as a third feature, but the project is desktop-only today, so pressure
> curves and tilt-driven stamp rotation could be written and unit-tested yet
> never verified against real pen hardware. Shipping an unverifiable headline
> feature is worse than deferring it; it should return as its own issue when
> there is a tablet to test on.

## Brush presets

A preset is a full snapshot of the brush: tool, footprint, stamp/tiling asset,
radius / strength / falloff, edge shape, channel, the stamp dynamics
(spacing / scatter / size + opacity jitter / rotation), and the colour source
(solid vs gradient, plus the ramp name).

**15 bundled presets:** Soft Round, Hard Round, Pencil Sketch, Spray Paint,
Foliage Cluster, Edge Wear, Scratched Metal, Wet Brush, Smudge Soft, Smudge
Hard, Stencil Hard, Stencil Soft, Eraser Soft, Eraser Hard, Cavity Dirt.

Bundled presets are defined **in C++**, not shipped as data files, so they
cannot go missing from an install — the same choice `GradientRamp::bundledPresets`
already makes. Custom presets serialize to `<AppData>/paint/presets/*.json`.

### Two things that are easy to get wrong

**Applying a preset does not touch the paint colour.** A preset describes the
brush — its shape and dynamics — not what you are painting *with*. Restoring a
saved colour on every preset click would silently discard the user's current
choice, so colour is excluded from both capture and apply.

**Apply order: the stamp/tiling asset is set before the footprint type.** The
other order leaves a stamp brush briefly pointing at the previous preset's
image.

Deleting a bundled preset is refused rather than performed: they are compiled
in, so a "delete" could only remove a user override and the entry would appear
to come back on restart. The UI disables the button to make that visible.

## Colour palettes

**6 bundled palettes** (CC0 / factual colour values, no third-party asset
files): Material Design, Pantone Classics, Skin Tones, Foliage Greens, Sky
Blues, Earth Tones. Custom palettes live in `<AppData>/paint/palettes/*.json`.

- **5-column swatch grid.** Left-click sets the foreground, right-click the
  background — matching the existing FG/BG toolbar swatch's semantics.
- **Recent colours** (last 12, newest first). Re-picking a colour *promotes* it
  rather than appending a duplicate, so the ring stays 12 distinct colours.
  Only the **foreground** feeds it: the background is a secondary slot changed
  rarely, and mixing it in would churn the history. It is session-only —
  persisting it would restore a stale palette unrelated to the next task.
- **Palette from texture** extracts representative colours from the active
  paint buffer by coarse colour-cube quantisation (5 bits/channel), averaging
  the real pixels in each bucket so every swatch is a colour that actually
  occurs. Counting exact RGB values would return ten imperceptibly different
  shades of the same colour. Fully transparent pixels are skipped, or a
  mostly-empty texture reports "transparent black" as its dominant colour.

`Swatch` carries **no alpha** on purpose: a palette curates hues, and paint
alpha is a separate brush property. Baking alpha into swatches would override
the user's setting on every pick.

## Files

| File | Role |
|---|---|
| `src/BrushPresetLibrary.{h,cpp}` | preset data, bundled table, JSON, import/export (pure data) |
| `src/ColorPaletteLibrary.{h,cpp}` | palette data, bundled table, JSON, recent ring, extraction (pure data) |
| `src/TexturePaintController.{h,cpp}` | the apply/capture layer + `paletteChanged` |
| `src/mainwindow.cpp` | the brush-portal UI (Qt Widgets, beside the existing paint controls) |

Breadcrumbs: `paint.preset.apply` / `.save` / `.delete` / `.import`,
`paint.palette.apply` / `.save`.

## UI toolkit note

These controls are **Qt Widgets**, not QML, which departs from CLAUDE.md's
"new UI should be QML" guidance. They live inside the existing brush portal
alongside the radius/strength/footprint controls, which are all Widgets; adding
QML there would split one panel across two toolkits for no user benefit. The
same reasoning applies to `StampLibraryDialog`, whose grid this follows.

The issue asks for a preset **thumbnail grid with search and tags**. Presets are
parameter sets with no image to display, so a grid of identical tiles would
carry no more information than the name — a dropdown is used instead. Search and
tags are omitted as unjustified at 15 entries; they become worth adding once a
user library grows large enough to need them.

## Known limits

- The stamp/tiling asset a preset names must exist. A bundled preset naming a
  missing stamp would apply silently and leave the previous footprint, so a test
  resolves every bundled reference against `BrushAssetLibrary`.
- Preset enum fields are stored as ints holding the controller enum values, so
  the controller enums must not be renumbered without a format bump.
- Palette extraction reads the paint buffer, so it needs an active paint
  session; there is no "extract from an arbitrary file" path.

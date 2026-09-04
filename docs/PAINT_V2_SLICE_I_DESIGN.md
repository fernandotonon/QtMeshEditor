# Paint v2 — Slice I: Bake-up workflow (#552)

Bakes painted layers down to the deliverables a game engine consumes: a flat PBR
texture set plus engine-specific channel packs.

Parent epic: #543. Depends on Slice C (#546, layer stack) and Slice D (#547, PBR
channels), both shipped.

## What ships

- **Layer stack → flat PBR set.** Composite the visible layers of each painted
  channel and write one texture per channel at a chosen resolution.
- **Engine packs.** Unity / Unreal / Godot / glTF channel layouts (below).
- **Vertex layer → texture layer.** Rasterise a vertex-colour bake into UV space
  with seam dilation and add it to the stack as a real layer.
- **Sidecar JSON** describing inputs → outputs + engine target, for
  reproducibility.
- **Surfaces.** A QML bake dialog, `qtmesh paint-bake`, and an MCP tool.

## Two deliberate deviations from the issue text

**1. Six channels, not seven — Height is intentionally absent.**

The issue lists Height among the seven outputs. Slice D (#547) deliberately
removed Height as a paintable channel: `paintChannels()` skips it and
`setActiveChannel(Height)` redirects to Normal, because a separate Height
channel produced a second normal-map bake fighting the Normal one, and no
parallax/displacement shader exists that would consume a standalone height
texture. Height therefore has **no buffer of its own** — it shares the Normal
session.

So there is no Height data to bake. Emitting a file anyway would duplicate the
normal map's grayscale source under a name that nothing reads, which is worse
than omitting it: it looks like a deliverable and is not. We bake the six
channels that have real data (BaseColor, Normal, Roughness, Metallic, AO,
Emissive). If a displacement consumer ever lands, Height becomes a real channel
again and this bake picks it up with no structural change.

**2. `BakeTargets` is pure data; the packing is not "reuse `TextureChannelPacker`
verbatim".**

`TextureChannelPacker::ChannelSource` and `NormalMapGenerator::GenSpec` take a
**file path**, not an in-memory image. A freshly composited channel lives in
memory, so reusing them as-is would mean writing every channel to a temp PNG and
reading it straight back — which `bakeChannel` already does today and which is
grubby. Slice I instead does its channel arithmetic on `QImage` directly (the
same Rec.601 luminance rule `TextureChannelPacker` uses, kept identical so the
two agree) and leaves the path-based packer untouched for its existing CLI/GUI
callers. Extending `ChannelSource` with an optional `QImage` is a reasonable
follow-up but is not required here and would touch unrelated call sites.

## Engine channel layouts

| Target | Layout |
|---|---|
| **Generic** | One texture per channel, as painted. Normal = OpenGL +Y up. |
| **Unity** | `_MetallicSmoothness` RGBA (R = metallic, A = smoothness = 1 − roughness); AO separate; normal converted to **DirectX +Y down**. |
| **Unreal** | `_ORM` RGB (R = occlusion, G = roughness, B = metallic); normal OpenGL. |
| **Godot** | Every channel separate, plus a `.tres` sidecar carrying the sRGB/linear flag per texture. |
| **glTF** | glTF 2.0 metallic-roughness: `_metallicRoughness` with G = roughness, B = metallic; occlusion may share the R lane. |

Smoothness-vs-roughness and +Y-up-vs-down are the two conversions that silently
produce wrong-looking materials if got backwards, so both are unit-tested
against hand-computed values rather than golden images.

## Structure

- **`src/PaintBakeTargets.{h,cpp}`** — pure data, no Ogre, no Qt widgets. Takes
  a `QImage` per channel plus a target + resolution; returns the named output
  images and the sidecar JSON. This is where every engine rule lives, so the
  rules are testable without a scene, a GPU, or a paint session.
- **`TexturePaintController`** — collects the per-channel composites and calls
  the above. It must replicate `bakeChannel`'s live-vs-stashed branch: the
  **active** channel's stack lives in `m_layerStack`, and its `m_channelSessions`
  copy is deliberately stale until the next switch. Reading the map blindly
  would silently bake the pre-edit state of whatever channel is open.
- **`qtmesh paint-bake` / MCP `paint_bake`** — headless entry points.

## Notes

- "Include hidden layers" cannot be expressed through `compositeTo`, which
  hardcodes `solo ? i == solo : visible`. The bake snapshots the stack, forces
  visibility, composites, then restores — rather than adding an option to the
  live compositing path, which is on the paint hot path.
- Seam dilation reuses `VertexColorBaker::dilate`, already a public static and
  already reused by `MultiViewTextureBaker`.
- Breadcrumbs: `paint.bake.*`, carrying the engine target.

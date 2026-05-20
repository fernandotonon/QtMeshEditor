# Godot VAT Demo Project

Three scenes showcasing QtMeshEditor's OpenVAT bake. Built on Godot 4.2+.

| Scene | Purpose |
|---|---|
| `scenes/demo_web.tscn` | Single Rumba dancer + orbit camera. The one we embed in the website. |
| `scenes/demo_perf_vat.tscn` | 1000 instances driven by VAT. FPS readout in the corner. |
| `scenes/demo_perf_skeleton.tscn` | 1000 instances driven by Godot's `SkinnedMeshRenderer` + `AnimationPlayer`. Same FPS readout. |

The two perf scenes are visually identical and use the same source mesh
(`assets/Rumba/source.gltf`). Comparing FPS between them shows how much
the VAT path saves over per-instance GPU skinning.

## Web demo

```bash
godot --path tools/godot-vat-demo scenes/demo_web.tscn
```

Controls:

- **Drag** — orbit
- **Wheel** or **+/-** — zoom

Export for web (HTML5/WebAssembly):

1. Open the project in the Godot editor.
2. Project → Export → Add HTML5 preset (install the Web export
   template via Editor → Manage Export Templates if needed).
3. Set "Variant: thread" off (single-threaded is the safer default
   for the broadest browser compatibility).
4. Export with the .html target — Godot writes a self-contained
   bundle of `.html` + `.js` + `.wasm` + `.pck`.

The web build is ~30 MB compressed; the dancer + bake is most of it.

## Perf comparison

Both perf scenes spawn 1000 dancers in a grid with desynchronised
starting frames so the GPU can't optimise away identical work. Run
them back-to-back and watch the FPS overlay:

```bash
godot --path tools/godot-vat-demo scenes/demo_perf_skeleton.tscn  # heavier
godot --path tools/godot-vat-demo scenes/demo_perf_vat.tscn       # lighter
```

What to look for:

- **`Min (1s window)`** is the most honest metric. Average FPS hides
  hitches; the rolling minimum surfaces them.
- The skeletal path costs scale with **per-instance bone matrix
  uploads**. The Rumba rig has ~50 bones, so ~50,000 matrices per
  frame at 1000 instances. The VAT path is a single texture sample
  per vertex per frame — no per-instance CPU work.
- Both scenes leave shadows off so the difference is in the skinning
  step, not the shadow pass.

Typical numbers on an Apple M-series laptop:

| Path | FPS (1000 instances) |
|---|---|
| Skeleton | 45–60 |
| VAT | 100+ (often hits the refresh cap) |

YMMV by GPU. On Steam Deck the gap is larger; on a desktop RTX both
should comfortably hit refresh-rate.

## How VAT works (one paragraph)

A Vertex Animation Texture encodes one frame of vertex data per row
in a PNG: column = vertex index, channels = (x, y, z) normalized to
the model's bounding box. The shader samples the texture once per
vertex per frame and reconstructs the world position. No skeleton,
no bone matrices, no CPU animation tick — the GPU does all the work
in the vertex stage. The cost is one texture sample per vertex, vs.
a 4-bone skinning blend in the traditional path.

QtMeshEditor's `qtmesh vat` writes the OpenVAT format
([sharpen3d/openvat](https://github.com/sharpen3d/openvat)). Sample
shaders for Godot / Unity / Unreal live at `tools/vat-shaders/`.

# Godot VAT Demo Project

Scenes showcasing QtMeshEditor's OpenVAT bake and its bone texture (BTA)
bake (#1166), against Godot's stock skeletal path. Built on Godot 4.2+
(re-exported with 4.7).

| Scene | Purpose |
|---|---|
| `scenes/demo_web.tscn` | Single Rumba dancer + orbit camera. The one we embed in the website. |
| `scenes/demo_perf_vat.tscn` | 1000 instances driven by VAT. FPS readout in the corner. |
| `scenes/demo_perf_skeleton.tscn` | 1000 instances driven by Godot's `SkinnedMeshRenderer` + `AnimationPlayer`. Same FPS readout. |
| `scenes/demo_perf_bta.tscn` | 1000 instances driven by a bone texture (`assets/RumbaBones`, `qtmesh vat --mode bone-texture --target godot`): one `MultiMeshInstance3D`, GPU skinning from per-frame bone matrices. Same FPS readout. |

Every perf scene takes the instance count from `?n=` (web) or `-- --n=N`
(native), clamped to 1..10000, and pulls the camera back so the whole crowd
is in view. The overlay also shows draw calls and the GPU texture / buffer
memory Godot reports for the whole scene.

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

`./bench.sh` runs every path at 100 / 400 / 1000 / 5000 instances (2 s
warm-up, 8 s measured with wall-clock timestamps, vsync off, 1280x720, the
camera pulled back until the whole crowd is in view, path order rotated per
count) and writes one JSON line per run to `bench_results.jsonl`. Measured
2026-11-18 on an Apple M5 (Godot 4.7.2, Forward+ / Metal), same Rumba
dancer (5828 vertices, 70 bones, 72-frame clip), one sample per cell:

| Instances | Skeletal | VAT | Bone texture |
|---:|---:|---:|---:|
| 100  | 245 fps (4.1 ms), 1111 draws | 529 fps (1.9 ms), 22 draws | 563 fps (1.8 ms), 22 draws |
| 400  | 61 fps (16.4 ms), 4411 draws | 385 fps (2.6 ms) | 384 fps (2.6 ms) |
| 1000 | 22 fps (46.2 ms), 11011 draws | 206 fps (4.9 ms) | 188 fps (5.3 ms) |
| 5000 | 3.4 fps (290 ms), 55011 draws | 51.8 fps (19.3 ms) | 42.4 fps (23.6 ms) |

GPU memory as Godot reports it for the whole scene (all textures, all
buffers):

| Instances | Skeletal (tex / buffers) | VAT | Bone texture |
|---:|---:|---:|---:|
| 100  | 65.5 / 27.3 MB | 69.8 / 15.9 MB | 61.8 / 15.9 MB |
| 1000 | 65.5 / 131.6 MB | 69.8 / 16.0 MB | 61.8 / 16.0 MB |
| 5000 | 65.5 / 594.6 MB | 69.8 / 16.2 MB | 61.8 / 16.2 MB |

What the animation data itself costs: the VAT texture is 5828 x 142
texels (positions + normals, one column per vertex), the bone texture
210 x 72 RGBA float = 0.24 MB (rgba16f would halve it). Both instanced
paths add only the MultiMesh instance buffer per instance (~64 bytes),
while the skeletal path grows by ~115 KB of buffers per instance plus a
draw call per surface.

Reading it: on this GPU the two baked paths are close up to ~1000
instances, and VAT pulls ahead at 5000 (19.3 vs 23.6 ms), where the bone
texture's heavier vertex shader (up to 24 texel fetches per vertex vs
VAT's 4) dominates. The bone texture's advantage is size and flexibility,
not speed: its texture grows with bones x frames instead of vertices x
frames, many clips fit in one texture, and each instance can pick and
crossfade clips. Both baked paths are an order of magnitude ahead of
per-instance skeletal skinning past a few hundred instances. One sample
per cell; differences of a few percent are noise. These are one machine's numbers: measure on your target hardware.

Both perf scenes spawn 1000 dancers in a grid with desynchronised
starting frames so the GPU can't optimise away identical work. Run
them back-to-back and watch the FPS overlay:

```bash
godot --path tools/godot-vat-demo scenes/demo_perf_skeleton.tscn  # heavier
godot --path tools/godot-vat-demo scenes/demo_perf_vat.tscn       # lighter
godot --path tools/godot-vat-demo scenes/demo_perf_bta.tscn -- --n=5000   # any count
```

What to look for:

- **`Min (1s window)`** is the most honest metric. Average FPS hides
  hitches; the rolling minimum surfaces them.
- The skeletal path costs scale with **per-instance bone matrix
  uploads**. The Rumba rig has 70 bones, so ~70,000 matrices per
  frame at 1000 instances. The VAT path is a texture sample per vertex
  per frame; the bone texture path fetches each vertex's 4 bone
  matrices (3 texels each, twice when interpolating between frames).
  Neither does per-instance CPU work.
- All scenes leave shadows off so the difference is in the skinning
  step, not the shadow pass.


## How the bone texture works (one paragraph)

The bake stores every bone's skinning matrix (animated pose x inverse
bind) for every frame: 3 RGBA float texels per bone per row, clips stacked
by row. The mesh stays in bind pose and keeps 4 bone indices + weights per
vertex (`BoneTextureMesh.gd` copies them from the imported skin into
`CUSTOM0`/`CUSTOM1`, matching joints to the texture's bones by name), and
`bone_texture.gdshader` blends the 4 matrices per vertex — ordinary linear
blend skinning, just reading the pose from a texture. Each instance picks
its clip, time offset, speed and crossfade through `INSTANCE_CUSTOM`. The
texture grows with bones x frames, not vertices x frames, so it is the
better fit for detailed characters with many clips; VAT stays the choice
for non-skeletal deformation (cloth, sims, morphs). Re-bake with:

```bash
qtmesh vat "media/models/Rumba Dancing.fbx" --mode bone-texture --target godot \
  --basename Rumba -o tools/godot-vat-demo/assets/RumbaBones
```
(then delete the copied `Boss_*.png` and `.material` sidecar: the `.glb`
embeds its textures). The BTA asset faces +Z like every current export; the
VAT/skeletal assets predate the importer change that stopped mirroring Z
(#977) and face -Z, which is why their spawners turn the dancers by 180
degrees and `PerfSpawnerBTA.gd` does not.

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

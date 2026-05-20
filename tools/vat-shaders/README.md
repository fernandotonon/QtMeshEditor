# VAT Shaders — drop-in templates for OpenVAT bakes

These are minimal, production-ready shaders for replaying a VAT baked by
`qtmesh vat` (or any other [sharpen3d/openvat](https://github.com/sharpen3d/openvat)-compatible
exporter). Drop them into your engine project, point them at the bake's
texture + sidecar, and you have shader-driven animation playback with
no skeleton.

| Engine | File | Status |
|---|---|---|
| Godot 4 (Forward+) | [`openvat.gdshader`](./openvat.gdshader) | Tested |
| Unity 2022+ (BiRP) | [`openvat.shader`](./openvat.shader) | Tested |
| Unreal Engine 5 | [`openvat.usf`](./openvat.usf) | HLSL snippet — paste into a Custom node |

URP and HDRP variants for Unity: the vertex math is identical. Swap
the `LightMode` tag and the `#include`s for the pipeline you target —
see the comments at the top of `openvat.shader`.

## What the bake looks like

`qtmesh vat` writes two files per bake:

```text
<basename>_pos.png             16-bit RGB PNG, height = 2 × Frames
                               Top half:    vertex positions, normalized to [Min..Max]
                               Bottom half: per-vertex normals, encoded as (n+1)/2
<basename>-remap_info.json     OpenVAT sidecar, schema:
                               { "os-remap": { "Min": [...], "Max": [...], "Frames": <int> } }
```

The shaders need three things from the sidecar: `Min`, `Max`, `Frames`.
Read them in your engine's preferred scripting language and forward
them to the shader as uniforms.

## The UV2 requirement

VAT shaders address per-vertex texture columns via a **second UV
channel** on the mesh (UV2 in Godot/Unity, UV1 in Unreal — Unreal
0-indexes). Each vertex's UV2 holds `(col + 0.5) / width, 1.0 -
(last_pos_row + 0.5) / tex_height` — its column and the V coordinate of
the last position row in its block. Adding `frame * (1 / tex_height)`
to UV2.y walks up through the bake's frame strip.

There are three ways to get UV2 on your mesh:

1. **Author it in the DCC tool** before exporting. Blender's OpenVAT
   add-on does this automatically (the `VAT_UV` channel).
2. **Convert with `assimp export in.fbx out.gltf -fgltf2`** — preserves
   `TEXCOORD_1` through to glTF, which Godot/Unity import as UV2.
3. **Synthesize at load time** from the bake's known width + frame
   count. The Godot harness at
   `tools/godot-vat-test/scripts/VATPlayer.gd:_ensure_uv2_on_mesh`
   shows the math. The Unity shader file has a C# port at the bottom.
   For Unreal, do this via a Python editor script (snippet in
   `openvat.usf`).

> **`qtmesh convert` strips UV2 today.** Tracked as a follow-up: the
> Ogre→glTF exporter should preserve a second UV channel. Until then,
> if you re-process a bake with `qtmesh convert`, you'll need to
> synthesize UV2 in-engine OR run the source FBX through assimp
> directly.

## Texture import settings

Your engine's texture importer **must** be set to load the bake as
data, not as a color texture:

| Setting | Required value |
|---|---|
| sRGB / Color Texture | OFF |
| Filter | Nearest / Point |
| Compression | None / Uncompressed |
| Wrap | Clamp |
| Mipmaps | OFF |

Wrong settings = gamma correction smearing every position by ~5%, or
texel-blending across vertex columns producing garbage between frames.

In Godot, load via `Image.load_from_file()` + `ImageTexture.create_from_image()`
to avoid the import pipeline entirely.

In Unity, click the imported PNG and set the values manually, or
write an `AssetPostprocessor` to apply them automatically (the harness
shows this — see `tools/unity-vat-test/Assets/VAT/Scripts/VATPlayer.cs`'s
`ApplyDataTextureSettings`).

In Unreal, set `Compression = TC_HDR`, `sRGB = OFF`, `Texture Group =
ColorLookupTable`, `Filter = Nearest`.

## Sidecar `Min` / `Max` are strings

OpenVAT writes them as quoted strings with 8 decimal places — e.g.
`"-0.69999999"`. Parse them through your locale-invariant float parser
(`float.Parse(s, CultureInfo.InvariantCulture)` in C#, `float(s)` in
GDScript and Python). The strings are deliberate: it makes the JSON
diffable across exporters that round differently.

## Normal-flip gotcha

QtMeshEditor's FBX import path applies `aiProcess_ConvertToLeftHanded`,
which flips winding without flipping the captured normal vector. The
shader templates include a `NORMAL = -normalize(n)` (or equivalent) on
read to compensate. **If your bake came from a different source** (e.g.
Blender via the OpenVAT add-on) and you see inverted lighting, remove
the negation:

```cpp
// Godot:    NORMAL = normalize(n);   // not -normalize
// Unity:    OUT.worldNrm = normalize(mul((float3x3)unity_ObjectToWorld, n));   // drop the -
// Unreal:   result.Normal = normalize(n);                                      // drop the -
```

## Where this came from

These templates are extracted verbatim from the test harnesses under
`tools/godot-vat-test/` and `tools/unity-vat-test/`. The harnesses
include working side-by-side scene setups, asset staging scripts, and
keyboard controls for verification. Use them as a reference if the
templates don't behave as expected.

## License

MIT. Use freely in commercial or non-commercial work.

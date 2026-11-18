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
| Godot 4 — **rigid-body** bakes (`--mode rigid`) | [`openvat_rigid.gdshader`](./openvat_rigid.gdshader) | Tested against the rigid fixture |

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

## Normal-flip gotcha (historical)

Templates used to negate the decoded normal (`NORMAL = -normalize(n)`) to
compensate for the exporter of the time, which left the import-side
`aiProcess_ConvertToLeftHanded` winding flip un-inverted in the exported
glTF. The exporter now restores source space (and the bake data follows
the same export space), so the templates read the normal **as-is**:

```cpp
// Godot:    NORMAL = normalize(n);
// Unity:    OUT.worldNrm = normalize(mul((float3x3)unity_ObjectToWorld, n));
// Unreal:   result.Normal = normalize(n);
```

If you pair an OLD bake (made before this change) with these templates and
see inverted lighting, re-bake — or re-add the negation locally.

## Bake modes (#522)

`qtmesh vat --mode …` picks the sampler; the texture and sidecar shape
stay OpenVAT and the sidecar gains `_mode` (plus mode-specific keys) so
a consumer can dispatch on it.

| Mode | Source | Texture columns | Extra sidecar keys | Template |
|---|---|---|---|---|
| `skeletal` (default) | a skeletal clip, post-skinning positions | vertices | — | `openvat.gdshader` / `.shader` / `.usf` |
| `mesh-anim` | a full-mesh vertex clip (Alembic cache, `VAT_POSE` stream) | vertices | `_track` (clip id) | same as skeletal |
| `morph` | a morph-weight clip resolved to positions (default clip `MorphAnim`) | vertices | `_morph_targets`, `_track` | same as skeletal |
| `rigid` | any clip, ONE rotation + pivot per **chunk** (= submesh) per frame | **chunks** | `_rigid.{chunk_count, chunks[], max_residual}` | `openvat_rigid.gdshader` |

Skeletal, mesh-anim and morph bakes are interchangeable for the shaders
above — the per-vertex layout is identical. `_bit_depth` is `8`, `16`
or `32` (`--encoding rgba8 | rgba16 | exr`); an 8-bit PNG decodes with
the same `bounds_min + texel * (bounds_max - bounds_min)` formula.

### Rigid-body layout

```text
width  = chunk count
height = 2 × Frames
rows [0 .. Frames)         pivot position of chunk c this frame (RGB, normalized Min..Max;
                           raw floats for EXR)
rows [Frames .. 2×Frames)  rotation quaternion (x, y, z, w) encoded (q + 1) / 2 in RGBA
_rigid.chunks[c].pivot     the chunk's BIND-pose pivot (its centroid)
```

Runtime per vertex of chunk `c`:

```glsl
vec3 pivot_frame = decode(texel(c, frame).rgb);
vec4 q           = texel(c, Frames + frame) * 2.0 - 1.0;   // normalize after a frame lerp
p' = rotate(q, p - pivot_bind) + pivot_frame;
n' = rotate(q, n);
```

Chunk `c` is glTF primitive / Godot surface / Unity sub-mesh `c` of
the bake's `source.gltf` (Ogre submesh order). Set the shader's chunk
index per surface material, or bake with `--emit-uv2` — on a rigid bake
UV2.x carries the vertex's chunk column. Adjacent frames are kept in
the same quaternion hemisphere, so `normalize(mix(q0, q1, t))` is a
valid short-arc blend.

`_rigid.max_residual` is the worst `|fit − actual|` over every vertex
and frame in source units. Near zero means the pieces really move
rigidly (a one-bone-per-piece rig, an RBD cache); a large value means
a chunk deforms and you want `--mode skeletal` instead.

**Axis conventions per target.** The bake is Y-up right-handed. The
Godot template applies no swizzle (Godot is Y-up RH). For **Unity**
(Y-up left-handed) negate X on the pivot AND negate the quaternion's
`x` and `w` components (equivalently mirror the rotation); for
**Unreal** (Z-up left-handed, centimetres) swap Y/Z on the pivot and
on `q.xyz`, negate `q.w`, and scale positions by 100. The per-vertex
Unity/Unreal templates already carry the same swizzles for positions
and normals — mirror them for the quaternion when you port the rigid
shader.

## Bone texture animation (#1166)

`qtmesh vat <rigged file> --mode bone-texture -o <dir>` (MCP `bake_vat
{mode: "bone-texture"}`, Inspector VAT mode "Bone texture (GPU skinning)") is
the skeletal alternative to the per-vertex VAT. Instead of every vertex
position per frame it stores every bone's **skinning matrix** per frame
(animated global x inverse bind), so the texture grows with
`bones x frames` instead of `vertices x frames`, and the vertex shader skins
the bind-pose mesh itself.

| File | Contents |
|---|---|
| `<name>_bones.exr` | RGBA float (`rgba32f`, default) or half (`rgba16f`). Width = bones x 3, height = total frames of all clips. |
| `<name>_bones.json` | `qtmesh-bone-texture-v1`: bones (name, parent), clips (`firstRow`, `frameCount`, `duration`, effective `fps`, `loop`), layout and conventions. |
| `<name>.glb` | The rigged mesh in bind pose (skip with `--no-mesh`). |
| `bone_texture.gdshader`, `BoneTextureMesh.gd` | Godot template (with `--target godot` / `--include-shaders godot`). |

**Layout.** Texel `(bone*3 + r, row)` is row `r` of the bone's 3x4 matrix
(`m[r][0..3]`); the 4th row is `(0,0,0,1)`. Clips are stacked by row, each
sampling both ends (row `firstRow` = t 0, row `firstRow+frameCount-1` = clip
end), so interpolating two adjacent rows never reads the next clip. Looping
wraps from the last row to the first; that is seamless only when the clip's
last pose matches its first (true for authored loops). Row 0 is the first scanline. Skinned position:
`p' = sum_i w_i * M_i * vec4(p, 1)` (4 influences, renormalised).

**Space.** Mesh-local, Ogre / glTF convention (Y up, right-handed, source
units). The scene-node transform is not baked. For another convention apply
`C * M * C^-1` on read.

**Bone indices.** The texture uses the source skeleton's bone order (the
sidecar's `bones[]`). An engine's imported skin numbers its joints in its own
order, so map each joint to the texture **by name**. `BoneTextureMesh.gd`
does this for Godot (names are normalised on both sides, since Godot
sanitises `:` and friends on import), keeps the strongest 4 influences and
writes them to `CUSTOM0` (indices) / `CUSTOM1` (weights) as RGBA float, which
a spatial shader can read; it also drops the engine skin so the mesh can go
in a `MultiMesh`.

**Per instance** (`INSTANCE_CUSTOM`): `r` clip index, `g` time offset (s),
`b` speed (0 = 1), `a` crossfade (integer part = second clip, fractional
part = its weight). One shared `time_seconds` uniform is the only per-frame
CPU work.

**Import.** The EXR must stay a float texture: Godot *Compress Mode = VRAM
Uncompressed*, mipmaps off, filter nearest (the shader uses `texelFetch`).
Unity: `RGBAFloat`/`RGBAHalf`, no compression, point filter, no mips.

**Accuracy.** With `rgba32f`, on baked frames the texture reproduces the
source skinning to float precision for vertices with up to 4 influences
(QtMeshEditor parity test against Ogre's software skinning: 2.4e-7).
Vertices with more influences lose the weakest ones (the sidecar reports how
many: `skinning.verticesOverLimit`), and `rgba16f` adds half-float rounding
(about 1e-3 relative; values past +/-65504 are refused at bake time). Between
frames (and in a crossfade) the shader blends matrices component-wise, which
slightly shrinks large per-frame rotations (1.2 mm at 20 fps on a fast test
rig); bake at a higher fps if that matters. In the Godot demo the shader's
silhouette matches Godot's own skeletal skinning of the same `.glb` to within
the 1-pixel antialiased edge.

**Godot template limits.** The shader's clip table holds 32 clips (the bake
warns past that); `BoneTextureMesh.gd` has no `class_name` (each bake ships a
copy, two copies would collide) — `preload` it by path.

Only a Godot template ships today; Unity/Unreal users can port
`bone_texture.gdshader` directly (three `texelFetch`es per influence, the
matrix assembly is spelled out in it).

## Where this came from

These templates are extracted verbatim from the test harnesses under
`tools/godot-vat-test/` and `tools/unity-vat-test/`. The harnesses
include working side-by-side scene setups, asset staging scripts, and
keyboard controls for verification. Use them as a reference if the
templates don't behave as expected.

## License

MIT. Use freely in commercial or non-commercial work.

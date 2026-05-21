# QtMeshEditor — OpenVAT demo for Unreal Engine

The Rumba dancer baked by `qtmesh vat` running inside Unreal via the
shipped `openvat.usf` Custom-node shader. This folder is the Unreal
counterpart to `tools/godot-vat-demo/`.

**What ships in the repo:**

```
tools/unreal-vat-demo/
├── QtMeshVAT.uproject           ← open this in UE 5.3+
├── README.md                    ← you are here
└── Content/
    ├── Rumba/                   ← bake artifacts (data files only)
    │   ├── source.gltf  + .bin  ← mesh + per-vertex column index in TEXCOORD_1
    │   ├── Boss_diffuse.png     ← diffuse texture
    │   ├── mixamo.com_pos.png   ← 16-bit position+normal texture
    │   ├── mixamo.com-remap_info.json
    │   ├── mixamo.com_ogre_bind.bin  ← legacy fallback, not used here
    │   └── openvat.usf          ← reference shader body
    └── Python/
        └── build_vat_demo.py    ← runs once to create Material + BP
```

**Why a Python bootstrap instead of pre-built `.uasset`s:**
Unreal's `.uasset` is a proprietary binary that re-cooks per engine
version and can't be hand-written or committed reliably across UE 5.3 /
5.4 / 5.5. The bake itself (PNG, JSON sidecar, glTF, USF shader) is all
text/standard formats — we ship those and let the script build the
engine-specific glue (texture import settings, Material, demo
Blueprint) on first open.

## How the demo works

`qtmesh vat --emit-uv2` writes the per-vertex bake-column index as a
`TEXCOORD_1` attribute directly into `source.gltf`. Unreal's mesh
importer reorders vertices for cache locality, but a vertex attribute
travels with its vertex through any reorder — so the imported mesh's
`TexCoord[1]` still points at the right column in the position
texture. The Material's Custom HLSL node reads `TexCoord[1]`,
fetches `pos_tex.Load(int3(col, row, 0))`, and writes the result to
**World Position Offset**. The skeleton is unused at runtime.

This is the same trick the Godot demo uses; it took landing the
`--emit-uv2` flag (#654) to make it work cleanly in Unreal too — no
runtime UV2-baking, no bind-sidecar matcher, no engine-version-
specific Geometry Script paths.

## One-time setup

1. **Install UE 5.3 or newer** (5.4 and 5.5 tested locally). The
   project enables the `PythonScriptPlugin` and
   `EditorScriptingUtilities` plugins, both shipped with stock UE;
   no marketplace plugins required.

2. **Open `QtMeshVAT.uproject`.** Unreal will scan/import the content
   folder. The project is content-only — no C++ compile.

3. **Run the bootstrap script.** In the editor: **Window → Output
   Log → switch the Cmd dropdown to `Python` →**
   ```python
   py Content/Python/build_vat_demo.py
   ```
   Look for `=== Bootstrap done. ===` in the log.

   This creates:
   - `/Game/Rumba/T_OpenVAT_Pos` (position texture, **non-sRGB,
     no DXT, no mips, Nearest filter** — required, the script sets all four)
   - `/Game/Rumba/T_Boss_Diffuse` (diffuse texture)
   - `/Game/Rumba/SK_Rumba` (skeletal mesh from `source.gltf`, imported
     via Unreal's built-in **Interchange** framework)
   - `/Game/VATDemo/M_OpenVAT` (Custom-node material — drives
     `current_frame = Time × fps` internally, so the animation
     loops on its own without a Blueprint Tick)
   - `OpenVAT_Dancer` — a `SkeletalMeshActor` spawned at the world
     origin in your current level, with `SK_Rumba` + `M_OpenVAT`
     applied and Animation Mode set to None

   **You should now see the dancer animating in the editor viewport.**
   No Play required — Unreal's material `Time` node ticks in the editor
   too. If you don't, scrub the editor camera around the origin.

   **If the script logs `InterchangeManager not available` or the
   .gltf import fails:** drag `Content/Rumba/source.gltf` into the
   Content Browser manually, rename the resulting Skeletal Mesh to
   `SK_Rumba`, then re-run the script — the texture/material/spawn
   steps will pick up the already-imported mesh.

   **If the script logs `source.gltf is MISSING TEXCOORD_1`:** the
   bake folder predates `qtmesh vat --emit-uv2` (added in PR #654).
   Re-bake your source FBX with:
   ```bash
   qtmesh vat path/to/source.fbx --anim <name> --emit-uv2 \
              -o tools/unreal-vat-demo/Content/Rumba/
   ```
   then re-run the bootstrap.

## Performance notes

Like the Godot demo, this scales to a 1000-instance crowd via
**HierarchicalInstancedStaticMeshComponent** plus per-instance custom
data carrying the per-instance frame-phase offset. The `openvat.usf`
shader reads `PerInstanceCustomData` instead of a single
`current_frame` parameter in that variant. The 1000-instance MultiMesh
equivalent is in `tools/godot-vat-demo/scripts/PerfSpawnerVAT.gd`;
an Unreal port would mirror that pattern.

## Limitations of this demo

- **Bind-pose offset.** The Custom node returns absolute model-space
  position and writes it straight to WPO, which assumes the bind pose
  sits at the actor origin. Mixamo characters do, so this works for
  the demo; an asset with a non-origin bind pose would need a
  `WPO = custom_output - bind_position` correction.
- **No skeleton at runtime.** Set Animation Mode = None on the
  SkeletalMeshComponent. The mesh is reduced to a static vertex
  buffer driven by the texture.

## How it relates to the rest of the project

- Same bake artifacts as the [Godot demo](../godot-vat-demo/).
- Shader template ships in [`tools/vat-shaders/openvat.usf`](../vat-shaders/openvat.usf).
- `qtmesh vat --include-shaders unreal -o <dir>` drops `openvat.usf`
  next to a bake, alongside the JSON sidecar + position texture.
- `qtmesh vat --emit-uv2 -o <dir>` writes the per-vertex column index
  as `TEXCOORD_1` directly into `source.gltf` — what makes this demo
  a true "open and play" setup without any post-import vertex
  remapping.
- The live web demo at [qtmesheditor.com/#vat-demo](https://qtmesheditor.com/#vat-demo)
  uses the Godot variant; no Unreal-web equivalent exists (Unreal's
  HTML5/web export was deprecated).

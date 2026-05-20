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
    ├── Rumba/                   ← bake artifacts (text + binary data files)
    │   ├── source.gltf  + .bin  ← vertex-order-aligned mesh
    │   ├── Boss_diffuse.png     ← diffuse texture
    │   ├── mixamo.com_pos.png   ← 16-bit position+normal texture
    │   ├── mixamo.com-remap_info.json
    │   ├── mixamo.com_ogre_bind.bin
    │   └── openvat.usf          ← Custom-node body reference
    └── Python/
        └── build_vat_demo.py    ← runs once to create Material + BP
```

**Why a script instead of pre-built `.uasset`s:**
Unreal's `.uasset` is a proprietary binary that re-cooks per engine
version and can't be hand-written or committed reliably across UE 5.3 /
5.4 / 5.5. The bake itself (PNG, JSON sidecar, glTF, bind `.bin`,
USF shader) is all text/standard formats — we ship those and let the
script build the engine-specific glue (texture import settings,
Material, demo Blueprint) on first open.

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
   - `/Game/VATDemo/M_OpenVAT` (the Custom-node material — does
     World Position Offset against the position texture)
   - `/Game/VATDemo/BP_VATDancer` (an actor skeleton — needs 4-node
     tick wiring per step 5 below)

   **If the script logs `InterchangeManager not available` or the
   .gltf import fails:** drag `Content/Rumba/source.gltf` into the
   Content Browser manually, rename the resulting Skeletal Mesh to
   `SK_Rumba`, then re-run the script — the texture/material steps
   will pick up the already-imported mesh.

4. **Bake UV2 per vertex.** The position texture is column-addressed
   by Ogre vertex index, but Unreal's mesh importer reorders the
   vertex buffer for cache locality. We have to write each Unreal
   vertex's bake-column index into UV2 (TexCoord 1) so the shader
   can `Load()` the right texel.

   In **UE 5.4+ with the Geometry Script plugin**, run this in the
   Python console:
   ```python
   import Content.Python.build_vat_demo as bd
   bd.bake_uv2("/Game/Rumba/SK_Rumba")
   ```
   (the script logs the plan and the per-vertex matching that the
   Geometry Script plugin commits to the mesh's UV2 channel).

   In **UE 5.3** (no Geometry Script on skeletal meshes from Python),
   use the equivalent C++ helper documented at the bottom of
   `build_vat_demo.py`, or — easiest — re-import with a slightly
   modified `source.gltf` that already carries the UV2 channel
   (a future revision of `qtmesh vat --include-shaders unreal` will
   write that variant directly).

5. **Wire `BP_VATDancer`'s tick.** Open `BP_VATDancer`, add:
   - A `SkeletalMeshComponent` referencing `/Game/Rumba/SK_Rumba`.
   - Set its material override 0 to a **MaterialInstanceDynamic** of
     `/Game/VATDemo/M_OpenVAT` (create-on-Construction-Script).
   - On Event Tick:
     ```
     time = GameTimeInSeconds
     fps = 30   (literal, or expose as a variable)
     frame_count = (read from the material's frame_count param)
     current_frame = fmod(time * fps, frame_count)
     SkeletalMesh → Set Scalar Parameter Value on Materials
         (parameter_name = "current_frame", value = current_frame)
     ```
   - Disable engine animation on the SkeletalMeshComponent
     (Animation Mode = None) — the VAT shader replaces all per-vertex
     motion; Unreal's skinning would fight it.

6. **Drop `BP_VATDancer` into a level** and hit Play. The dancer should
   loop the Rumba clip without any per-frame CPU animation tick.

## Performance notes

Like the Godot demo, this scales to a 1000-instance crowd via
**HierarchicalInstancedStaticMeshComponent** plus per-instance custom
data carrying the per-instance frame-phase offset. The `openvat.usf`
shader reads `PerInstanceCustomData` instead of a single `current_frame`
parameter in that variant. The 1000-instance MultiMesh equivalent is
in `tools/godot-vat-demo/scripts/PerfSpawnerVAT.gd`; an Unreal port
would mirror that pattern.

## Limitations of this demo

- **UV2 bake step is engine-version-dependent.** Unreal's per-version
  changes to mesh-editing Python APIs mean the script can't ship a
  one-shot solution for every version. The Geometry Script path
  (5.4+) is the cleanest; 5.3 needs the C++ helper.
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
- The live web demo at [qtmeshcloud.dev/#vat-demo](https://qtmesheditor.com/#vat-demo)
  uses the Godot variant; no Unreal-web equivalent exists (Unreal's
  HTML5/web export was deprecated).

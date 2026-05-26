# QtMeshEditor — OpenVAT demo for Unity

The Rumba dancer baked by `qtmesh vat` running inside Unity via the
shipped `openvat.shader` Built-in Render Pipeline shader. This is the
Unity counterpart to `tools/godot-vat-demo/` and `tools/unreal-vat-demo/`.

## ⚠️ Recommended path: the official OpenVAT Unity package

The author of the OpenVAT format ships a maintained Unity package at
[`sharpen3d/openvat-unity`](https://github.com/sharpen3d/openvat-unity).
It uses **Shader Graph decoders**, which require URP or HDRP — not
Built-in Render Pipeline. If you're starting a fresh project, that
is the recommended path:

1. Create a new Unity 2022.1+ project with the **URP** template.
2. Open Window → Package Manager → `+` → "Add package from git URL"
   and paste `https://github.com/sharpen3d/openvat-unity.git`.
3. Copy this folder's bake (`Assets/VAT/Rumba/`) into your project's
   `Assets/OpenVATContent/`, renaming `mixamo.com_pos.png` →
   `mixamo.com_vat.png` (OpenVATEditor looks for `*_vat.png`).
4. **Tools → OpenVAT Editor** → set folder path → "Process OpenVAT
   Content". The editor reads the sidecar JSON, builds a Shader Graph
   material, and spawns the dancer prefab.

The bake's sidecar format (`os-remap.Min/Max/Frames`) is compatible
with what OpenVATEditor expects — no schema translation needed.

## Status: WebGL build animates but vertex→column mapping is still off

After a deep debugging session through three render pipelines (BiRP,
URP/Metal, URP/WebGL2) we landed on a WebGL build where:
- ✅ The Rumba mesh imports correctly via QtmGltfImporter — 5828
  verts across 11 submeshes with stable vertex order.
- ✅ Per-vertex column index encoded into vertex Color32 (R = low
  byte, G = high byte) preserves all 5828 unique values losslessly
  end-to-end (CPU and GPU).
- ✅ `LOAD_TEXTURE2D_LOD` with integer texel coordinates samples the
  position texture per-vertex correctly (confirmed by a diagnostic
  showing a static bind-pose dancer with per-vertex pastel colors).
- ✅ Animation drives `_frame` from OpenVATDriver per Update.
- ❌ With VAT replay engaged, the dancer is an animated chaotic
  confetti egg — vertices ARE sampling per-vertex, but each vertex
  samples a column DIFFERENT from its own (the vertex→column
  mapping is scrambled somewhere in the OpenVATEditor pipeline).

Suspected root cause: OpenVATEditor's `Instantiate(LoadAssetAtPath<
GameObject>(modelPath))` produces a runtime mesh whose vertex order
doesn't match the column ordering that QtmGltfImporter baked into
the Color32 channel. The `c[N] = N` invariant verified at runtime
suggests the encoding is correct from the importer's perspective —
but Unity's runtime mesh may renumber when the prefab is
instantiated or when the renderer batches submeshes.

### Suggested next steps (out of scope for this in-session work)
- Write a `MeshDataArray`-based mesh-cooker that builds the
  per-vertex column buffer FROM the post-import mesh's vertex order
  rather than the importer's, eliminating the renumber risk.
- OR: ship a runtime `StructuredBuffer<int>` of column indices,
  indexed by `SV_VertexID` directly inside the shader instead of
  relying on attributes.

After a long debugging session we hit the same wall in both render
pipelines: **`SAMPLE_TEXTURE2D_LOD` in the vertex shader does NOT
vary per-vertex on Unity 6 + Metal on Apple Silicon.** Every vertex
appears to sample the SAME texel, regardless of how varied the input
texture coordinates are.

### Confirmed working end-to-end
- The custom QtmGltfImporter parses the bake's glTF correctly and
  preserves vertex order across 11 submeshes (5828 verts × 11 prims).
- UV2 reaches the runtime mesh.uv2 array with 5828 unique X values
  in range [0..5827], one per vertex.
- Color32 encoding (R = col low byte, G = col high byte) also
  preserves all 5828 unique unpacked column values losslessly.
- All material parameters set correctly: `_UseTime=1`,
  `_UsePackedNormals=1`, `_exaggeration=1`, `_speed=1`, `_frames=71`,
  `_resolutionY=142`, plus the bounds from the sidecar.
- The OpenVATDriver MonoBehaviour ticks `_frame` per Update and the
  console confirms it's incrementing.
- All 11 submeshes get the VAT material assigned via
  `FixupMultiSubmeshPrefab`.
- The custom HLSL shader (`Assets/Shaders/OpenVAT_URP.shader`) is
  bound and compiles cleanly (the upstream OpenVAT shadergraph leaves
  VertexDescription.Position unwired — confirmed by tracing the
  graph JSON's edge list).

### The remaining failure mode
A diagnostic shader (in `OpenVAT_URP.shader`) painted by the sampled
texture's RG channels showed a SMOOTH pastel gradient across the
whole egg-shaped mesh — i.e. every fragment sees the same sampled
value as its triangle's neighbours, interpolated bilinearly. If
texture sampling varied per-vertex, we'd see 5828 distinct colors
scattered chaotically across the surface; instead we see a single
smooth gradient with NO per-vertex variation.

### What this means
Unity 6's Metal backend treats the position texture as a fragment-
stage resource and doesn't bind it as a vertex-stage texture, even
when we use `SAMPLE_TEXTURE2D_LOD(...,0)` explicitly. This is
documented as a known limitation in some Unity 6 Metal threads but
isn't well-publicised.

### Next steps (out of scope for this demo)
- Move position decode to a compute shader that pre-bakes each frame's
  positions into a runtime-allocated `MeshDataArray`. The vertex
  shader then reads from the CPU-uploaded vertex buffer — no GPU-side
  texture fetch needed.
- OR: write a custom `MaterialPropertyBlock`-based per-instance
  position array, indexed by `SV_VertexID` from a `StructuredBuffer`.
- OR: switch the demo's target to a render pipeline + platform combo
  where vertex texture fetch is reliable (BiRP on DirectX, URP on
  DirectX, or HDRP). The bake itself is platform-agnostic.

### Working artifacts to keep
- The bake under `Assets/OpenVATContent/` is correct and reusable.
- The OpenVAT package integration (Packages/manifest.json) is wired.
- The CLISetupURPAndBuild editor script automates the whole import +
  scene build + standalone build pipeline.
- The mesh import, material setup, and animation tick infrastructure
  are all working — only the GPU texture sampling is the blocker.

## Status: BiRP custom shader path is incomplete

This folder also ships a hand-rolled BiRP `Hidden/QTM/VAT` shader for
projects that can't or won't use URP. **It currently produces an
egg-shaped blob instead of the dancer** — the cause has been narrowed
down to vertex-stage texture sampling returning constant values under
Metal on Apple Silicon despite:
- UV2 confirmed per-vertex correct in the Mesh asset (verified by a
  bind-pose-with-uv2-gradient diagnostic that paints a smooth red→blue
  gradient across the dancer)
- Position texture confirmed at 5828×142, R16G16B16A16_UNorm at runtime
- Pixel values confirmed in [0,1] when sampled from CPU
- Tested with both `_PosTex.Load(int3)` (texelFetch) and `tex2Dlod()`
  (textureLod) paths at `#pragma target 3.0+` — both return (0,0,0)

This is most likely a Unity 6 + Metal + BiRP-shader interaction that
warrants targeted research. The URP path (above) is closer to working
and is the recommended starting point.

**What ships in the repo:**

```text
tools/unity-vat-demo/
├── README.md                         ← you are here
├── ProjectSettings/
│   └── ProjectVersion.txt            ← targets Unity 2022.3 LTS
├── Packages/manifest.json
└── Assets/
    ├── Shaders/
    │   └── openvat.shader            ← the shader (also at tools/vat-shaders/openvat.shader)
    ├── Scripts/
    │   ├── VATPlayer.cs              ← MonoBehaviour: binds material, drives _CurrentFrame
    │   ├── OrbitCamera.cs            ← mouse-drag orbit + wheel zoom
    │   ├── FPSOverlay.cs             ← avg + min(1s) FPS readout
    │   ├── PerfSpawnerVAT.cs         ← 1000-instance grid (VAT path)
    │   ├── PerfSpawnerSkeleton.cs    ← 1000-instance grid (skeletal path)
    │   └── Editor/
    │       └── BootstrapVAT.cs       ← auto-wires bake from sidecar on Add Component
    ├── Scenes/                       ← (empty — you build these in 30 seconds, see below)
    └── VAT/
        └── Rumba/                    ← bake artifacts (data files only)
            ├── source.gltf + .bin    ← mesh, consumed by the custom QtmGltfImporter (vertex-order-preserving)
            ├── Boss_diffuse.png      ← diffuse texture
            ├── mixamo.com_pos.png    ← packed positions + normals
            ├── mixamo.com-remap_info.json
            └── mixamo.com_ogre_bind.bin  ← legacy fallback, not used here
```

**Why no pre-built `.unity` scenes:**
Unity's `.unity` scene format references every asset by GUID — those GUIDs
live in `.meta` files that the editor generates on first import. Hand-
authored scenes referencing GUIDs from another machine break the moment
you re-import. The setup below takes < 60 seconds and produces a scene
that's identical to what we'd ship anyway.

## One-time setup

1. **Install Unity 2022.3 LTS** via the Unity Hub. Newer 6000.x versions
   should also work — the demo uses only the Built-in Render Pipeline
   and stock Unity API, no URP/HDRP/Shader Graph.

2. **Open the project in the Unity Hub:**
   - Hub → Add → "Add project from disk" → select `tools/unity-vat-demo`.
   - Open it. First import takes a minute (Unity builds its asset
     database).

3. **Texture import settings — critical for the position texture.**
   In the Project window, click `Assets/VAT/Rumba/mixamo.com_pos.png`
   and set in the inspector:
   - **Texture Type:** Default
   - **sRGB (Color Texture):** OFF — positions are linear data, not color
   - **Compression:** None
   - **Filter Mode:** Point (no filter)
   - **Wrap Mode:** Clamp
   - **Read/Write Enabled:** OFF (saves memory; the GPU reads it)

   Click Apply.

4. **Mesh import — handled by a custom ScriptedImporter.**
   The bake's `source.gltf` is imported by `QtmGltfImporter.cs` (in
   `Assets/Scripts/Editor/`), a minimal vertex-order-preserving glTF
   reader specifically written for VAT replay. It reads positions,
   normals, UV0, and TEXCOORD_1 (the per-vertex bake-column index
   written by `qtmesh vat --emit-uv2`) and emits a Unity Mesh sub-
   asset without welding, deduping, or reordering verts — preserving
   the exact column alignment the position texture needs.

   > **Why a custom importer instead of Unity's stock importers?**
   > Unity 6's FBX importer uses Autodesk's strict FBX SDK and rejects
   > `qtmesh`'s 7300 binary output as corrupt. The OBJ importer reorders
   > vertices into face-walk order during the rebuild into a unified
   > vertex buffer, which breaks the VAT column indexing. Unity also
   > has no built-in glTF support. A 350-line ScriptedImporter is the
   > cleanest path — no Package Manager add, no plugin dependency, and
   > we control exactly what happens to the vertex buffer.

## Build the web/single-dancer scene (30 seconds)

1. **File → New Scene → Basic (Built-in)** → save as `Assets/Scenes/Web.unity`.

2. **Camera setup:**
   - Select the `Main Camera` → Add Component → **Orbit Camera**.
   - In the Orbit Camera inspector, set `target = (0, 1, 0)` and
     `distance = 4.5`.

3. **Spawn the dancer:**
   - GameObject → 3D Object → Cube (placeholder; we'll override its mesh).
   - Add Component → **VAT Player**.
   - `BootstrapVAT` auto-wires the bake fields from the sidecar JSON.
     Verify in the Inspector that `Source Mesh`, `Position Texture`,
     `Diffuse Texture`, `Frame Count = 71`, and the bounds look right.
   - (If auto-wire didn't run — e.g. you renamed the bake folder —
     manually drag the assets into the four slots and copy `Frames`,
     `Min`, `Max` from `mixamo.com-remap_info.json` into the matching
     fields.)
   - **Slot still empty?** Click the **"Auto-Wire from Bake"** button
     at the bottom of the VAT Player inspector. The button (re)reads
     the sidecar JSON and walks the FBX's sub-assets directly — bypasses
     Unity's Mesh picker, which by default hides sub-assets nested
     inside a Model importer (the FBX's actual Mesh lives as a sub-asset,
     not a top-level Mesh asset). The auto-wire-on-add hook does the
     same thing automatically, but only fires the moment you add the
     component — if Unity was still importing the FBX at that moment
     (common on first project open), the auto-fire misses and the
     button is your follow-up.
   - **If you want to pick manually anyway:** expand the `▶ source.fbx`
     row in the Project window — you'll see the Mesh sub-asset there
     and can drag it directly onto the slot.

4. **Light:**
   - GameObject → Light → Directional Light. The default values are fine.

5. **Press Play.** The dancer plays the Rumba animation, driven entirely
   by the position texture in the vertex shader. No skeleton, no
   Animator, no per-instance CPU work.

## Build the perf comparison scenes

### `Assets/Scenes/PerfVAT.unity`

1. New Scene → save as `PerfVAT.unity`.
2. Camera → Add Component → **Orbit Camera**, target = `(0, 1, 25)`,
   distance = 35, pitchDeg = -25.
3. Create an empty GameObject "Spawner" → Add Component → **Perf
   Spawner VAT**. Drag the bake assets into the inspector slots
   (Source Mesh from `source.gltf`'s prefab, the `_pos.png` and
   `Boss_diffuse.png` textures, and confirm `frameCount = 71`).
4. Create another empty GameObject "UI" → Add Component → **FPS Overlay**.
5. Directional Light as before; no shadows for the apples-to-apples
   comparison (toggle `Shadow Type = No Shadows` on the light).
6. Press Play.

### `Assets/Scenes/PerfSkeleton.unity`

1. New Scene → save as `PerfSkeleton.unity`.
2. Same camera + FPS Overlay setup as PerfVAT.
3. Spawner GameObject → Add Component → **Perf Spawner Skeleton**.
4. Drag `Assets/VAT/Rumba/source.gltf` into the `Source Prefab` slot.
   Unity will animate via the imported Animator on the prefab.
5. Press Play. Note the FPS readout vs PerfVAT.

## Web build (WebGL)

Unity's WebGL build target ships the demo as a static `index.html` +
`.wasm` + `.data` bundle.

1. File → Build Settings → switch platform to **WebGL** (re-import
   takes a few minutes the first time).
2. Player Settings → Resolution and Presentation → uncheck "Run In
   Background" so the dancer pauses cleanly on tab-switch.
3. Player Settings → Other Settings → **Compression Format: Brotli**
   (or Gzip if your static host doesn't support Brotli's `.br` files).
4. Build → pick an output folder, e.g. `tools/unity-vat-demo/Builds/web`.

The output ships as ~25 MB compressed. The Godot demo ships the
website's live perf comparison; this Unity port is mostly useful for
the perf scenes and as a stock-Unity API reference.

## How VAT works (one paragraph)

A Vertex Animation Texture encodes one frame of vertex data per row in a
PNG: column = vertex index, channels = (x, y, z) normalised to the
model's bounding box. The shader samples the texture once per vertex
per frame and reconstructs the world position. No skeleton, no bone
matrices, no CPU animation tick — the GPU does all the work in the
vertex stage.

QtMeshEditor's `qtmesh vat` writes the OpenVAT format
([sharpen3d/openvat](https://github.com/sharpen3d/openvat)). The
matching shaders for Godot / Unity / Unreal live at
`tools/vat-shaders/`.

## Re-baking the demo

```bash
# From the repo root, with the qtmesh CLI installed.
# --emit-uv2 is REQUIRED — it writes the per-vertex bake-column index
# into the glTF as TEXCOORD_1. Without it, multi-submesh meshes (like
# Mixamo's) won't replay correctly because Unity's importer can't
# reconstruct the original column order from face-walk alone.
qtmesh vat path/to/your/source.fbx --anim Rumba --emit-uv2 \
           -o tools/unity-vat-demo/Assets/VAT/Rumba/
```

After re-baking, switch back to Unity (or restart the editor). The
custom `QtmGltfImporter` will re-run on the new files, and the
`VATAssetPostprocessor` will re-apply the texture settings.
Then click the VATPlayer's **"Auto-Wire from Bake"** button to pick
up the new frame count + bounds from the updated sidecar.

## How it relates to the rest of the project

- Same bake artifacts as the [Godot demo](../godot-vat-demo/) and
  [Unreal demo](../unreal-vat-demo/).
- Shader at [`tools/vat-shaders/openvat.shader`](../vat-shaders/openvat.shader).
- `qtmesh vat --include-shaders unity -o <dir>` drops `openvat.shader`
  next to a bake.
- The live web demo at [qtmesheditor.com/#vat-demo](https://qtmesheditor.com/#vat-demo)
  uses the Godot variant; this Unity demo is a stock-Unity reference
  port — useful when integrating the bake into an existing Unity
  pipeline.

## Limitations

- **Bind-pose offset.** `VATPlayer` writes the absolute model-space
  position straight to clip-space without a bind-pose subtract. The
  Mixamo dancer's bind pose is at origin so this works; an asset with
  a non-origin bind pose would need a `pos - bindPos` correction in the
  shader (see `tools/vat-shaders/openvat.usf` for the Unreal version's
  bind-local subtraction).
- **Built-in Render Pipeline only.** The shader is a stock BiRP
  `Hidden/QTM/VAT`. For URP or HDRP, swap `UnityCG.cginc /
  AutoLight.cginc / Lighting.cginc` for URP's `Core.hlsl` — the vertex
  math is identical, only the lighting passes differ. The shader's
  header comment has the migration notes.
- **No batching across instances.** Each VATPlayer creates its own
  `Material` instance so `_CurrentFrame` can be set per-instance with a
  per-instance phase offset (the whole point of the perf comparison).
  A production setup using one phase per spawner group could
  `MaterialPropertyBlock` them instead and lean on SRP Batcher.

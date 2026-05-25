# QtMeshEditor — OpenVAT demo for Unity

The Rumba dancer baked by `qtmesh vat` running inside Unity via the
shipped `openvat.shader` Built-in Render Pipeline shader. This is the
Unity counterpart to `tools/godot-vat-demo/` and `tools/unreal-vat-demo/`.

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
            ├── source.fbx            ← mesh (Unity's stock importer handles FBX)
            ├── source.gltf + .bin    ← same mesh in glTF (used by the Godot + Unreal demos)
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

4. **Mesh import setting — Read/Write must be ON.**
   Click `Assets/VAT/Rumba/source.fbx` and on the Model tab:
   - **Read/Write Enabled:** ON — `VATPlayer.EnsureUV2()` writes a
     synthesized UV2 channel into the mesh at runtime, which needs
     CPU-side access.

   Click Apply.

   > **Why FBX, not glTF?** Unity 6's stock importer only handles
   > `.fbx`, `.obj`, `.dae`, and `.3ds`. `.gltf` needs the optional
   > [UnityGLTF](https://github.com/KhronosGroup/UnityGLTF) package.
   > The bake ships both formats and the demo prefers the FBX — same
   > 5828 verts in the same order (via `qtmesh convert`'s Ogre
   > intermediate), so the position-texture columns line up either
   > way. The Godot and Unreal demos use the glTF.

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
   - **Mesh picker hint:** clicking the dot ⊙ next to `Source Mesh`
     shows an empty picker because Unity only lists *top-level* Mesh
     assets, and the glTF's actual Mesh is a *sub-asset* of the .gltf
     importer. Either type `t:Mesh` in the picker search box to surface
     nested meshes, or expand the `▶ source.gltf` row in the Project
     window and drag the Mesh sub-asset directly onto the slot.

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
# From the repo root, with the qtmesh CLI installed:
qtmesh vat path/to/your/source.fbx --anim Rumba \
           -o tools/unity-vat-demo/Assets/VAT/Rumba/
```

After re-baking, restart the Unity editor so it re-imports the new
files, then re-set the texture import options on `mixamo.com_pos.png`
(Compression = None, Filter = Point — Unity's auto-import resets these).

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

# Unity VAT Test Harness

Mirrors the Godot harness — side-by-side comparison of QtMeshEditor VAT
playback vs the original skinned animation, but in Unity 2022 LTS+.

## What you need

- Unity Hub (already installed)
- Unity 2022.3 LTS or Unity 6 (any subversion). Open Unity Hub → Install → pick one.
- A built QtMeshEditor (`build_local/bin/QtMeshEditor.app/...`)
  OR `qtmesh` on `$PATH`.

## 60-second quickstart

```bash
# 1. Stage a bake
./tools/unity-vat-test/bake_and_stage.sh \
    "media/models/Rumba Dancing.fbx" "mixamo.com"

# 2. Open this project in Unity Hub:
#    Add → Add project from disk → select tools/unity-vat-test/

# 3. After Unity finishes its first-time import, in the menu bar:
#    QtMeshEditor → VAT → Build Test Scene

# 4. Press Play.
```

You'll see two characters side-by-side. Left is Unity's standard
`SkinnedMeshRenderer` driven by the imported `Animator`. Right is a
plain `MeshRenderer` driven by `VATPlayer.cs` sampling the baked
position texture in the shader — **no skeleton bound**.

If they dance the same way, the VAT bake is faithful.

## How it works

- `VATPlayer.cs` reads the sidecar JSON, applies texture importer
  settings (sRGB off, point filter, no compression, clamp), creates a
  `Material` per submesh with its own `_VertexOffset` uniform, and
  drives `_CurrentFrame` from `Update()`.
- `VAT.shader` (Built-in Render Pipeline) samples
  `(VertexOffset + SV_VertexID, _CurrentFrame)` and normalizes the
  result from `_BoundsMin` / `_BoundsMax`.
- `VATTestSetup.cs` is an Editor menu that builds the comparison
  scene from any staged bake under `Assets/VAT/Bakes/*/`.

## Multi-submesh meshes

The bake's position texture is one continuous strip — columns 0…N
where N = total vertex count across all submeshes. Unity's
`SV_VertexID` restarts at 0 for each submesh. We assign a separate
material per submesh with `_VertexOffset` set to that submesh's
starting column, so the right column is always sampled.

## URP / HDRP

The shader is BiRP-only as-shipped. For URP:

1. Change `LightMode` tag from `ForwardBase` to `UniversalForward`.
2. Replace `UnityCG.cginc` / `AutoLight.cginc` / `Lighting.cginc`
   with URP's `Core.hlsl` and rewrite the lighting fragment.
3. Or build the same logic in Shader Graph — `Custom UV` ↔
   `SV_VertexID`, `Sample Texture 2D LOD`, then a `Position` →
   `World Position Offset`. (No native Shader Graph node for
   `SV_VertexID` in 2022 LTS; use a Custom Function node.)

## Troubleshooting

**`SkinnedMeshRenderer` keeps deforming the right side.**
Re-run "QtMeshEditor → VAT → Build Test Scene" — the script strips
the `SkinnedMeshRenderer` + Animator on the VAT side. If you copied
the prefab manually, do it by hand.

**Right side is invisible.**
Likely Unity culling using the bind-pose AABB. `VATPlayer.OnEnable`
overrides `mesh.bounds` with the bake's bounds + 10% padding. If you
still see it, set the MeshRenderer's `Bounds Override` directly.

**Right side looks flat / wrong colors.**
The shader's `_HasNrmTex` defaults to 0. If you bake without
`--normals`, the shader falls back to a derived normal (tangent ×
world-up). Re-bake with `--normals` for proper lighting.

**Looking "back" / wrong direction.**
Unity and glTF agree on `forward = -Z`, so no rotation should be
needed. If the left side faces away from the camera, drag both
characters to face you — the bake captures whatever orientation
the rig had at import time.

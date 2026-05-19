# Godot 4 VAT Test Harness

A drop-in Godot 4 project that verifies QtMeshEditor's OpenVAT
(sharpen3d/openvat) export. Renders the original skinned mesh on the
left (ground truth via `AnimationPlayer`) and the VAT-driven mesh on
the right (shader replay, no skeleton bound). If the silhouettes match,
the export is faithful.

## What you need

- Godot 4.2 or newer (4.3 / 4.4 work too).
- A built QtMeshEditor at `build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor`,
  OR `qtmesh` on `$PATH` (any version that has the `vat` subcommand).

## 60-second quickstart

```bash
# From the QtMeshEditor repo root:
./tools/godot-vat-test/bake_and_stage.sh \
    "media/models/Rumba Dancing.fbx" "mixamo.com"

# Open the project (or press F5 in the Godot editor)
godot --path tools/godot-vat-test scenes/Main.tscn
```

You should see two characters dancing the same animation. The left
character is driven by the original FBX's skeleton via Godot's
`AnimationPlayer`; the right character is driven entirely by sampling
the baked position+normal texture in the vertex shader.

## Try other animations

```bash
./tools/godot-vat-test/bake_and_stage.sh \
    "media/models/Hip Hop Dancing.fbx" "mixamo.com"

./tools/godot-vat-test/bake_and_stage.sh \
    "media/models/Twist Dance.fbx" "mixamo.com" --fps 24
```

Each run replaces the staged bake and re-points the scene's
`bake_dir` + `gltf_path` — re-open the scene to pick up the change.

## What to look for

- **Silhouette match.** OpenVAT is always 16-bit RGB; the right side
  should be visually indistinguishable from the left, down to a sub-mm
  jitter at the texture's bound-edges.
- **Axis orientation.** Both characters face the same way. Source data
  is Ogre Y-up RH (`_axes` extension key in the sidecar); the embedded
  shader applies no swizzle since Godot is also Y-up RH.
- **Frame sync.** Press Space to pause both. They should freeze on
  visually-equivalent frames. Drift means the VAT FPS doesn't match
  the `AnimationPlayer`'s playback rate (tweak `fps_override` on the
  VAT node to compensate).

## Keyboard shortcuts

| Key | Action |
|---|---|
| Space | Pause / resume both characters |
| R | Reset VAT playback to frame 0 |
| ← / → | Step VAT by ±1 frame (while paused) |

## Project layout

```text
tools/godot-vat-test/
├── project.godot           Godot project descriptor
├── README.md               This file
├── bake_and_stage.sh       Bash helper: bake → convert → patch scene
├── scenes/
│   └── Main.tscn           Side-by-side comparison scene
├── scripts/
│   ├── Main.gd             Top-level controller (pause / status / keys)
│   ├── VATPlayer.gd        Loads the bake, drives current_frame
│   └── SkeletalLoader.gd   Loads the glTF + plays its AnimationPlayer
└── assets/
    └── <basename>_<anim>/  Created by bake_and_stage.sh:
        ├── <name>_pos.png            Packed VAT texture
        │                             (16-bit RGB, height = 2 × Frames;
        │                              top half positions, bottom half normals)
        ├── <name>-remap_info.json    OpenVAT sidecar (os-remap: Min/Max/Frames)
        ├── source.gltf               qtmesh convert of the original mesh
        └── source.bin                Buffer data for source.gltf
```

## Embedded shader

`scripts/VATPlayer.gd` carries the OpenVAT-compatible spatial shader
inline. Texture sampling layout matches the sharpen3d/openvat reference
shader's "Packed Normals" mode: position rows live in the top half of
the texture, normal rows in the bottom half, and the vertex shader
computes both UVs from `current_frame` + `frame_count`.

## Troubleshooting

**Right-side mesh is invisible / explodes.**
The mesh Godot loaded has a different vertex count than the bake.
Re-stage — `bake_and_stage.sh` always re-converts the source to glTF so
the vertex order matches.

**Left side is a green box.**
The glTF didn't load. Check that `assets/<bake>/source.gltf` exists
and that the scene's Skeletal node has `gltf_path` set. The staging
script should patch it; if you set up the project manually, edit the
scene and fill in the path.

**Both sides freeze on the same pose but don't move.**
SkeletalLoader couldn't find an AnimationPlayer. Confirm the glTF
actually has a clip (`qtmesh anim source.gltf --list`).

**Animation drifts apart over time.**
The VAT FPS doesn't match the live AnimationPlayer's rate. OpenVAT's
sidecar doesn't carry a framerate (positions are time-resampled at
bake time), so the VATPlayer drives playback at `fps_override` —
defaults to 30, the same default the CLI uses. If you baked at a
different `--fps`, edit the VAT node's `fps_override`.

## Next steps

This harness verifies the **OpenVAT** texture+sidecar round-trip end-to-end.
To extend it:
- Drive a Unity or Unreal scene from the same staged files for parity
  testing — see `docs/testing-vat.md`.
- Try `qtmesh vat <other-mesh>` to confirm multi-submesh meshes
  (Mixamo bodies with body+hair+clothing parts) line up via the
  per-surface `vertex_offset` uniform.

# PS1 golden capture suite (#659)

Parent epic: [#412](https://github.com/fernandotonon/QtMeshEditor/issues/412)

This document defines the **acceptance scenes** for closing the PS1 runtime rip epic. You supply your own BIOS and disc images; QtMeshEditor does not ship ROMs.

## Product bar

When epic #412 closes, maintainers should be able to **reliably extract some geometry** from real PS1 titles — not every mesh in every game, but **repeatable** success on the scenes below using **Capture Frame** → editor mesh → export ([#427](https://github.com/fernandotonon/QtMeshEditor/issues/427)).

Epic close requires **at least 2 of 3** scenes to pass the manual checklist on a maintainer machine.

## Golden scenes

| ID | Type | ISO env var | Reproduction steps | Pass criteria |
|----|------|-------------|-------------------|---------------|
| `homebrew-static` | Homebrew | `QTMESH_PS1_GOLDEN_HOMEBREW_ISO` (legacy: `QTMESH_PS1_TEST_HOMEBREW_ISO`) | Boot a static COP2/GP0 test ISO (synthetic scene with visible 3D tris). Arm capture, run a few seconds, **Capture Frame**. | ≥8 tris, recognizable mesh, export glTF opens |
| `retail-a` | Commercial | `QTMESH_PS1_GOLDEN_RETAIL_A_ISO` (legacy: `QTMESH_PS1_TEST_ISO`) | Example: platformer title screen — load `.cue`, press **Start**, pause at a **static camera** menu or title card. Arm capture, **Capture Frame**. | Silhouette matches emulator viewport screenshot |
| `retail-b` | Commercial | `QTMESH_PS1_GOLDEN_RETAIL_B_ISO` | Example: RPG / different engine — load `.cue`, reach a **fixed camera** scene (shop, menu, or pre-rendered backdrop with 3D props). Arm capture, **Capture Frame**. | Same as retail-a; geometry-only pass OK if textures are documented |

### Manual checklist (per scene)

1. **Capture Frame** → non-empty mesh in the editor (vertex/triangle counts in status).
2. Mesh is **visually recognizable** (not a flat screen-space slab) — compare to the PS1 viewport screenshot.
3. Vertex colors and at least one texture page look plausible, **or** note a **geometry-only** pass in the test log.
4. Export to glTF ([#427](https://github.com/fernandotonon/QtMeshEditor/issues/427)) opens in an external viewer without fatal errors.

Record pass/fail and QtMeshEditor version in your PR or release notes when claiming epic #412.

## Environment variables

| Variable | Purpose |
|----------|---------|
| `QTMESH_PS1_TEST_BIOS` | Path to `scph1001.bin` (or region-matched BIOS) |
| `QTMESH_PS1_GOLDEN_SCENE_ID` | Active golden ID: `homebrew-static`, `retail-a`, or `retail-b` (optional; tags Sentry breadcrumbs) |
| `QTMESH_PS1_GOLDEN_HOMEBREW_ISO` | Homebrew / test ISO (`.cue` recommended) |
| `QTMESH_PS1_GOLDEN_RETAIL_A_ISO` | First commercial golden ISO |
| `QTMESH_PS1_GOLDEN_RETAIL_B_ISO` | Second commercial golden ISO |

Legacy aliases still work for homebrew and retail-a: `QTMESH_PS1_TEST_HOMEBREW_ISO`, `QTMESH_PS1_TEST_ISO`.

## CI / local automated checks

**Default CI:** no ISOs required. `MeshReconstructorGoldenTest` runs slab/volume heuristics on synthetic captures.

**Optional integration** (maintainer machine or env-gated job):

```bash
export QTMESH_PS1_TEST_BIOS=/path/scph1001.bin
export QTMESH_PS1_GOLDEN_RETAIL_A_ISO=/path/game.cue
# optional: limit to one scene
export QTMESH_PS1_GOLDEN_SCENE_ID=retail-a

./build/bin/UnitTests --gtest_filter='MeshReconstructorGoldenTest.*'
```

`ConfiguredGoldenIsoReconstructsWithVolume` skips when BIOS/ISO paths are unset. When set, it boots libretro, captures ~240 frames, and asserts non-empty reconstruction, `!slabLike`, and `hasBounds()`.

Unset `QTMESH_PS1_FORCE_STUB` locally (CI forces stub).

## Related issues

- Per-draw matrix / GTE stats: #658
- GP0 FIFO accuracy: #662
- Session UI / golden picker: #425

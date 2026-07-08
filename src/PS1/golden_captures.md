# PS1 golden capture suite (#659)

Parent epic: [#412](https://github.com/fernandotonon/QtMeshEditor/issues/412)

This document defines the **acceptance scenes** for closing the PS1 runtime rip epic. You supply your own BIOS and disc images; QtMeshEditor does not ship ROMs.

## Product bar

When epic #412 closes, maintainers should be able to **reliably extract some geometry** from real PS1 titles — not every mesh in every game, but **repeatable** success on the scenes below using **Capture Frame** → editor mesh → export ([#427](https://github.com/fernandotonon/QtMeshEditor/issues/427)).

Epic close requires **at least 2 of 3** scenes to pass the manual checklist on a maintainer machine.

## Golden scenes

With the in-core capture chain (#813–#817) the pass bars gain **metric thresholds** on
top of the visual checklist. Record for every scene: the fork commit hash
(`scripts/build-ps1-rip-core.sh` pin), tracked/depth/none vertex percentages (status bar
`tracked N% · depth M%`), prim counts per source (`GP0 <source> (...)`), and a screenshot
pair (emulator viewport vs reconstructed viewport).

| ID | In-core pass bar (fork loaded, `in-core hooks: active`) |
|----|---------------------------------------------------------|
| `homebrew-static` | tracked ≥ 95%; reconstructed mesh **equals** the authored model; glTF export opens clean |
| `retail-a` | with `QTMESH_PS1_TMD_SCANNER=0`: recognizable silhouette, tracked ≥ 50%, `!slabLike`. With the scanner on: the in-core mesh ⊇ the TMD mesh geometry (same object recovered both ways = ground-truth cross-validation) |
| `retail-b` | as retail-a |
| `retail-c` | recognizable static geometry, tracked > 0 documented, depth-valid ≥ 50%, `!slabLike`; partial recovery allowed and documented |


| ID | Type | ISO env var | Reproduction steps | Pass criteria |
|----|------|-------------|-------------------|---------------|
| `homebrew-static` | Homebrew | `QTMESH_PS1_GOLDEN_HOMEBREW_ISO` (legacy: `QTMESH_PS1_TEST_HOMEBREW_ISO`) | Boot a static COP2/GP0 test ISO (synthetic scene with visible 3D tris). Arm capture, run a few seconds, **Capture Frame**. | ≥8 tris, recognizable mesh, export glTF opens |
| `retail-a` | Commercial | `QTMESH_PS1_GOLDEN_RETAIL_A_ISO` (legacy: `QTMESH_PS1_TEST_ISO`) | Example: platformer title screen — load `.cue`, press **Start**, pause at a **static camera** menu or title card. Arm capture, **Capture Frame**. | Silhouette matches emulator viewport screenshot |
| `retail-b` | Commercial | `QTMESH_PS1_GOLDEN_RETAIL_B_ISO` | Example: RPG / different engine — load `.cue`, reach a **fixed camera** scene (shop, menu, or pre-rendered backdrop with 3D props). Arm capture, **Capture Frame**. | Same as retail-a; geometry-only pass OK if textures are documented |
| `retail-c` | Commercial, **custom engine** | `QTMESH_PS1_GOLDEN_RETAIL_C_ISO` | Pick one of Crash Bandicoot / Spyro / FFVII (field) / MGS — the class that defeats every RAM-scan path. Document the exact title, region, save point and camera instructions when recording a pass. Requires the **rip fork** (`in-core hooks: active`). Arm capture at a static gameplay scene, **Capture Frame**. | Recognizable static geometry (explicitly allowed to be partial — document what is and isn't recovered); `tracked > 0` documented; depth-valid ≥ 50%; `!slabLike` |

### Manual checklist (per scene)

1. **Capture Frame** → non-empty mesh in the editor (vertex/triangle counts in status).
2. Mesh is **visually recognizable** (not a flat screen-space slab) — compare to the PS1 viewport screenshot.
3. Vertex colors and at least one texture page look plausible, **or** note a **geometry-only** pass in the test log (acceptable when VRAM status shows framebuffer mirror only — see #660).
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
| `QTMESH_PS1_GOLDEN_RETAIL_C_ISO` | Custom-engine golden ISO (Crash/Spyro/FFVII field/MGS class) |
| `QTMESH_PS1_RIP_INCORE` | `0` disables in-core hook registration — the A/B switch vs RAM-scan heuristics |

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

`ConfiguredGoldenIsoReconstructsWithVolume` is a no-op when BIOS/ISO paths are unset (CI has no retail ISOs). When configured locally, it boots libretro, captures ~240 frames, and asserts non-empty reconstruction, `!slabLike`, and `hasBounds()`.

Unset `QTMESH_PS1_FORCE_STUB` locally (CI forces stub).

## A/B harness (in-core vs RAM-legacy, #817)

Run the same scene twice and diff the capture stats:

```bash
# Run 1 — in-core (fork in PS1Cores/, default):
#   status bar shows: in-core hooks: active · GP0 gp0_incore (...) · tracked N% · depth M%
# Run 2 — RAM-legacy baseline:
export QTMESH_PS1_RIP_INCORE=0
#   status bar shows: in-core hooks: unavailable ... · GP0 ram_ot/ram_linear (...) · tracked 0%
```

Each **Capture Frame** writes a CSV dump to `<tmp>/qtmesh_ps1_capture/<id>.csv` with
per-prim `frame,provenance0,gteRecord0,viewW0` columns — diff the two runs' CSVs plus the
`ps1.rip.capture.gp0_hook` / `ps1.rip.matrix.stats` breadcrumb lines (prims per source,
tracked %, bounds, slabLike) and paste the comparison into this doc when recording a
golden run. Expected: in-core ≥ RAM-legacy on tracked %, prim coverage, and non-slab
bounds for every retail scene.

**Textures (#660):** use `mednafen_psx_libretro` / `beetle_psx_libretro` (software renderer). Avoid `beetle_psx_hw`. After capture, the session status bar shows `VRAM: full VRAM` when texture decode can read TPAGE/CLUT pages.

## Related issues

- Per-draw matrix / GTE stats: #658
- GP0 FIFO accuracy: #662
- Session UI / golden picker: #425
- In-core capture chain (fork ABI / GTE records / GP0 correlation / tiered reconstruction / validation): #813 #814 #815 #816 #817

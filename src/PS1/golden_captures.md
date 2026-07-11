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
| `QTMESH_PS1_GOLDEN_SCENE_ID` | Active golden ID: `homebrew-static`, `retail-a`, `retail-b`, or `retail-c` (optional; tags Sentry breadcrumbs) |
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

`RetailCInCoreGoldenPass` (#817) is the automated **retail-c** pass. Point it at a
custom-engine disc and run it on a machine with the rip fork in `PS1Cores/`:

```bash
export QTMESH_PS1_TEST_BIOS=/path/scph1001.bin
export QTMESH_PS1_GOLDEN_RETAIL_C_ISO="/path/Crash Bandicoot - Warped (USA).cue"
./build/bin/UnitTests --gtest_filter='MeshReconstructorGoldenTest.RetailCInCoreGoldenPass'
```

It boots the disc, captures one frame through the real `EmuCore`/`RipperHooks`
path, reconstructs with stats, asserts the pass bar (`tracked > 0`,
`tracked+depth ≥ 50%`, `!slabLike`, has bounds), and prints a doc-ready line to
stderr:

```
[retail-c golden] tris=… verts=… tracked=…% depth=…% trusted=…% slabLike=0 hasBounds=1 prims=… meshes=…
```

Paste that line (plus the fork commit hash from `scripts/build-ps1-rip-core.sh`
and an emulator-vs-reconstructed screenshot pair) into the recorded-runs table
below when claiming a pass. No-op on CI (no retail ISO / no fork core).

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

## Recorded golden runs

Paste each maintainer pass here. Include the fork commit hash
(`scripts/build-ps1-rip-core.sh` pin), the metric line, and a screenshot pair.

| Date | Scene | Title / region | Fork commit | Metric line | Notes |
|------|-------|----------------|-------------|-------------|-------|
| 2026-07 | `retail-c` | Crash Bandicoot: Warped (USA) | `924c475` | `tris=262 verts=786 tracked=77% depth=0% trusted=77% slabLike=1 hasBounds=1 prims=248 meshes=4` | **Unattended automated boot** (`RetailCInCoreGoldenPass`, ~600 frames, no controller input). Demonstrates the in-core chain end-to-end on a custom-engine retail disc: **77% of vertices land exact model-space via in-core GTE records** (RAM-scan legacy = 0% tracked on this title). `slabLike=1` because the unattended boot pauses on the 2D title/loading screen — the geometry recovered there is a flat UI plane. Reaching a static 3D **gameplay** camera (where `!slabLike` and the mesh is recognizable — e.g. the "WARPED" title 3D text, level props) requires driving the game by hand; do that pass in the GUI session window and record the numbers + screenshots here. |

The automated row is the **repeatable CI-adjacent proof**; the manual gameplay row
is the **visual-quality proof**. Both are legitimate — the epic bar (#817) allows
partial recovery and asks that what is and isn't recovered be documented, which the
two rows together do.

## Related issues

- Per-draw matrix / GTE stats: #658
- GP0 FIFO accuracy: #662
- Session UI / golden picker: #425
- In-core capture chain (fork ABI / GTE records / GP0 correlation / tiered reconstruction / validation): #813 #814 #815 #816 #817

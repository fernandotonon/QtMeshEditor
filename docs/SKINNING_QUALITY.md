# Skinning quality: metrics, acceptance suite, and the Mixamo comparison protocol

Issue [#819](https://github.com/fernandotonon/QtMeshEditor/issues/819) (Skinning v2, Slice E).
The algorithms these measure: **GeodesicVoxel** (default, Slice A),
**InverseDistance** (#402 legacy / fallback), the Slice-B post-passes,
and the **Dual-Quaternion display toggle** (Slice D).

## Metrics

All computed by `qtmesh skin <file> --evaluate [--json]` on the asset's
**existing** weights (whoever authored them), and by the pure-data
`SkinMetrics` / `SkinWeightsPost` APIs in the acceptance tests.

| Metric | Definition | Good looks like |
|---|---|---|
| Influence histogram | vertices per influence count (0–8), average, max | max ≤ 4 (hardware-skinning convention) |
| Weight smoothness | Laplacian energy: mean over mesh edges of ‖w_u − w_v‖² | lower = smoother falloffs; hard 0/1 borders score ~2 per edge |
| Bleed fraction | share of committed (vertex, bone) weights whose bone is **not geodesically local** to the vertex (GeodesicVoxelBind field) | 0 — geodesic-voxel weights are 0 by construction |
| Volume preservation | LBS-deformed / rest mesh volume under a specified joint rotation (`SkinMetrics::deformLBS` + `meshVolume`) | ≥ 0.9 at a 90° single-joint bend |

## Acceptance suite (CI)

Procedural, hermetic fixtures in `src/SkinMetrics_test.cpp` /
`src/GeodesicVoxelBind_test.cpp` (no binary assets, no downloads):

- **Two-limb proximity pair** (boxes 0.5 units apart, one bone each):
  geodesic-voxel bleed = **0** (asserted exactly); the inverse-distance
  contrast on the same fixture must show > 0.1 bleed — if it stops
  bleeding the fixture has lost its meaning.
- **U-shape bend-not-chord**: weights follow the geodesic path, never
  the Euclidean chord between arm tips.
- **Cracked (non-watertight) box**: weights match the closed box —
  voxel-scale hole closing.
- **90° elbow bend** (capsule tube, radius 0.5 × length 4, two bones,
  smoothing 8): volume ratio ≥ **0.9**. Reference run: **0.911**
  (rest 2.828, bent 2.577). For twists beyond what any weight map can
  fix, use the Dual-Quaternion display toggle (Slice D).
- **Smoothing property**: Laplacian relaxation strictly reduces the
  weight-field energy.

## Evaluating an asset

```bash
qtmesh skin character.fbx --evaluate            # text report
qtmesh skin character.fbx --evaluate --json     # machine-readable
qtmesh skin character.fbx --evaluate --voxel-res 128   # finer bleed field
```

Sample output (bandit.fbx, 90k verts, 119 bones, skinned by
`qtmesh skin --algo geodesic-voxel`):

```
Skin Evaluation
===============
Vertices:            90573
Bones:               119
Avg influences:      3.47
Max influences:      4
Smoothness energy:   0.004... (lower = smoother falloffs)
Bleed fraction:      0.0055 (weights not geodesically local)
```

## Mixamo comparison protocol (manual — no Adobe assets in-repo)

Adobe's Mixamo auto-rigger is the industry reference for one-click
skinning. The protocol compares our weights against it on the same
mesh without committing any Adobe-derived asset:

1. Take a CC0 humanoid (e.g. a [Quaternius](https://quaternius.com)
   character), export it **unrigged** as FBX.
2. Upload to [mixamo.com](https://www.mixamo.com), auto-rig with the
   default skeleton (no animation), download as FBX Binary, T-pose.
   Keep the file local — it is Adobe-processed and must not be
   committed.
3. Rig + skin the same unrigged mesh with ours:
   `qtmesh rig model.fbx --skeleton humanoid --skin -o ours.fbx`
   (or `qtmesh skin` if you already have a matching skeleton).
4. Compare: `qtmesh skin ours.fbx --compare mixamo_ref.fbx --json`
   — vertices are matched by position (Mixamo re-exports reorder
   them), bones by name; the report gives mean/max per-vertex weight
   L1 difference and the top-10 differing bones.
5. Also run `--evaluate` on both files and compare the metric table.

Interpretation: bone names differ between our templates and Mixamo's
(`mixamorig:*`) — rename with `qtmesh anim --rename` or map manually;
unmatched bone names inflate the L1 diff. The #819 target: UniRigML
within 10% of the Mixamo reference on the `--evaluate` metrics, GVB
within 20%.

The env-gated regression test (`SkinEvaluateTest.CompareAgainstReference`)
runs the comparison automatically when `QTMESH_SKIN_REF_FBX` (the
Mixamo file) and `QTMESH_SKIN_OURS_FBX` are set; it is skipped
otherwise, so CI stays hermetic.

### Recorded run

_Pending a manual Mixamo export (requires an Adobe account). Record
the `--compare` and `--evaluate` outputs here when first run._

## Dual-quaternion display (Slice D)

`Animation mode → Skinning → Display: Linear | Dual Quaternion`, or
MCP `set_skinning_display {mode: "dual-quaternion"}`. Kills the
candy-wrapper collapse on twists that no weight map can fix. Display
only: exporters consume the unchanged vertex weights — engines
re-skin with their own blend. Entities above the RTSS bone cap (96)
stay on the default path.

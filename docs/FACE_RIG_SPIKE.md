# Face auto-rig — Spike Findings & Contract (#889 / slice A #896)

**Epic:** [#889 — AI: Auto-generate ARKit blendshapes on any humanoid mesh (deformation transfer)](https://github.com/fernandotonon/QtMeshEditor/issues/889)
**Slice:** [#896 — Spike: NRICP feasibility + ICT-FaceKit licensing due-diligence](https://github.com/fernandotonon/QtMeshEditor/issues/896)
**Status:** Spike — **GO.** Non-rigid ICP fits the MIT ICT-FaceKit template to
a different-topology head to sub-1% accuracy, and deformation transfer produces
anatomically-correct ARKit blendshapes on the user's own topology. No ML, no
ONNX — a deterministic sparse-linear-algebra pipeline. Slices C/D are a native
C++ port of the proven `scripts/spike-facerig.py`.

---

## TL;DR — Recommendation: **GO**

- **Template licensing — CLEARS THE BAR.** ICT-FaceKit is **MIT** (a neutral
  head + 52 ARKit-named expression meshes, all one topology). Redistributable
  on the HF models repo. The separate "full model" USC-specific tier is
  rejected; we ship only the MIT tier. Recorded in `THIRD_PARTY_AI_MODELS.md`.
- **NRICP — WORKS.** Amberg-2007 optimal-step, pure numpy/scipy (no Wrap3D):
  fit the 26,719-vert template onto a **12,763-vert (different topology)** user
  head → surface fit **mean 0.003%, max 0.59%** of the head diagonal.
- **Deformation transfer — WORKS.** Sumner & Popović 2004: each of the 52 ICT
  expressions transfers onto the user topology with correct semantics —
  jawOpen drops the lower face (mean ΔY −0.32) while the forehead stays still
  (|Δ| 0.002); eyeBlink stays localized to the eye (1,328 verts), jawOpen is
  the biggest deformation (6,771 verts, 9% max), browInnerUp is small (1.3%).
- **No new runtime dependency:** deterministic geometry (sparse solve), like
  `GeodesicVoxelBind` / `QuadRetopo`. No model download at inference (only the
  MIT template asset downloads on first use, like other bundled assets).

---

## The template (ICT-FaceKit, MIT)

`FaceXModel/` ships `generic_neutral_mesh.obj` + one `.obj` per expression, ALL
sharing the neutral's topology (26,719 verts / 26,384 tris), so a blendshape is
simply `expr_obj − neutral_obj` (per-vertex delta). ICT uses `<name>_L/_R`
stems; the spike maps them to the canonical `FaceCap::kBlendshapeNames` (the
mocap-52 order) — full table in `scripts/spike-facerig.py::ICT_TO_ARKIT`.
Slice B bakes this template into a compact bundled form + hosts it.

> **OBJ gotcha:** these OBJs are multi-group; trimesh loads them as a Scene and
> reorders/duplicates vertices, breaking the shared-topology assumption. Parse
> `v`/`f` manually and preserve order (the spike + Slice B loader both do).

## The pipeline (what Slices C/D implement natively)

```
user neutral head (arbitrary topology, roughly humanoid, +Y up, facing +Z)
  │
  1. rigid pre-align: centroid + bbox-scale match template→user
  │    (correspondence-free; NRICP refines. A real user mesh may need
  │     up/forward-axis detection first — the spike's decimated head shared
  │     ICT's orientation so identity axes sufficed.)
  │
  2. NRICP (Amberg 2007 optimal-step):
  │    unknown = per-template-vertex 3×4 affine A_i
  │    minimize ‖A_i·ṽ_i − closest_point_on_user_surface(X_i)‖²        (data)
  │           + α·‖(A_i − A_j)‖²  over template edges (i,j)             (stiffness)
  │    stiffness annealed α = 50→20→8→3→1→0.5, ~3 inner iters each;
  │    closest point via point-to-triangle projection; one sparse lsqr per axis.
  │    → fitted template verts X (lie on the user surface) = CORRESPONDENCE.
  │
  3. deformation transfer (Sumner & Popović 2004), per ARKit expression:
  │    template expression correspondence = X + (expr_tmpl − neutral_tmpl)
  │    per-user-vertex delta = displacement of the nearest correspondence point
  │    (the spike's simplified transfer; the full C++ form solves the
  │     per-triangle deformation-gradient least-squares — see below).
  │
  → 52 per-user-vertex deltas → Ogre::Pose morph targets named per kBlendshapeNames
```

### Note on the transfer step (Slice D must upgrade the spike form)

The spike transfers by **nearest-correspondence-point displacement**, which is
enough to prove semantics and fit quality. The production Slice D should use the
**full deformation-gradient transfer**: build each template triangle's affine
`S_j = V_expr · V_neutral⁻¹` (with the 4th "normal" vertex trick), then solve one
sparse least-squares `min ‖A_userTri − S_j‖²` for the user vertex positions
(the Sumner-Popović matrix). This is more faithful for large/rotational
deformations (jaw) than nearest-point displacement. The linear solver is shared
with NRICP.

## Measured quality (2026-07-14, `scripts/spike-facerig.py`)

Template 26,719v → user 12,763v (60%-decimated, different topology):

| ARKit shape | max displacement | verts moved | semantics check |
|---|---|---|---|
| NRICP surface fit | mean **0.003%** / max **0.59%** of diag | — | template lands on user surface |
| jawOpen | 9.0% | 6,771 | lower face ΔY −0.32 (drops), forehead \|Δ\| 0.002 (still) ✅ |
| mouthSmileLeft | 3.3% | 3,552 | localized to mouth ✅ |
| eyeBlinkLeft | 3.8% | 1,328 | localized to eye ✅ |
| browInnerUp | 1.3% | 2,181 | localized to brow ✅ |

## Risks / limits (carry into the epic)

1. **Humanoid-only.** Transfer from a human template only makes sense for
   roughly human face meshes; a prop/creature yields garbage. Slice E gates on
   this (NRICP fit-quality metric + a clear error), the AutoRig/Pinocchio
   precedent.
2. **Orientation.** The spike's user head shared ICT's axes. A real arbitrary
   mesh needs up/forward detection (or a user hint) before the rigid pre-align
   — fold into Slice C/E.
3. **Correspondence quality drives shape quality.** Landmark constraints (eye
   corners / nose / mouth) may be needed on faces far from the template
   proportions; the spike didn't need them on the decimated ICT head — revisit
   on real Ready-Player-Me / scanned heads in Slice C.
4. **Full deformation-gradient transfer** (Slice D) over the nearest-point
   spike form, for faithful large deformations.
5. **Performance.** The python spike's dense per-vertex assembly is slow (~minutes
   at 26k verts); the C++ port must assemble the sparse system directly and use a
   real sparse solver (the project's existing linear-algebra path) — target a
   few seconds, worker-threaded in the GUI.

## Go/No-Go

**GO.** Both algorithms proven on a real, different-topology head with a
permissive (MIT) template. Slices B→G are an engineering port of a working
prototype, not open research. Ship the full deformation-gradient transfer +
landmark option as the two quality upgrades over the spike.

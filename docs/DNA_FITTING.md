# MetaHuman body fitting and editing — Phase 4 findings (epic #1095, #1098)

Status: **investigation done, prototype implemented** (`qtmesh dna fit`,
`src/DNA/DnaFit*.{h,cpp}`). This document answers the epic's questions in the order
it asks them. Numbers come from Epic's `SKM_Body.dna` (LOD 0, 30 455 positions,
342 joints) via `qtmesh dna fit` and the unit tests.

## 1. What OpenRigLogic gives us — and what it does not

Verified against the pinned 5.8 sources:

| Capability | Provided? |
|---|---|
| Read / write binary + JSON DNA (every layer) | **Yes** — `BinaryStreamReader`, `BinaryStreamWriter`, `Writer::setFrom(reader)` copies all layers |
| Edit stored data (joint neutral translations, vertex positions, RBF solvers, joint groups …) | **Yes** — per-field writer setters, including the full RBF and joint-group API |
| Evaluate the rig (RBF → corrective joints) | **Yes** — RigLogic |
| **Calibrate / re-fit a DNA to a changed mesh** | **No.** There is no calibration code anywhere in the repository (no `calib` in headers, sources or docs). DNA calibration is a separate Epic library that is not part of OpenRigLogic. |

So *serialising* a modified DNA is supported; *recalibrating* one is ours to build.
The epic was right not to assume otherwise.

## 2. Correspondence requirements (explicit)

The solver needs to know, for every target vertex, which source vertex it is.

* **Supported:** the target has the **same topology** as the source — a sculpt,
  lattice or other position-only edit of the imported character, or another
  MetaHuman body of the same LOD (positions line up one-to-one; pass the other `.dna`
  as `--target`).
* **Not supported (refused with a message, never guessed):** different vertex counts
  or a different submesh structure; edited UVs; retopologised, split, decimated or
  subdivided targets; arbitrary wrapped meshes.

### The vertex-order trap (found while building this)

The obvious implementation — assume the target's vertices are in the same order as
the imported character's — **does not survive the editor's own import**. Re-exporting
the *same, unedited* glb and importing it again returned the same set of vertices
with 99.98 % of the indices permuted (32 327 of 32 334), giving a bogus 471 mm mean
"shift". The first import after exporting straight from the DNA happened to keep the
order, which is why a naive identity test passes and then fails for any real
round-tripped edit.

The fix is `matchImported`: **UV0** survives sculpting (positions move, UVs do not)
and identifies a surface point, so every target vertex is matched to the source
vertex with the same UV. V orientation is detected automatically; if a UV has several
candidates the nearest in 3D wins; seam duplicates (one DNA position, several layout
vertices) are averaged and their disagreement is reported. It **fails** — instead of
guessing — if fewer than 99.5 % of vertices match (changed UVs / topology). Vertex
order is only a fallback when the target has no UVs at all. Measured on the body:
100 % matched, 0 ambiguous, for the re-exported glb, the lattice-identity glb and the
edited targets.

## 3. The solver (what is implemented)

1. displacement field = target − source at every matched vertex;
2. a smooth **RBF warp** — polyharmonic kernel φ(r) = r plus an affine polynomial,
   on ≤ 900 voxel-subsampled control points (74 for the body) — interpolates it, so
   **every affine edit (scale, shear, translation) is reproduced exactly**;
3. each joint moves by the warp evaluated at its neutral position; **orientations and
   skin weights are kept**, local translations re-derived through the (unchanged)
   parent rotations;
4. the bind pose becomes the target mesh exactly.

Solve time ≈ 0.3 s end to end for the body, including import of the target.

## 4. Quality: "usable deformation across reference poses"

`--poses` skins the original and the fitted rig (no correctives) through eight
reference poses (knee, hip, hip+knee, two shoulders, elbow, right-side knee/shoulder)
and compares them. The edit-independent measure is **edge stretch**: the ratio of
every triangle edge's posed length to its bind length; a fit that keeps skinning sane
reproduces the source rig's stretch.

| Edit | Warp error | Pose result |
|---|---|---|
| identity (re-exported glb) | 0 | 0 shift in all poses |
| uniform ×1.10 | 0.00 mm | **0.000 mm** vs the ideal `1.1 × posed original` in all 8 poses; stretch Δ 0.000 |
| anisotropic (taller, y × 1.12) | 0.00 mm | stretch Δ max 0.08–0.34, **p99 ≤ 0.026** |
| local (hips x × 1.25 below y = 0.9) | 3.55 mm | stretch Δ max ≤ 0.148, **p99 ≤ 0.021** |

(A control test proves the metric works: scaling the mesh but leaving the original
joints gives stretch Δ > 0.15 where the fitted rig gives < 1e-4.)

The fitted DNA also runs through the Phase 2 corrective pipeline unchanged: posing
the wide-hips body with correctives moves the surface by 39.4 mm (3 022 vertices over
1 mm) versus 39.3 mm (3 023) for the original, and the renders pose identically with
the new hips and no tearing.

## 5. LOD propagation

The warp is a function of position, so it applies to every LOD with **no second
correspondence**. Measured as the distance of the warped LODn vertices to the fitted
LOD 0 surface, against the same gap before fitting (uniform ×1.10):

| LOD | warped gap | unwarped baseline |
|---|---|---|
| 1 | 0.03 mm | 0.03 mm |
| 2 | 1.70 mm | 1.54 mm |
| 3 | 2.73 mm | 2.49 mm |

i.e. the coarser LODs follow the fine one as closely as they sat to it originally.
`--write-dna` applies the same warp to every LOD mesh in the file.

## 6. Serialisation is not recalibration

`--write-dna` rewrites joint neutral translations and every mesh's vertex positions
(in the file's own units and axes — the mapping from the importer's frame is *derived
from the data*, never assumed) and copies all other layers untouched. Validation:
the written file reads back within **0.0001 mm**, RigLogic loads it, and its rest pose
equals the fitted skeleton. What it is **not**: the RBF poses, joint-group deltas and
neural-net data are still the ones authored for the *original* proportions, so a large
edit drifts there. Vertex normals are copied unchanged. A recalibration would have to
re-solve corrective joint deltas against the new body — that is the part that needs
a real solver (or Epic's calibration library) and is the next step if corrective
fidelity on edited bodies matters.

## 7. Preview and reversible commit

* **Preview** = the report (`qtmesh dna fit … --poses --lods [--json]`): nothing is
  written unless `-o` / `--write-dna` is given.
* **Commit** is non-destructive: the fitted result goes to a *new* glb / DNA and the
  source is never overwritten (the writer refuses `out == source`).
* **Not built:** an in-editor preview with an undo step. Moving a live entity's bind
  pose means rebuilding its skinned mesh (the 256-bone and pose-buffer constraints
  from Phases 2–3 apply), so it was left for a follow-up rather than faked.

## 8. Generic RBF corrective authoring (exploration)

For stylised / non-MetaHuman characters: the DNA writer exposes the complete RBF solver
API (`setRBFSolver*`, `setRBFPose*`) and the joint-group / PSD setters, so a corrective
DNA **can** be authored from scratch with the library — raw controls = driver-joint
quaternions (the convention Phase 2 verified), poses = authored target rotations,
outputs = corrective joint transforms. The practical authoring loop would be: pose a
driver, sculpt/move a corrective bone, "add pose"; the tool emits a solver per driver
and the joint-group matrix. Open questions that make this a separate feature, not part
of this prototype: a corrective *bone* per pose needs skin weights (a Ogre 256-bone
budget issue), and an in-editor RBF without DNA (our own pose-space solver driving
existing morph targets) may be the better product for game assets. Recommendation:
prototype the DNA-free pose-space version first.

## 9. Known limits / follow-ups

1. Corrective data is not recalibrated (section 6).
2. No in-editor preview/undo (section 7).
3. Non-affine edits are followed only by their smooth part (reported as
   `warpResidualMm`; > 5 mm raises a warning).
4. The target must come through the editor's importer as the same submesh structure;
   multi-mesh bodies (head/body combos) need every mesh present.
5. Joint orientations are kept — a large limb-length change with the same rotations is
   what the stretch metric exists to flag.

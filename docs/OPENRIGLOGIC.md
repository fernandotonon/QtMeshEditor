# MetaHuman DNA / OpenRigLogic (epic #1095) — import + body correctives

Status: **Phases 1–3 implemented; Phase 4 (fitting) investigated with a working prototype — see `DNA_FITTING.md`.** This document records what was verified
and the gaps that shape them.

## What shipped

| Piece | Where |
|---|---|
| Optional dependency | `-DENABLE_OPENRIGLOGIC=ON` → `cmake/OpenRigLogic.cmake` (FetchContent, pinned commit, static) |
| Pure-data reader | `src/DNA/DnaReader.{h,cpp}` — descriptor, joints, meshes, skin, blend shapes, rig-logic inventory |
| Ogre adapter | `src/DNA/DnaScene.{h,cpp}` — skeleton + one submesh per DNA mesh + morph targets |
| Import hook | `.dna` in `MeshImporterExporter` (GUI open, `qtmesh convert head.dna -o head.glb`, MCP `load_mesh`) |
| CLI | `qtmesh dna info <file.dna> [--lod N] [--json] [--keep-units]` |
| Tests | `src/DNA/DnaReader_test.cpp` (synthetic DNA written by the library's own writer) |

A build without the flag compiles; every entry point reports
`rebuild with -DENABLE_OPENRIGLOGIC=ON`.

## Feasibility findings

* **License:** OpenRigLogic is MIT. Sample characters (Fab Standard License)
  and MetaHuman assets are separate and are never bundled — see
  `THIRD_PARTY_AI_MODELS.md`.
* **Revision:** `5.8` branch, commit `55756aca…` (RigLogic 13.2.9). `main` is
  upstream-labelled experimental (13.5.0 at spike time).
* **One library, two jobs:** `riglogic4` contains both the DNA reader/writer
  and the RigLogic evaluator, so Phase 2 needs no further dependency.
* **Build:** verified on **macOS arm64 (Apple Silicon)** — clean static build,
  no source-tree writes with `READ_ONLY_SOURCE_TREE=ON`. Upstream targets
  Windows/Linux too and has no third-party runtime deps, but **Windows (MinGW)
  and Linux were not built in the spike** — they are covered by CI once the flag
  is enabled there. Action item before enabling by default.
* **API:** `dna::BinaryStreamReader` with layer selection (`Definition`,
  `Geometry`, `RBFBehavior`, `MachineLearnedBehavior`, `TwistSwingBehavior`,
  …), LOD selection, and a library-side coordinate-system transform.
  Error statuses distinguish signature / version / invalid data, which the
  reader maps to actionable messages.
* **Body correctives are in the data model:** DNA carries PSD rows, joint
  groups, RBF poses (`getRBFPoseCount`), neural networks, and twist/swing
  setups. `qtmesh dna info` prints these counts — the starting inventory for
  Phase 2.

## Conventions used by the importer

| Topic | Choice |
|---|---|
| Units | cm → metres by default (`--keep-units` / `Units::Preserve`); glTF is metre-based and the viewport grid expects ~1-unit characters |
| Coordinates | converted by the library to right-handed, Y up, character facing **−Z** by default (the editor's viewport convention). The file itself is `x=left y=up z=front` (faces +Z, left on +X); the library is asked for that identity transform and `yawRig` turns the root joints + meshes 180° about Y afterwards, so RigLogic's joint-local outputs stay valid. `ReadOptions::facePlusZ` / `QTMESH_DNA_FACE_PLUS_Z=1` keep +Z (glTF/Mixamo) |
| Euler → quaternion | Maya rotate order: first letter applied first (`xyz` ⇒ X, then Y, then Z; column-vector product Rz·Ry·Rx) — equal to OpenRigLogic's row-vector `Rx*Ry*Rz`. The first cut multiplied the other way round and produced a scrambled skeleton |
| Neutral joint space | **PARENT-RELATIVE** (verified on Epic's real DNA; `NeutralSpace::World` is only for producers that write world transforms). The root joint carries a −90° X orientation that turns the data's Z-up joint axes into Y-up |
| UVs | V flipped (DNA bottom-left → Ogre/glTF top-left) |
| Faces | n-gons fan-triangulated; invalid indices dropped and reported, never dereferenced |
| LODs | one LOD imported (default 0); meshes that exist only at other LODs are reported |
| Names | blend-shape channel names become morph-target names; mesh names become submesh names |

## Verified on Epic's own archetype DNA (UE 5.7 `SKM_Body.dna` / `SKM_Face.dna`)

Not redistributed — read from a local Unreal install. Both import; the head sits directly
above the body in the shared space, the character faces −Z after the import turn (+Z with `facePlusZ`),
and the skeleton agrees with the skin: the median vertex lies 3.9 cm (body) / 1.0 cm (face)
from its dominant joint in world space. Findings:

* **Ogre's 256-bone limit is a hard wall.** Body = 342 joints, face = 870 (almost all
  skinned: twist/corrective joints). The importer drops joints with no skin weights, then
  **folds the lowest-influence leaf joints into their parent** until it fits, with a
  `LOSSY` warning. Phase 2 cannot rely on Ogre skinning for the full rig.
* **Skin influences:** up to 11 (body) / 10 (face) per vertex vs Ogre's 4.
* **Inventory:** body — 176 raw controls, 72 joint groups, **215 RBF poses**, 8 twists,
  8 swings; face — 174 GUI / 263 raw controls, 545 PSD rows, 122 joint groups, 38 RBF poses.

## Recorded gaps (decisions for later phases)

1. **Skin influences:** MetaHuman body vertices can carry up to ~12 influences;
   Ogre hardware skinning takes 4. The importer keeps the 4 strongest,
   renormalises and **reports the count of truncated vertices**. Phase 2 must
   decide between more influences (custom skinning path) and baking.
2. ~~**No rig evaluation yet**~~ — resolved for body correctives in Phase 2 (below).
   Facial controls and corrective blend shapes/animated maps are still open (Phase 3).
3. **Reference validation:** joint space, Euler order and coordinate conversion are now checked against Epic's real DNA (vertex↔dominant-joint distance), but not yet against RigLogic's own evaluated output; Phase 2 must compare posed frames with a trusted reference. The repo ships no distributable MetaHuman, so this needs a user-supplied asset.
4. **Textures/materials:** DNA contains no textures. The DCC export ships them
   separately; the importer assigns `BaseWhite`. Texture + material binding is
   a follow-up.
5. **Groom and clothing** are not in the DNA/DCC package. They are separate
   pipeline inputs (import as ordinary meshes and bind to the DNA skeleton);
   not handled automatically.
6. **JSON DNA** is not read (binary only).
7. **Export:** the only DNA writer is `qtmesh dna fit --write-dna` (Phase 4):
   it rewrites joint neutral translations and mesh positions of a *fitted* rig
   and must read back and load in RigLogic, else the file is removed. DNA
   serialisation is not rig recalibration — corrective data is not re-solved
   (see `DNA_FITTING.md` section 6). There is no general DNA export.
8. **LOD propagation / lower-LOD skeleton pruning:** the full skeleton is
   imported at every LOD; `Rig::lodJointIndices` records the per-LOD subset.

## Phases still open

4. Body mesh fitting: prototype done (`DNA_FITTING.md`); in-editor preview/undo and corrective recalibration remain.
Follow-up (separate issue): image-based body prediction.


---

# Phase 2 — body correctives (#1096)

A MetaHuman body DNA carries a pose-space rig: 44 **driver joints** (clavicle,
upper arm, thigh, calf, hand, foot, every finger joint…) feed their rotation into
72 **RBF solvers** (215 poses), whose blend weights drive ~234 **corrective
joints** (`_in_/_out_/_fwd_/_bck_`, bulge, twist) that fix elbows, knees,
shoulders and hips as the limbs bend. `DnaCorrectiveManager` evaluates that rig
every frame on top of the skeleton's animation.

| Piece | Where |
|---|---|
| RigLogic wrapper (pure data) | `src/DNA/DnaRigLogic.{h,cpp}` — `RigEvaluator` |
| Live evaluation + toggle | `src/DNA/DnaCorrectiveManager.{h,cpp}` (QML singleton, `SceneManager::Listener`) |
| Inspector | Animation mode → **DNA Correctives** (checkbox + live cost) — `qml/DnaCorrectivesPanel.qml` |
| MCP | `get_dna_correctives`, `set_dna_correctives {enabled}` |
| CLI | `qtmesh dna pose <body.dna> --set calf_l=z:-90 [--compare] [--no-correctives] [-o out.png/.stl/.glb] [--json]` |

## Conventions (verified on Epic's SKM_Body.dna)

* Raw control `"<joint>.qx|qy|qz|qw"` is the joint's rotation **relative to its
  neutral pose, in its own local frame** (identity at rest). The manager reads
  `neutral⁻¹ · current` from the posed Ogre bone (`Bone::getInitialOrientation`).
* RigLogic joint outputs are **deltas** laid out `joint*9 + attribute` (tx ty tz,
  rx ry rz, sx sy sz): cm and Euler degrees in the descriptor's sequence. The
  final local rotation is `eulerToQuaternion(neutral + delta)`, **not** a
  quaternion product.
* **Evaluation order / no double transform:** animation clips write only the
  driver/skeleton joints (the corrective joints have no tracks). Each frame, after
  the skeleton is posed from its clips (`setAnimationState` resets every bone, the
  corrective ones included, to neutral), the manager overwrites the corrective
  joints with the rig's **absolute** local transforms and marks the state set
  dirty (so a paused clip still shows the result). A corrective can therefore
  never stack on itself.

## Validation

* **Against RigLogic's own data, every pose:** for each of the 72 solvers and
  every interpolating RBF pose, feeding that pose's authored target quaternion
  reproduces the DNA's stored corrective output to < 0.002 (`DnaRigEvaluator.
  EveryRbfPoseTargetReproducesTheDnasOwnCorrective`; measured max error 1e-5 on
  the probe).
* **Rest pose is identical to the importer's skeleton** (`NeutralPoseMatchesThe
  ReaderSkeleton`) — otherwise applying correctives would shove bones off the
  bind pose.
* **Scene level** with a fake rig (`DnaCorrectiveManager_test.cpp`): driver
  read-out relative to neutral, absolute corrective writes without accumulation,
  toggle restoring plain skinning while keeping a preview pose, missing joints,
  destroyed entities.
* **Not done:** a comparison against *Unreal's* rendered output of the same pose.
  The RigLogic maths is the same library Unreal uses, but skinning differs (see
  limits), so this is a plausibility check, not visual parity.

### What the correctives change (Epic SKM_Body, corrected vs plain skinning)

| Pose | Vertices moved by the pose (max) | Corrective adds (max / p99) | Vertices >1 mm / >5 mm |
|---|---|---|---|
| calf_l z −90° | 588 mm | 36.8 mm / 14.2 mm | 1466 / 696 of 32 334 |
| calf_l z −100° + thigh_l y −60° | 720 mm | 39.3 mm / 21.4 mm | 3023 / 2105 |
| upperarm_l z −80° | 557 mm | 51.0 mm / 16.3 mm | 1093 / 710 |
| upperarm_l y +70° | 636 mm | 14.9 mm / 5.4 mm | 771 / 347 |
| clavicle_l z +25° | 233 mm | 33.6 mm / 9.0 mm | 987 / 623 |

(`qtmesh dna pose body.dna --set … --compare`.) Posed renders show smooth
shoulders/knees/hips with no tearing or collapse.

## Performance (Apple M-series, body LOD 0, 255-bone skeleton)

RigLogic **0.004 ms** + bone writes **0.04 ms** per frame for 44 drivers and 179
corrective bones — negligible next to rendering. Software-skinned reads for the
`--compare` metric are the only expensive part and are CLI-only.

## Limits that matter

1. **The 256-bone Ogre limit still bites.** Of the 234 corrective joints, **55 are
   not in the imported skeleton** (the base skeleton plus the skinned correctives
   exceeds 256 even before folding), so their corrections are not evaluated; 179
   are. The importer now folds *unprotected* leaf joints first and rig-driven
   ones last, and `get_dna_correctives` / the Inspector report the missing count.
   Full fidelity needs the skeleton limit lifted (Ogre) or a split-palette
   approach — see the epic's follow-up.
2. **4 skin influences** (MetaHuman uses up to 11) still approximate the weights
   the correctives were authored against.
3. **ConstraintManager + corrective manager on the same entity:** both re-pose
   from the clips before writing, so the later listener discards the earlier
   one's writes.
4. Head DNAs (no quaternion drivers) import as before and are **not** evaluated —
   facial controls are Phase 3.
5. Corrective *blend shapes* and animated maps are not driven yet (the body has
   none; the face does).


---

# Phase 3 — facial controls: a MetaHuman head → ARKit morph targets (#1097)

A MetaHuman face DNA is a JOINT rig: 870 joints (732 skinned), 263 expression raw
controls (`CTRL_expressions.browDownL`, `eyeBlinkL`, `jawOpen`, `mouthCornerPullL`,
`mouthFunnelUL` …), 545 PSD rows, 38 RBF poses, plus `neck_01/neck_02/head`
quaternion drivers. Ogre cannot hold that skeleton (256 bones) and the editor's
face capture, lipsync and every exporter speak morph targets, so the rig is
**baked**, not played:

```
delta(shape) = skin(rig evaluated with the shape's controls) − skin(neutral)
```

`skin()` is full linear-blend skinning over the COMPLETE skeleton with every
influence (up to 10 per vertex; Ogre would clip to 4), so a single baked shape is
exact for its control set. Result: the head imports with the 50 ARKit-named
targets, which the existing face capture (`qtmesh mocap --face`), Audio2Face
lipsync (`qtmesh lipsync`) and any glTF/FBX export drive directly — no mapping
layer in between (the names are the canonical ARKit names).

| Piece | Where |
|---|---|
| ARKit-52 → MetaHuman control table (data, overridable) | `src/DNA/DnaArkitMap.{h,cpp}` |
| Bake + linearity measurement (pure data) | `src/DNA/DnaFaceBake.{h,cpp}` |
| Import integration | `DnaScene` — automatic for any DNA that exposes expression controls |
| CLI | `qtmesh dna face head.dna [-o head.glb \| sheet.png] [--all-controls] [--map arkit.json] [--preview label:shape=w,… ] [--validate] [--json]` |
| Opt-out | `QTMESH_DNA_FACE_MORPHS=0` (face bake), `QTMESH_DNA_CORRECTIVES=0` (live neck correctives) |

## The mapping (documented supported subset)

All **51** ARKit channels (every canonical name except `_neutral`) have a table
entry; **11 are flagged approximate** because MetaHuman has no single equivalent:
`cheekSquint{Left,Right}`, `eyeSquint{Left,Right}`, `mouthClose`, `mouthFunnel`,
`mouthPucker`, `mouthRollLower/Upper`, `mouthShrugLower/Upper` (the table records
why for each). L/R are the character's left/right on both sides; ARKit "In"/"Out"
are nose-relative, so `eyeLookInLeft` drives MetaHuman's `eyeLookRightL`. A project
can replace or drop entries with a `qtmesh-dna-arkit-map-v1` JSON (`--map`); an
unknown ARKit name is an error, never a silent no-op.

## Validation (Epic SKM_Face.dna, LOD 0)

* **50 of 51 shapes bake in 22 ms**; `mouthClose` (all four lips-together controls)
  drives no vertices at LOD 0 and is reported, not faked.
* **Direction/side:** `jawOpen` moves the lower face DOWN (largest displacement
  31.6 mm), left shapes peak at +X and right shapes at −X, and left/right magnitudes
  agree within 30 % (unit-tested on the real file).
* **Visual:** a contact sheet (`--preview`) shows neutral, jaw open (mouth, teeth
  and tongue visible), smile (cheeks and corners lift), blink (lids close) and a
  pucker rendering correctly on the live entity.
* **Nonlinearity, measured** (`--validate`: rig evaluated with several controls at
  once vs the sum of the baked shapes):

| Expression | Max error | Mean error | Expression size |
|---|---|---|---|
| jawOpen + smileL + smileR | 4.2 mm | 0.36 mm | 31.6 mm |
| eyeBlinkL + browDownL | 0.07 mm | 0.000 mm | 15.1 mm |
| mouthFunnel + jawOpen + browInnerUp | 3.3 mm | 0.28 mm | 38.8 mm |
| mouthPucker + jawOpen | **16.6 mm** | 1.1 mm | 31.6 mm |

Typical expressions are within a few millimetres; **pucker + open jaw is the worst
case and is clearly nonlinear** — a morph target cannot keep it, and that is the
price of not shipping 870 joints.

## What was found along the way

* **Ogre software pose animation needs POSITION in its own tightly packed buffer.**
  It addresses positions as `base + vertex * 3` floats. `EditableMesh` interleaves
  position/normal/uv, so as soon as any pose state was enabled the whole head
  exploded into spikes (even at weight 0, even for a 4-vertex quad). The glTF
  importer avoids it by building separate buffers. The DNA importer now splits
  POSITION into its own buffer for meshes that carry morph targets; a scene test
  pins it. This also affects any other `EditableMesh`-built mesh that later
  receives poses (retopo/split outputs) — worth a follow-up.
* Poses are now attached to the mesh BEFORE the entity exists (as the glTF
  importer does), not to a live entity.

## Gaps (decisions for later)

1. **Joint animation baking is not offered** — the face skeleton does not fit
   Ogre. The deliverable is shape-space (morph) baking; evaluated joint curves are
   not exported.
2. **Animated maps (82, e.g. wrinkle masks) are not baked or applied.**
3. **Corrective joints on the head** (neck) still run live through the Phase 2
   manager; on the face 591 of 840 rig-driven joints are not in the Ogre skeleton
   (the same 256-bone limit), which is harmless for the baked morphs but is
   reported.
4. Only LOD 0 meshes are baked; teeth/tongue/eyes/lashes are included as slices.
5. `--all-controls` bakes every `CTRL_expressions.*` control as its own target
   (~260): useful for authoring, heavy for the Inspector list.
6. No GUI surface beyond the import itself; the targets appear in the normal
   Vertex Morph Animation list.

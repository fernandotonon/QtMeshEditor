# MetaHuman DNA / OpenRigLogic (epic #1095) — Phase 1 feasibility + import

Status: **Phase 1 spike implemented** (DNA import). Phases 2–4 are tracked as
child issues; this document records what the spike verified and the gaps that
shape them.

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
| Coordinates | converted by the library to right-handed, Y up, character facing +Z (`x=left y=up z=front` in DNA naming — a 180° turn about Y from MetaHuman's authored `x=right y=up z=back`, which faces −Z) |
| Euler → quaternion | Maya rotate order: first letter applied first (`xyz` ⇒ X, then Y, then Z; column-vector product Rz·Ry·Rx) — equal to OpenRigLogic's row-vector `Rx*Ry*Rz`. The first cut multiplied the other way round and produced a scrambled skeleton |
| Neutral joint space | **PARENT-RELATIVE** (verified on Epic's real DNA; `NeutralSpace::World` is only for producers that write world transforms). The root joint carries a −90° X orientation that turns the data's Z-up joint axes into Y-up |
| UVs | V flipped (DNA bottom-left → Ogre/glTF top-left) |
| Faces | n-gons fan-triangulated; invalid indices dropped and reported, never dereferenced |
| LODs | one LOD imported (default 0); meshes that exist only at other LODs are reported |
| Names | blend-shape channel names become morph-target names; mesh names become submesh names |

## Verified on Epic's own archetype DNA (UE 5.7 `SKM_Body.dna` / `SKM_Face.dna`)

Not redistributed — read from a local Unreal install. Both import; the head sits directly
above the body in the shared space, the character faces +Z after the coordinate conversion,
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
2. **No rig evaluation yet:** Phase 1 imports the neutral pose only. Corrective
   joints/blend shapes driven by RigLogic are Phase 2.
3. **Reference validation:** joint space, Euler order and coordinate conversion are now checked against Epic's real DNA (vertex↔dominant-joint distance), but not yet against RigLogic's own evaluated output; Phase 2 must compare posed frames with a trusted reference. The repo ships no distributable MetaHuman, so this needs a user-supplied asset.
4. **Textures/materials:** DNA contains no textures. The DCC export ships them
   separately; the importer assigns `BaseWhite`. Texture + material binding is
   a follow-up.
5. **Groom and clothing** are not in the DNA/DCC package. They are separate
   pipeline inputs (import as ordinary meshes and bind to the DNA skeleton);
   not handled automatically.
6. **JSON DNA** is not read (binary only).
7. **Export:** there is deliberately no DNA writer surface. DNA serialisation
   is not rig recalibration; round-tripping must be validated before any
   export is exposed (Phase 4).
8. **LOD propagation / lower-LOD skeleton pruning:** the full skeleton is
   imported at every LOD; `Rig::lodJointIndices` records the per-LOD subset.

## Phases still open

2. Body correctives (RigLogic evaluation + skinning-vs-corrected toggle).
3. Facial controls, ARKit/Audio2Face mapping, baking.
4. Body mesh fitting and generic RBF correctives.
Follow-up (separate issue): image-based body prediction.

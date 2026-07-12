# Mesh part segmentation — model & data strategy (#410 follow-up)

Status: humanoid+animal v2 model shipped (2026-07). This doc records why the
v1 model failed, what v2 does about it, and the proposed roadmap for
category-aware segmentation (humanoids → animals → vegetation / vehicles /
buildings).

## Why the v1 model failed on real meshes

The v1 `meshseg.onnx` scored **31.5% per-vertex accuracy** against exact
rig-derived ground truth on the repo's rigged test characters (near-random for
limbs; a chibi-proportioned character collapsed almost entirely to "torso").
Root causes, all confirmed empirically:

1. **Volume vs surface.** v1 sampled points *inside* primitive volumes; real
   mesh vertices live *on surfaces*. This was the largest domain gap.
2. **Proportion coverage.** v1 heads were 7–18% of body height. Real stylised
   characters (chibi) have heads up to ~50% of height with huge ears; those
   meshes got labelled almost entirely torso.
3. **Vertex-density mismatch.** Real characters put 30–50% of vertices in the
   head/face (denser modelling); v1 sampled parts uniformly by count.
4. **Flipped handedness.** v1 placed the RIGHT arm at +X, but the rig-prior
   convention (character facing +Z) puts LEFT limbs at +X — the
   training data systematically swapped left/right.
5. **Train/inference point-count mismatch.** Trained at 1024 points, but
   `MeshSegmenter::predict()` samples 4096 (`Options::samplePoints`), changing
   kNN neighbourhood density under the model.
6. **Disconnected primitives.** v1 bodies had floating parts with gaps; real
   bodies are connected surfaces.

## What v2 does (shipped)

`scripts/export-meshseg-onnx.py` v2 (single script, same CLI) rebuilds the
whole data story:

- **Synthetic bodies**: surface-sampled, connected part layouts in three body
  plans — humanoid (normal / chibi / lanky regimes, ears/muzzle head bumps,
  posed arms, feet pointing +Z as a facing cue), quadruped (all four legs
  labelled by SIDE, tail→torso, matching `partForBoneName`'s rig-prior
  convention), and biped-with-tail (dino). Per-part point density is
  randomised (heads up to ~5× denser). Correct handedness (LEFT at +X).
- **Mined real rigs** (CC0 Quaternius packs, ~45 files after quality gating;
  see `training_rigs/SOURCES.md` ledger): mined via
  `qtmesh segment --dump-training-data`. Because FBX bind poses arrive in
  arbitrary frames (Z-up, side-facing, head-down), the loader
  **canonicalises each cloud from its own labels** (up = legs→torso with a
  per-limb-PCA candidate scored against "legs end up below the body";
  left = right→left limb axis; forward = left×up with a 180° fix when the
  head protrudes backward) and **reassigns arm/leg sides geometrically**
  (the miner's bone-name side detection was up to ~30% wrong on some rigs).
  Files that don't canonicalise coherently are excluded rather than mixed in.
- **Model**: PointNet++-style with TWO kNN aggregation blocks (~1 MB), CE loss
  with `ignore_index=0` (unknown masked) + inverse-sqrt class weights.
- **Schedule**: phase 1 on random 2048-point subsets (fast; subsetting is free
  augmentation), phase 2 fine-tune at the app's inference size of 4096.
- **Eval**: exact rig-derived labels from the repo's rigged demo characters
  (held out of training entirely — out-of-distribution for the model) via
  `.meshseg_work/eval_model.py`-style harness + held-out CC0 rigs.

### v2 results (July 2026, replicating the app's inference path)

| eval set | v1 model | v2 model |
|---|---|---|
| rig-truth, 3 out-of-distribution characters, per-vertex overall | **31.5%** | **94.7%** |
| — head / torso recall | 0.28 / 0.93 | 0.99 / 0.80 |
| — L-arm / R-arm recall | 0.18 / 0.11 | 0.99 / 1.00 |
| — L-leg / R-leg recall | 0.06 / 0.04 | 0.87 / 0.96 |
| held-out CC0 rigs (Male_Suit, Female_Dress, Sheep, Velociraptor) | — | 97.0% |
| held-out synthetic bodies | — | 95.6% |

Torso is the weakest class by construction: the ground truth comes from
skinning weights, whose shoulder/hip boundaries don't coincide exactly with
any geometric boundary. Left/right confusion — v1's dominant failure — is
essentially gone.

End-to-end through the 3.14.0-dev `qtmesh segment` binary (ONNX build,
including the topology smoothing pass): a RIGGED character reproduces the
rig-truth part counts almost exactly (rig-prior hints), and the same
character exported to unrigged OBJ — the pure model path — yields
head 2690 / torso 583 / L-arm 809 / R-arm 768 / L-leg 492 / R-leg 486
against rig-truth 2767 / 933 / 652 / 661 / 443 / 430 (boundary bleed at the
shoulders/hips, correct lateralisation everywhere).

## Category strategy (ADOPTED — #818 Track B2, implemented 2026-07)

The pre-B2 C++ contract was one 7-class body-centric label set
(`MeshSegmenter::Part`). That covers **humanoids** and stretches to
**quadrupeds/birds/dinos** ("front-left leg" → left_leg, tail/wing handled by
the rig-prior name map). It cannot express vegetation/vehicle/building parts.

Adopted architecture (option 1 below; option 2 stays rejected):

1. **One label vocabulary, several category models** (SHIPPED).
   A single flat label enum partitioned by category, one small ONNX per
   category (`meshseg.onnx` = body, `meshseg_vegetation.onnx`,
   `meshseg_vehicle.onnx`, `meshseg_building.onnx`). A tiny shared
   point-cloud *category classifier* (`meshseg_category.onnx`, PointNet
   max-pool → 4-way softmax, trained on generator-provenance labels) — or an
   explicit `--category` CLI/MCP argument — dispatches. Small per-category
   models train independently, fail independently, and download lazily — same
   `ensureModelBlocking` pattern, one file each. C++: `Part` extended past the
   body labels (`trunk…flower`, `vehicle_body…rotor`, `wall…foundation`;
   `window` is ONE global label shared by vehicle + building),
   `Options::category {Auto, Body, Vegetation, Vehicle, Building}`,
   per-category channel→Part maps (`categoryChannelMap`), and
   `resolveCategoryBlocking()` for the Auto path (classifier unavailable →
   Body, the pre-B2 behaviour). The geometric fallback is category-aware
   (vegetation: foliage/trunk up-band; vehicle: wheel/body; building:
   roof/wall).

2. *Rejected: one big multi-category model with a unified softmax.* Label
   imbalance across categories, one bad category degrades all, and every
   category addition forces a full retrain + re-download for everyone.

**Why the classifier and not SmolVLM as the Auto dispatcher:** the shipped
`ImageCaptioner` (SmolVLM-500M) was considered for "identify what this mesh
is". As the *dispatcher* it loses on every axis: it needs an offscreen GL
render + a ~500 MB model + `ENABLE_LOCAL_LLM`, and free-text output needs
fragile keyword mapping — while the point-cloud classifier is ~100 KB,
headless-CLI-safe, deterministic, and its training labels are free
(generator provenance: synthetic bodies/trees/vehicles/buildings + mined
rigs as `body`). SmolVLM remains the right tool for the OPTIONAL GUI
"identify/name this mesh (or part)" assist — a follow-up slice that renders
the viewport/part crops and suggests names; it composes with, not replaces,
the category dispatch.

Shipped label sets (channel order in scripts/export-meshseg-onnx.py; global
Part mapping in src/MeshSegmenter.cpp kCategoryChannelMaps):

- **Body (v2 + #788 retrain)**: head, torso, L/R arm, L/R leg. Future minor
  additions: `tail`, `wing` (currently folded into torso/arm by the rig-prior
  map — keep folding until a consumer needs them separated).
- **Vegetation (v1, synthetic-only)**: trunk, branch, foliage, root,
  flower(/fruit). Data: procedural trees (broadleaf/pine/palm/dead/bush
  regimes with canopy-vs-per-tip blobs, surface roots, coconuts) — exact
  labels, fully ours. Follow-up: mine CC0 nature packs (UNRIGGED, so labels
  come from connected-component + material/name heuristics, spot-checked).
- **Vehicle (v1, synthetic-only)**: vehicle_body, wheel, window, wing,
  rotor(/prop). Data: parametric cars/trucks (cabin+panes+wheels), planes
  (fuselage/wings/tail/prop/gear), helicopters (body/boom/rotors/skids).
  Follow-up: name-mined CC0 vehicle packs ("Wheel_FL", "glass" submesh names
  are strong exact labels).
- **Building (v1, synthetic-only)**: wall, roof, window, door, chimney,
  foundation. Data: parametric houses/towers/huts (gable/pyramid/flat roofs,
  window rows, door, chimney, foundation slab). Follow-up: name-mined CC0
  kits (Kenney castle kit etc.).

Key insight for non-rigged categories: **submesh + material + node names play
the role bone weights play for bodies** — CC0 packs are consistently named
(`Wheel_FL`, `Roof`, `Window_01`), so a name→label map plus
connected-component splitting yields exact labels at mining time, no manual
annotation. The `--dump-training-data` schema stays the same
(`qtmesh-meshseg-training-v1`: points + labels), so the training script needs
no changes per category — only a new miner path and a label-set table.

### v1 category-model results (July 2026, held-out synthetic val)

| model | val acc | notes |
|---|---|---|
| meshseg_vegetation.onnx | 93.8% | broadleaf/pine/palm/dead/bush regimes |
| meshseg_vehicle.onnx | 93.5% | car/truck/plane/helicopter |
| meshseg_building.onnx | 86.9% | hardest — many small parts (windows, chimneys) |
| meshseg_category.onnx | 99.1% | 4-way Auto dispatcher (incl. mined real bodies) |

End-to-end through `qtmesh segment` (ONNX build): the Auto path correctly
classified procedural tree/car/house/human test meshes and produced sane
part splits on each (some boundary bleed on low-detail primitives — the
models train on dense 4096-point surface clouds, so very low-poly meshes
lean on the duplicate-padding path and degrade; real assets are fine).
All four + the body model are hosted on the aggregate HF repo (`segment/`)
and on dedicated standalone repos
(`QtMeshEditor-mesh-segmentation-{vegetation,vehicle,building,category}`).

### Implementation status

1. (done, v2) Fix humanoids + basic quadrupeds with the existing 7-class model.
2. (done, #818 B2) Category classifier + `Options::category` plumbing (CLI
   `--category`, MCP `category` arg; `Auto` = classifier; GUI Select-by-Part
   resolves Auto transparently).
3. (done, v1 synthetic) Vegetation model.
4. (done, v1 synthetic) Vehicle model.
5. (done, v1 synthetic) Building model.
6. (follow-ups) Name-mined CC0 packs for 3–5; body label additions
   (tail/wing) only when a feature consumes them; SmolVLM "identify/name"
   GUI assist; SAM-2 multiview zero-shot path (`sam2seg`) for arbitrary
   kitbashed meshes where a fixed label set can't win (#818 B2 item 2).

## Continual improvement loop (unchanged)

Every rigged asset a user runs `--dump-training-data` on is one free exact
sample; `export-meshseg-onnx.py --real-data` mixes mined JSONs into training.
The canonicaliser makes previously unusable (arbitrarily-oriented) mined files
usable, so the corpus grows with zero manual labelling. Only CC0/CC-BY
sources may be mined for the SHIPPED model (ledger in
`training_rigs/SOURCES.md`).

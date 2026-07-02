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

## Category strategy (proposed roadmap)

The current C++ contract is one 7-class body-centric label set
(`MeshSegmenter::Part`). That already covers **humanoids** and stretches to
**quadrupeds/birds/dinos** ("front-left leg" → left_leg, tail/wing handled by
the rig-prior name map). It cannot express vegetation/vehicle/building parts.

Recommended architecture, in order of preference:

1. **One label vocabulary, several category models** (recommended).
   Keep a single flat label enum but partition it by category, and ship one
   small ONNX per category (`meshseg.onnx` = body, `meshseg_vegetation.onnx`,
   `meshseg_vehicle.onnx`, `meshseg_building.onnx`). A tiny shared
   point-cloud *category classifier* (or an explicit `--category` CLI/MCP
   argument, Inspector dropdown) dispatches. Small per-category models train
   independently, fail independently, and download lazily — same
   `ensureModelBlocking` pattern, one file each. C++ change: extend `Part`
   past `Count` with category-scoped labels and add
   `Options::category {Auto, Body, Vegetation, Vehicle, Building}`.

2. *Rejected: one big multi-category model with a unified softmax.* Label
   imbalance across categories, one bad category degrades all, and every
   category addition forces a full retrain + re-download for everyone.

Proposed label sets (all mineable or synthesisable permissively):

- **Body (shipped)**: head, torso, L/R arm, L/R leg. Future minor additions:
  `tail`, `wing` (currently folded into torso/arm by the rig-prior map — keep
  folding until a consumer needs them separated).
- **Vegetation**: trunk, branches, foliage, roots, flower/fruit. Data:
  synthetic L-system trees (trivially exact labels, fully ours) + CC0 packs
  (Quaternius/Kenney nature packs are UNRIGGED, so labels come from
  connected-component + material/name heuristics, manually spot-checked).
- **Vehicle**: body/hull, wheels/tracks, windows/canopy, wings, rotor/prop,
  lights, interior. Data: synthetic parametric cars/planes + CC0 vehicle
  packs with per-material/submesh name mining ("wheel", "glass", …
  submesh/material names are strong exact labels in CC0 packs).
- **Building**: walls, roof, windows, doors, chimney, base/foundation,
  fence/railing. Data: synthetic parametric houses + CC0 building kits
  (Kenney castle kit etc.), again submesh/material-name mining.

Key insight for non-rigged categories: **submesh + material + node names play
the role bone weights play for bodies** — CC0 packs are consistently named
(`Wheel_FL`, `Roof`, `Window_01`), so a name→label map plus
connected-component splitting yields exact labels at mining time, no manual
annotation. The `--dump-training-data` schema stays the same
(`qtmesh-meshseg-training-v1`: points + labels), so the training script needs
no changes per category — only a new miner path and a label-set table.

### Suggested implementation order

1. (done, v2) Fix humanoids + basic quadrupeds with the existing 7-class model.
2. Category classifier + `Options::category` plumbing (CLI `--category`,
   MCP arg, Inspector dropdown; `Auto` = classifier).
3. Vegetation model (synthetic L-systems are nearly free and exact).
4. Vehicle model (name-mined CC0 packs + parametric synthetics).
5. Building model (name-mined CC0 kits + parametric synthetics).
6. Body label additions (tail/wing) only when a feature consumes them.

## Continual improvement loop (unchanged)

Every rigged asset a user runs `--dump-training-data` on is one free exact
sample; `export-meshseg-onnx.py --real-data` mixes mined JSONs into training.
The canonicaliser makes previously unusable (arbitrarily-oriented) mined files
usable, so the corpus grows with zero manual labelling. Only CC0/CC-BY
sources may be mined for the SHIPPED model (ledger in
`training_rigs/SOURCES.md`).

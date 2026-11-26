# Generic part segmentation (design)

Status: **slices 1-2 implemented** (pure-data core in `src/GenericSeg/`, tested with a fake mask provider; SAM 2.1 export + C++ mask provider, measured on real meshes); hosting and integration pending (#1179). Proposed new `MeshSegmenter::Category::Generic`.

## Problem

`MeshSegmenter` returns **named** parts for four categories (body, vegetation,
vehicle, building). Everything else (furniture, props, weapons, machines,
creatures outside the humanoid rig prior) currently gets forced into one of
those vocabularies, or into the geometric fallback, which only knows the body
layout. Users want to split *any* object into its parts: to select a part,
split / explode it, re-material it, or hand a mask to a completion model
(HoloPart-style "pull the sword out of the stone").

Goal: a class-agnostic segmenter that returns **unnamed part instances**
(`part_01`, `part_02`, …) for arbitrary meshes, offline, on CPU, in the
editor, with the same cleanup, PartOps and CLI/MCP surfaces as today.

Non-goals (first version): semantic names, hierarchical part trees, per-part
completion, beating the named category models on their own categories.

## Approach: own implementation of the SAMesh method

SAMesh (Tang et al., "Segment Any Mesh", arXiv 2408.13679) is training-free:

1. render the mesh from many views (surface normals + shape-diameter values),
2. run Segment Anything on each render to get 2D masks,
3. lift the masks onto the mesh through a face-id render,
4. merge masks that cover the same faces across views (graph community
   detection), then vote a label per face and clean up.

Its reported quality matches or beats the classic Shape Diameter Function
segmenter on Princeton / CoSeg and was preferred in a user study on generated
meshes. It needs no per-object training (unlike SAMPart3D) and no CUDA.

### Licence constraints (checked 2026-11-25)

| Piece | Licence | Use |
|---|---|---|
| SAMesh code (gtangg12/samesh) | **none** (default copyright) | **Not used.** We implement the method from the paper. |
| SAM 2 / 2.1 code + checkpoints | Apache-2.0 | ✅ export to ONNX, host on our HF repo, credit Meta in `THIRD_PARTY_AI_MODELS.md` |
| SA-1B dataset | research licence | not used (we only use the released weights) |
| igraph (SAMesh's Leiden) | GPL-2 | **Not vendored.** Own community detection (below). |
| Boykov-Kolmogorov max-flow (alpha-expansion refine) | GPL / research | **Not used.** Our existing smoothing passes instead. |
| PartField (NVIDIA) | non-commercial | rejected |

## Pipeline

```
mesh (positions, indices)
 └─ normalise (centre, unit diagonal), weld for adjacency
 ├─ per-face attributes: normal, Shape Diameter Function (SDF) value
 ├─ N views on an icosahedral sphere
 │    ├─ software raster: face-id buffer + depth
 │    ├─ shaded images: normal render, SDF render (colour-mapped)
 │    └─ SAM 2.1 automatic masks  (encoder once per image, decoder on a point grid)
 ├─ lift: each mask → set of visible face ids (+ pixel counts)
 ├─ merge graph: node = (view, image kind, mask); edge = face-overlap ratio
 ├─ community detection → part instances
 ├─ face vote (score-weighted), fill unseen faces by graph flood
 └─ existing cleanup: smoothLabelBoundaries → cleanupLabelIslands → vertexLabelsFromFaces
```

### 1. Rendering — CPU, pure data

Do the rendering **in software**, not through Ogre:

- Headless-testable with no GL context (the CI Linux lane, macOS unit tests).
- Exact face ids (no colour-encoding / MSAA / filtering traps).
- Same input regardless of the entity's material, textures or RTSS state.

`GenericSeg::Raster` (new, pure): orthographic camera framing the normalised
mesh's bounding sphere, scanline triangle raster with a z-buffer, outputs
`faceId[w*h]` (-1 = background), `depth`, interpolated normal. Default 512 px
(SAM's encoder input is 1024; upscaling a 512 render is fine and halves raster
time — measure both).

Views: 20 (icosahedron face centres) by default; option 12 (vertices) for
speed. Up axis does not matter (no semantic prior).

Image kinds (SAMesh's best setting is normals + SDF, both untextured):

- **normal**: world-space normal → RGB.
- **SDF**: per-face shape-diameter value, normalised over the mesh, through a
  fixed colour ramp. Captures thickness, which separates thin parts (blade,
  handle, leg) from thick ones even where normals are continuous.
- **matte** (optional): Lambert with a head light. SAMesh notes low-poly meshes
  give poor normal/SDF images; matte is their fallback for those. Auto-select
  when the mesh has < ~2k faces.

### 2. Shape Diameter Function

Per face: cast K = 30 rays inside a 120° cone around the inverted normal, keep
hits whose hit-normal faces the ray (inside wall), take the median-filtered
mean distance (Shapira et al. 2008). Needs a ray-triangle accelerator: add a
small pure-data BVH (`GenericSeg::TriangleBvh`, SAH binned build). There is no
shared BVH in the tree today (`EditModeController::rayTriangleIntersect` is a
brute-force test); this one should be reusable by other code. Smooth SDF over
the face graph (2–3 Laplacian passes) before colour-mapping.

Cost: 30 rays × faces. For 200k faces ≈ 6M ray casts, a few seconds on CPU,
parallel with `QtConcurrent`. Above a face budget (default 300k), compute SDF on
a meshoptimizer-simplified copy and transfer by closest point.

### 3. SAM 2.1 automatic masks (ONNX)

Two graphs, exported by a new `scripts/export-sam2-onnx.py` (offline dev tool,
not shipped):

- `sam2.1_hiera_small_encoder.onnx`: image `[1,3,1024,1024]` → image
  embeddings + the two high-resolution feature maps the decoder needs.
- `sam2.1_hiera_small_decoder.onnx`: embeddings + point prompts (batched) →
  masks `[B,3,256,256]` + predicted IoU.

Small vs tiny: start with **small** (better masks), measure tiny; the tier is an
option like TripoSR's.

Automatic mask generation, reimplemented in C++ (`GenericSeg::SamMaskProvider`
+ the pure `Sam::` helpers in `SamMasks.{h,cpp}`): 16×16 point grid restricted
to foreground cells (32×32 found the same parts on the test meshes at 2.5× the
time), decoder in batches of 64, all 3 multimask outputs per point kept when
predicted IoU ≥ 0.7 and stability ≥ 0.85 (offset 1.0), mask NMS at IoU 0.7 on
the 256² masks (box pre-check), upsample bilinearly (align_corners = false) to
the raster, drop masks below 0.1 % of the foreground.

**Measured export** (`scripts/export-sam2-onnx.py`, opset 17, TorchScript
exporter): encoder 138 MB, decoder 21 MB (dynamic batch and point count);
parity vs the upstream predictor on three mesh renders: encoder max diff
1.5e-5, mask IoU ≥ 0.9995, IoU-score diff < 2e-5. The prompt encoder's
indexed assignments are rewritten functionally in the wrapper (the empty-Add
trap).

**Smallest mask wins** (`Options::smallestMaskWins`, default on): SAM returns
the whole object as one of its masks in every view. Those 40 masks form one
community covering every patch and outvoted every part: the car, a horse and a
rat came out as ONE part. As in SAMesh, each image's masks are painted largest
first so a pixel keeps only the smallest mask covering it; after that the car's
four wheels, the rat's ears, paws and tail and the horse's tail separate.

Hosting: HF `QtMeshEditor-models/segment/` with SHA-256 pinned in code and
verified on disk (the `ModelFetch` `expectedSha256` path), licence text beside
the graphs. Overrides: the existing `QTMESH_SEGMENT_MODEL_BASE_URL` /
`ai/segmentModelBaseUrl`, guard `QTMESH_SEGMENT_NO_DOWNLOAD`.

Export must pass the usual gate before hosting: loads in onnxruntime, finite
outputs, parity with PyTorch on fixed images (mask IoU ≥ 0.99, IoU-score diff
< 1e-3), and a fixed-shape check (the UniRig lesson: frozen traced constants).

### 4. Lifting and merging (as implemented)

**Surface patches, not faces.** Masks are lifted onto patches: faces grouped
by a 3D grid of their centroids, about 2 pixels wide (`Options::patchPixels`).
Lifting onto single faces failed in the first tests: on a dense mesh many
triangles are smaller than a pixel, so each view keeps a different random
subset of them, and masks of the same part from neighbouring views barely
share faces (thin table legs split in two). Boundary precision is therefore
about one patch (~0.4 % of the object's size at 512 px) before the boundary
smoothing pass.

A patch counts as seen from a view with >= 2 pixels; a mask covers it when it
holds >= 30 % of those pixels.

**Edges compare masks only where both views look, by IoU.** For masks a, b
from different images: `IoU = shared / (area(a seen from b's view) + area(b
seen from a's view) - shared)`, edge when IoU >= 0.6 and the common region is
>= 10 % of the smaller mask (nearly opposite views carry no evidence). Two
earlier measures failed the tests:

- `shared / min(area)` over all faces: masks of one part from different
  views cover different faces, so true pairs scored low (graph too sparse).
- containment on the common region: a mask spanning two parts scores 1
  against each, gluing them together. With IoU it scores about the share of
  the part it shares (~0.5 for two equal halves) and stays out.

Edge weight = IoU x the lower mask score.

**Community detection: Leiden with the Constant Potts Model** (own
implementation, `GenericSeg/Leiden.*`; modularity also available). Modularity
has a resolution limit: on the table test it cut each leg's ring of masks in
half. CPM keeps a group when its edge density beats gamma, independent of the
graph size; gamma is the granularity knob (0.02 / 0.05 / 0.15).

### 5. Face labels

Each patch votes `score x coverage` for the community of every mask covering
it, a mask that holds at least half of the patch weighted by
`(median mask area / mask area)^0.5` (`voteAreaExponent`): the cross-view
version of smallest-wins. In views where SAM gave no mask for a wheel, the
whole-car mask covers it, and without this weight those votes tie with the
wheel's own. Gating the weight on majority coverage keeps a small mask's
silhouette edge from stealing the neighbouring patches. Faces take their
patch's label; faces no view saw take the label of the nearest labelled face
by face-graph BFS.

**Graph-cut refinement** (`Options::graphCut`, default on; `GraphCut.{h,cpp}`,
alpha-expansion with an exact Dinic max-flow per move). Energy per face:
area x -log of the smoothed vote share (`voteSmoothing` pseudo-votes per
label, so a patch seen by one view is weak evidence). Per mesh edge between
two parts: `smoothness` x length, down to 2 % of that along concave creases
sharper than `creaseAngleDegrees`. Plus **through links**: a ray straight
inward from each face to the wall it exits, weighted
`throughWeight x area x exp(-distance / throughThickness)`, so the two sides
of a thin part pay to differ. That last term fixed the worst defect of the
plain vote: the views that see a thin part's underside are not the ones that
see its top, so their masks never link and the underside came out as another
part (the rat tail's underside took the body's label; the back of a
character's hand was its own part). Faces still unknown after the flood (floating pieces no view saw) stay
unknown. Each part's expansion move is solved only around the faces that voted
for it, plus two hops along surface and through links (which reach a thin
part's other side): whole-mesh moves took 12-24 s on 25k-face props with 30-60
parts, local ones take 0.2-1 s for the whole refinement with the same quality.
Weights tuned with cached masks: smoothness 0.2 already absorbs a character's
legs into the torso, 0.1 keeps the eyes; a thickness of 0.04 left a shirt's
front and back as two parts, 0.15 joins them. Then: split each label into connected
components (two wheels = two parts), merge parts below `minPartFraction` of
the surface into the neighbour with the longest shared border, boundary
smoothing + island cleanup (`MeshSegmenter`), split/merge again, renumber by
area (`part_01` = largest).

**Shape diameter note:** Shapira's estimator averages slanted rays in a 120°
cone, so a flat slab reads about 1/mean(cos θ) ≈ 1.33x its thickness. It is
only used relatively (normalised for the SDF image), so the paper's estimator
is kept; the test pins proportionality and the bias band.

**Measured (slice 1, no model):** 97,200-triangle mesh, 20 views x 2 image
kinds at 512 px: 0.42 s total (SDF 0.28 s, raster + lifting 0.08 s, graph
0.02 s, labels 0.03 s). Debug trace: `QTMESH_GENSEG_DEBUG=1`.

### 6. Granularity

One user-facing control, **Coarse / Medium / Fine**, mapping to the Leiden
resolution, the minimum part area (3 % / 1 % / 0.3 % of the surface) and the
SAM grid density. Exposed as `--granularity` / MCP `granularity`. A continuous
value can come later if the presets are not enough.

## Label space (the one API change)

The named categories share the global `MeshSegmenter::Part` enum. Generic parts
are instances, so they cannot live in that enum.

- `Result` gains `std::vector<QString> labelNames` (index = label value;
  empty for the named categories). Generic labels are `1..K`, `0` = unknown.
- New `QString Result::labelName(int)`: `labelNames[i]` when present, else
  `partName(i)`.
- The 12 `MeshSegmenter::partName(int)` call sites outside the segmenter
  (CLIPipeline 3, EditModeController 6, MCPServer 2, SubMeshOps 1) switch to
  the result-aware name. `SubMeshOps::groupFacesByLabel` sets `FaceGroup::name`
  from it, so the names flow on to split/explode from there. (`PartOpsScene`
  has its own local `partName` for submesh names; check it reads the group
  name rather than re-deriving it.)
- `--write-labels` JSON (`qtmesh-partops-labels-v1`) gains a `labelNames`
  array; readers that ignore it keep working. Bump to `-v2` only if a field
  changes meaning.

`Category::Auto` stays as it is (classifier → body/vegetation/vehicle/building)
in the first version. Routing low-confidence classifier results to Generic is a
later decision, measured on the benchmark.

## Surfaces

- CLI: `qtmesh segment <file> --category generic [--granularity coarse|medium|fine] [--views 12|20]` — works with `--json`, `--write-labels`, `--split-parts`, `--explode-parts`, `--solidify`.
- MCP: `segment_mesh {category:"generic", granularity}`; `split_mesh_by_segments` / `explode_mesh_parts` accept the same.
- GUI: "Generic (any object)" in the category picker of the segmentation and Split into Parts sections; granularity segmented control; progress (render → masks per view → merge) and Cancel through the existing worker-thread pattern (`toolProgress` for MCP).
- Sentry: `ai.assist.segment` with `category=generic`, view count, mask count, part count, time.
- AI agent: `segmentation` capability doc lists the generic category ("any object, unnamed parts").

## Performance (measured, M-series CPU, 10-12k faces, 20 views × normal + SDF, 512 px)

| Stage | Time |
|---|---|
| SDF + raster + lift + merge + cleanup | < 0.5 s |
| Graph-cut refinement | 0.2-1 s |
| SAM encoder (40 images, small) | ~24 s ← dominant |
| SAM decoder (16×16 grid, ~1,700-3,600 prompts) | 20-48 s |
| Total | 44-74 s |

Ideas, untested: 12 views, normals-only for Coarse, the tiny tier, CoreML
for the encoder, fewer prompts on views that add nothing new.

## Slices

1. **Core, no model**: raster, SDF + BVH, Leiden, lifting, face vote, cleanup,
   with the fake mask provider. All pure + tested.
2. **SAM 2.1 export + C++ mask generator**: export script with parity gate,
   hosting, `ModelFetch` with pinned digests.
3. **Integration**: `Category::Generic`, `labelNames` plumbing, CLI/MCP/GUI,
   breadcrumbs, docs (this file → user section in `docs/PART_OPS.md`).
4. **Benchmark + tuning**: thresholds, view count, tier, granularity presets.
5. Later: naming parts with the local vision-language model (render each part
   highlighted, ask for a noun); hierarchical tree from several resolutions;
   Auto → Generic routing; HoloPart completion fed by these masks.

## Open questions

- Evaluation meshes: our own TRELLIS.2 game assets (characters, props,
  vehicles from the QtMesh games) plus a rat, a car and a horse; no Mixamo
  assets. SAM 2.1 *small* on untextured normal/SDF renders, with the
  specificity weight and the graph cut, at Fine: a school desk splits into top,
  seat, back, each leg and bar; a cleaning cart into body, bottles, mop,
  handle, bags, wheels; a turret into barrel, housings, legs, feet; a police
  officer into body, hat, arms, shoes, belt, pockets; an orc archer into body,
  arms, shoulder pad, loincloth, feet (head not separated); a car into body + 4
  wheels; a rat into body, ears, paws, tail; a horse into body, head, tail,
  hooves and two lower legs. Merging the remaining horse legs and separating
  heads is slice 4 (*base+*, more prompts).
- Can the encoder run on CoreML for these shapes? (Several of our ONNX graphs
  fall back to CPU; measure before promising speed.)
- Textured assets: should a textured render be a third image kind? Texture
  boundaries often are part boundaries (and sometimes are not: decals, seams).
  Off by default; measure on the benchmark.

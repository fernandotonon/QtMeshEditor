# Generic part segmentation (design)

Status: **design, not implemented.** Proposed new `MeshSegmenter::Category::Generic`.

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

Automatic mask generation, reimplemented in C++ (`GenericSeg::MaskGenerator`):
32×32 point grid restricted to foreground pixels (faceId ≥ 0), decoder in
batches of 64, keep the best of the 3 multimask outputs per point, filter by
predicted IoU ≥ 0.7 (SAMesh lowers SAM's default; tune) and stability score,
mask NMS (IoU 0.7), then drop masks smaller than 0.1 % of the foreground.

Hosting: HF `QtMeshEditor-models/segment/` with SHA-256 pinned in code and
verified on disk (the `ModelFetch` `expectedSha256` path), licence text beside
the graphs. Overrides: the existing `QTMESH_SEGMENT_MODEL_BASE_URL` /
`ai/segmentModelBaseUrl`, guard `QTMESH_SEGMENT_NO_DOWNLOAD`.

Export must pass the usual gate before hosting: loads in onnxruntime, finite
outputs, parity with PyTorch on fixed images (mask IoU ≥ 0.99, IoU-score diff
< 1e-3), and a fixed-shape check (the UniRig lesson: frozen traced constants).

### 4. Lifting and merging

For each mask m in view v: `F(m)` = faces whose pixels fall inside the mask,
with per-face pixel counts; drop faces with < 2 pixels or < 30 % of their
visible pixels in the mask (mask edges straddle faces).

Graph nodes = masks. Edge between masks a, b (different views) when
`|F(a) ∩ F(b)| / min(|F(a)|, |F(b)|) ≥ τ` (default 0.5), weight = that ratio
(pixel-count weighted). Masks from the same view never connect directly (they
are already SAM's decision about that view).

Community detection: own **Leiden** implementation (`GenericSeg::Leiden`,
pure, modularity with a resolution parameter; Louvain + Leiden's refinement
step, deterministic seed). igraph is GPL, so it is not used. Resolution is the
granularity knob.

### 5. Face labels

Each community c → part instance. Face score `s(f, c) = Σ` over masks in c that
contain f of `mask_score × pixel_fraction`. Label = argmax; faces never seen
from any view (cavities, interiors) get the label of the nearest labelled face
by face-graph BFS (geodesic). Then split each label into connected components
(two disconnected wheels are two parts), then the existing cleanup:
`smoothLabelBoundaries`, `cleanupLabelIslands`, `vertexLabelsFromFaces`.
`levelLimbCuts` / `planarRecut` do not apply (body-specific).

Finally renumber parts by descending surface area: `part_01` is the largest.

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

## Performance budget (estimates, to be measured)

| Stage | 20 views, 100k faces, M-series CPU |
|---|---|
| SDF | 2–4 s |
| Raster (2 kinds × 20) | < 1 s |
| SAM encoder (40 images, small) | ~40–80 s ← dominant |
| SAM decoder (grid points × 40) | ~10–20 s |
| Lift + merge + cleanup | < 2 s |

If the encoder dominates as expected: default to 12 views × 2 kinds, or
normals-only for Coarse, and try the tiny tier. Target: under a minute for
Medium.

## Testing

Pure data, no ONNX, no GL:

- `GenericSegRaster_test`: face ids on a known mesh at known pixels; depth
  ordering; background = -1.
- `GenericSegSdf_test`: SDF of a capsule ≈ its diameter; thin plate < thick
  block.
- `GenericSegLeiden_test`: planted-partition graphs recovered; determinism.
- `GenericSegLift_test`: synthetic "table" (top + 4 legs) with a **fake mask
  provider** (masks = the rendered faces of each true part, plus noise and
  split/merged masks) → 5 parts recovered. The mask generator is an interface,
  so the whole pipeline after SAM is testable without the model.

ONNX-gated: encoder/decoder contract and a fixed-image parity check
(`QTMESH_SEGMENT_NO_DOWNLOAD` keeps CI offline; skip with `SUCCEED()` + note,
not `GTEST_SKIP`).

Benchmark (not CI): class-agnostic mIoU on a fixed set. PartObjaverse-Tiny
(200 objects, instance labels) if its licence and the underlying Objaverse
asset licences allow it; otherwise a CC0 set we label ourselves. Compare
against the current geometric fallback and, on body/vehicle meshes, against the
named category models (generic should not be the default where those win).

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

- Is SAM 2.1 *small* good enough on untextured normal/SDF renders, or do we
  need *base+*? (SAMesh used SAM2; it does not say which size.)
- Can the encoder run on CoreML for these shapes? (Several of our ONNX graphs
  fall back to CPU; measure before promising speed.)
- Textured assets: should a textured render be a third image kind? Texture
  boundaries often are part boundaries (and sometimes are not: decals, seams).
  Off by default; measure on the benchmark.

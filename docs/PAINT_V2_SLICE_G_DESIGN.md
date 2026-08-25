# Paint v2 Slice G — Cavity / Curvature / AO masks (#550)

Auto-generated per-mesh **derived maps** that drive the classic weathering
workflow: dirt in crevices, wear on edges, shadowing in occluded areas. Each map
can gate the brush or initialise a layer mask, and three one-click recipes wire
up the common cases.

Parent epic: #543. Depends on Slice C (#546, layers).

## The three maps

| Map | Meaning | Stored range | Typical use |
|---|---|---|---|
| **Cavity** | concave only | `0` flat/convex → `1` deep crevice | crevice dirt, grime |
| **Curvature** | signed | `0` convex ← `0.5` flat → `1` concave | edge wear (inverted) |
| **AO** | ambient occlusion | `0` open → `1` fully occluded | weathering, contact shadow |

Cavity and curvature come from the same geometric signal: for each vertex,
average the dot of its normal with the direction to each 1-ring neighbour
(`HalfEdgeMesh::verticesAroundVertex`). Neighbours sitting *above* the tangent
plane mean the surface closes in — concave. Below — a convex ridge.

- **Cavity** keeps only the concave half, so grime never lands on ridges.
- **Curvature** keeps the sign and centres flat at `0.5`, with a `flatTolerance`
  that pins near-flat values to exactly neutral. Without it, tessellation noise
  on nominally flat panels speckles into visible edge wear.

The mesh is welded across submeshes first (via `HalfEdgeMesh`), so a UV seam or
a material split does not read as a crease.

## AO without a ray tracer

The issue specified "short-ray hemispherical occlusion" on the CPU. There is no
BVH, kd-tree or octree anywhere in the repo, and every existing ray query is a
brute-force linear scan — so a real ray AO would have meant adding an
acceleration structure.

Instead AO reuses the **depth-map visibility test** already proven by
`ProjectionPainter::OcclusionMap` (#549): render the mesh's depth from 12 evenly
spread directions (Fibonacci lattice — a naive lat/long grid clusters at the
poles and biases AO vertically), then per vertex count how many of those views
can actually see it. That fraction is the occlusion.

Two behaviours that matter, both pinned by tests:

- **Back-facing views are skipped, not counted as occluding.** A view looking at
  the back of a face cannot tell you how lit the front is; counting it would
  darken every vertex by roughly half regardless of geometry.
- **When no view faces the normal the result is `0`** (unoccluded), not `1`.
  Reporting "fully occluded" for a surface the view set happens not to cover
  would black out whole regions of the map.

The **bias** is `max(2 grayscale steps of the encoded range, 1% of the bounds
radius)`. Below that, depth quantisation alone makes a surface occlude itself
(depth acne). Distance is compared along the **camera axis**, not Euclidean,
because the depth map encodes linear fog distance along that axis.

The visibility *maths* is pure data (a vertex + `DepthView`s → a scalar) and
unit-tested headlessly against synthetic depth images; only the rendering of
those views touches the Ogre scene.

## Rasterisation

Per-vertex scalars are rasterised into UV0 with an edge-function half-space
test, then **seam-dilated** — UV islands need this or bilinear/MIP sampling
bleeds background across the seams.

This is a local float implementation rather than a call into
`VertexColorBaker::rasterizeTriangle`, which the issue suggested reusing. That
one is typed on RGBA8 `ColourValue`, so a scalar map would quantise to 8 bits
and band visibly across a smooth AO gradient. The *discipline* is copied
verbatim, including the explicit coverage vector: a texel whose value happens to
equal the background is otherwise indistinguishable from an unwritten one, so
dilation cannot infer coverage from "differs from background".

## Cache and invalidation

Maps live in `<AppData>/paint/derived_maps/<mesh-hash>/<kind>.bin`, with the
magic+version header and 40-hex-char key validation from `HdrCache` — the key
becomes a path component, so `../` is made *unrepresentable* rather than
sanitised. Writes go to a temp file and are renamed, so an interrupted save
cannot leave a half-written entry that a later load would trust.

**Invalidation is by content hash, not a revision counter.** The issue proposed
invalidating "via `EditableMesh` revision counter", but no such counter exists —
and `EditableMesh` exposes a public mutable `subMeshes()` accessor, so any
counter could be bypassed without incrementing and would not be authoritative.
The SHA-1 already needed for the directory name *is* the invalidation: changed
geometry hashes differently and misses naturally, with nothing to keep in sync.

The hash covers positions, normals, UV0 and indices. It deliberately **excludes**
vertex colour and bone weights, which cannot change any of these maps —
including them would force needless rebakes.

Bump `DerivedMapCache::kFormatVersion` whenever a generator's output would change
for identical input (an algorithm tweak, a different remap curve). The mesh hash
alone cannot notice that.

## Using a map

**Gate the brush** — "Mask the brush" multiplies each dab by the map value at
the painted UV, so a stroke lands only in crevices or only on edges. It scales
the colour's **alpha**, not RGB: scaling RGB would drag paint toward black in
cavities instead of hiding it there.

**Initialise a layer mask** — "Mask active layer" fills the active layer's
`maskAlpha` from the map. Paint freely afterwards; the mask keeps it in the right
places. Sampling is by UV, not 1:1 texels, since a map may be baked at a
different resolution than the paint buffer.

**One-click recipes** — each adds its own masked `Generated` layer as a single
undo step:

| Recipe | Map | Colour | Blend |
|---|---|---|---|
| Edge wear | curvature, inverted | light bare metal | Normal |
| Crevice dirt | cavity | dark grime | Normal |
| AO darken | AO | black | Multiply |

A recipe temporarily switches the active kind to bake its own map and then
**restores the user's picker selection**, so clicking one does not silently
retarget the UI.

## Files

| File | Role |
|---|---|
| `src/DerivedMapGenerator.{h,cpp}` | concavity, remaps, scalar rasterise + dilate (pure data) |
| `src/DerivedMapOcclusion.{h,cpp}` | depth-map visibility → per-vertex AO (pure data) |
| `src/DerivedMapCache.{h,cpp}` | versioned on-disk cache + mesh hashing |
| `src/TexturePaintController.{h,cpp}` | properties, bake orchestration, brush/mask/recipes |
| `qml/PropertiesPanel.qml` | the collapsible "Cavity / Curvature / AO" group |

Breadcrumbs: `paint.derived_map`, `.bake`, `.cache_hit`, `.cache_write_failed`,
`.error`, `.layer_mask`, `.recipe`, `.recalculate`.

## Known limits

- AO renders on the **main thread** (it drives the Ogre RTT), so a bake briefly
  blocks the UI. It is cached, so the cost is paid once per geometry.
- AO quality is bounded by the 12-view count and the 256² depth resolution;
  small contact details can be missed. Increasing either trades bake time.
- Curvature/cavity are per-**vertex** signals, so their detail is bounded by
  mesh density — a low-poly mesh yields broad, soft masks. Sub-vertex detail
  would need a per-texel normal-difference pass.
- The maps are baked in the mesh's own UV0 layout; a mesh with overlapping UVs
  gets overlapping map data, exactly as any other UV-space bake would.

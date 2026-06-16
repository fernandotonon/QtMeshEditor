# Multi-View AI Texture Bake — Design

**Status:** proposed · **Epic:** #397 (AI-assisted authoring) · follows issue #403 (depth-conditioned mesh texture)

## Goal

Generate a full diffuse texture for a mesh by ControlNet-generating images from
several camera views (front + back to start, 4–6 later) and **projection-baking**
them into the mesh's UV0 atlas — replacing today's crude planar bind that "is
unreliable on RTSS-rendered PBR materials whose diffuse TUS is unnamed/numeric"
(CLAUDE.md, issue #403 known limitation).

## Why projection bake, not "UVs from the image"

A generated 2D image has no intrinsic 3D correspondence. The only mapping back
to the mesh is **the camera that produced the depth map** that conditioned the
generation. So we do not derive UVs from the image; we keep the mesh's UV0 fixed
and rasterize image → UV by re-projecting each triangle through that camera. This
is the standard "project from view" / reprojection bake (Blender, Dream
Textures, StableProjectorz).

Decisions taken:
- **Views:** ship front+back; the view list + blend are **data-driven** so 4–6
  views is a config change, not a rewrite.
- **UV handling:** **auto-unwrap when UV0 is missing or low-coverage**, else keep
  existing UVs. This also fixes the #403 unreliability on raw/poorly-unwrapped
  meshes (the bake writes into a real atlas instead of relying on a planar bind).

## Pipeline

```text
selected Entity
  │
  ├─ 0. UV gate: UvUnwrap::infoForEntity → if !hasUv0 || uv0Coverage < THRESH
  │        run UvUnwrap (GUI-safe path), bake against the unwrapped copy.
  │
  ├─ 1. for each view V in viewList:                       (data-driven)
  │        a. depth_V = MeshDepthRenderer::renderDepthMapView(entity, size, V)
  │        b. image_V = SDManager::generateMeshTexture(prompt, depth_V, controlNet, …)
  │           (same seed across views for style consistency)
  │
  ├─ 2. MultiViewTextureBaker::bake(entity/meshData, views[], images[], opts)
  │        for each submesh triangle T (in UV0 space):
  │          for each view V:
  │            - project T's 3 world verts through camera_V → screen UVs
  │            - facingWeight = max(0, −faceNormal · viewDir_V)   (cull back-faces)
  │            - occlusion test: sample depth_V at the projected centroid;
  │              skip if triangle is behind the recorded surface (± epsilon)
  │            - if facingWeight > 0: rasterize T into the atlas
  │              (VertexColorBaker::rasterizeTriangle pattern) but sample
  │              image_V at the per-pixel projected screen UV instead of
  │              interpolating a vertex colour; accumulate color*weight + weight
  │        resolve: atlasPixel = Σ(color*weight) / Σ(weight)
  │        dilate seams (VertexColorBaker::dilate) → fill UV-island gaps
  │
  └─ 3. export PNG → register resource → applyTextureToEntityDiffuse()
         (existing RTSS rebind + PBR slot wiring)
```

## New / changed components

### A. `MeshDepthRenderer` — add a view parameter (small change)
Today the camera is hardcoded to the −Z front (MeshDepthRenderer.cpp:162). Add a
view descriptor and a new overload; keep the existing `renderDepthMap` delegating
to the front view so #403 is unchanged.

```cpp
struct DepthView {
    Ogre::Vector3 dir;          // camera→target direction in WORLD space
                                // front = (0,0,+1) looking toward −Z mesh face
    const char*   name;         // "front", "back", "left", …  (for logs/files)
};
// returns the depth QImage AND the exact view/proj matrices used, so the
// baker re-projects through the identical camera (no drift).
struct DepthRenderResult { QImage depth; Ogre::Matrix4 view, proj; Ogre::Vector3 camPos; };
static DepthRenderResult renderDepthMapView(Ogre::Entity*, int size, const DepthView&, QString* err=nullptr);
```
Built-in view table (data): `front (0,0,+1)`, `back (0,0,−1)`, then `left/right/top/bottom`
for the 4–6 expansion. The bounding-sphere framing math is reused verbatim, only
`camPos = center + dir.normalised()*dist` and the look-at change.

### B. `MultiViewTextureBaker` (new — `src/MultiViewTextureBaker.h/.cpp`)
Pure-data baker (no GL): inputs are the per-view images + their view/proj
matrices + the mesh geometry (positions, normals, UV0, indices). Reuses:
- `VertexColorBaker::rasterizeTriangle` / `dilate` (UV-space raster + seam fill)
- the barycentric + projection math already in `TexturePaintController`
  (`findMeshPointForUV`, `hitTestUV`) and `EditModeController::worldToScreen`.

```cpp
struct BakeView { QImage image; Ogre::Matrix4 viewProj; Ogre::Vector3 camDir; QImage depth; };
struct BakeOptions { int resolution = 1024; int dilationPixels = 4;
                     float facingPower = 1.0f; float occlusionEpsilon = 0.01f; };
struct BakeReport { bool ok; int pixelsWritten; int trianglesProjected;
                    QList<int> perViewTriangleCounts; QString error; };
static BakeReport bake(const Ogre::Entity* entity,
                       const QList<BakeView>& views,
                       TexturePaintBuffer& out,
                       const BakeOptions& opts);
```
Accumulation buffer holds `RGBA + weightSum`; final divide → straight color.
Per-view facing weight gives a feathered front↔back blend with no hard seam.

### C. `MaterialEditorQML` orchestration
New `Q_INVOKABLE void generateMeshTextureMultiView(prompt, width, height,
controlStrength, QStringList views)`. Drives steps 0–3; the existing
single-view `generateMeshTextureFromPrompt` stays as the "front-only" fast path.
Generation is async (SDWorker thread) → run views sequentially, accumulate
images, bake on the last completion. Same seed across views.

## UI / MCP surface
- **Material Editor → AI panel:** a "Multi-view (front+back)" checkbox next to
  the existing "Use selected mesh (depth-conditioned)" toggle, plus a small
  view-count selector (2 / 4 / 6) once the data path is proven.
- **MCP:** extend `generate_mesh_texture` with an optional `views: ["front","back"]`
  array (defaults to `["front"]` = current behavior).
- Sentry breadcrumb category `ai.assist.mesh_texture_multiview`.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| Occlusion / back-facing bleed | facing weight `max(0,−n·viewDir)` + per-view depth occlusion test |
| Front/back style mismatch | same seed + prompt; (later) img2img-condition back on front |
| Side seam with only 2 views | feathered facing blend + dilate; document that 4 views removes it |
| Bad/missing UV0 | auto-unwrap gate (UvUnwrap) before bake |
| Live skinned-mesh buffer mutation | bake reads geometry read-only; unwrap uses the GUI-safe `unwrapEntityToFile` snapshot/restore path |
| RTSS apply unreliability (#403) | bake targets real UV0 atlas → `applyTextureToEntityDiffuse` binds a normal diffuse_map |

## Test plan (headless, CI Linux+Xvfb; ASSERT_TRUE(tryInitOgre), no GTEST_SKIP)
- `MultiViewTextureBaker` unit tests (pure data, no SD): synthetic 2-triangle
  quad + 2 solid-color "images" from ±Z → assert front color on front face,
  back color on back face, blended band in between; weight normalization;
  dilation fills gaps; empty-view and no-UV0 guards.
- `MeshDepthRenderer::renderDepthMapView` front vs back differ (non-identical
  images) on robot.mesh; returned matrices project a known vert to the expected
  screen quadrant.
- UV gate: low-coverage mesh triggers unwrap; good-UV mesh does not.
- SD-dependent end-to-end stays behind `#ifdef ENABLE_STABLE_DIFFUSION` and is
  not unit-tested (no model in CI), matching the #403 pattern.

## Phasing
1. **Slice 1 (prototype):** `renderDepthMapView` + `MultiViewTextureBaker` +
   front-only bake through real UV0 (validates reprojection math end-to-end).
2. **Slice 2:** add back view + facing-weighted blend; the front+back deliverable.
3. **Slice 3:** UV auto-unwrap gate; MCP `views` param; UI toggle.
4. **Later:** 4–6 views (config only); optional img2img cross-view conditioning.

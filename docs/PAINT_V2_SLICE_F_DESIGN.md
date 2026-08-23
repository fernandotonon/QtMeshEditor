# Paint v2 Slice F — Projection / stencil painting + decals (#549)

Two image-driven paint modes for the in-viewport texture painter (epic #543).
Both project an image onto the mesh **through a camera**, rasterize it in UV0
space, and write into a paint layer. They share one core (`ProjectionPainter`)
and both create a **new layer** so they never overwrite the active one.

---

## Architecture

- **`src/ProjectionMath.h`** — header-only shared math: `projectToViewportUV`
  (world → viewport UV + NDC z + behind flag) and `sampleImage` (bilinear).
  Used by both `MultiViewTextureBaker` (#403) and `ProjectionPainter`.
- **`src/ProjectionPainter.{h,cpp}`** (pure-data) — the projection rasterizer.
  `project()` splats one image through a camera `View` onto world `Triangle`s
  (from `MultiViewTextureBaker::fromEntity`) into a `TexturePaintBuffer`;
  `projectDab()` is the footprint-bounded, accumulating stencil-brush variant.
  Per texel: interpolate world position + projected source UV → facing cull →
  occlusion / depth-limit → sample source → soft-edge → **src-over composite**.
- **`src/DecalSession.{h,cpp}`** (pure-data) — the decal's world-anchored
  oriented-quad state machine (place / translate / rotate / scale) and its
  orthographic `buildCommit`.
- **`TexturePaintController`** — projection state + stencil-brush hook in the
  stroke path + `snapProjectionCamera`/`projectFromPhoto` + the decal session,
  overlay, and commit. **`MeshDepthRenderer::RenderResult`** gained
  `depthNear/depthFar` so the occlusion test reconstructs surface distance
  exactly.
- Viewport interaction: `TransformOperator` mouse branch (decal) +
  `MainWindow::keyPressEvent` (Enter commits / Esc cancels).

### Occlusion (the hard part)
`MeshDepthRenderer` renders a depth map from the camera; the grayscale is
**linear world distance via fog** (near = bright, far = dark) over
`[depthNear, depthFar]`. Per texel we project its world position through the
depth map's *own* `viewProj`, read the surface distance
`dMap = near + (1-g)*(far-near)`, and measure the texel's **camera-axis**
distance (`dot(wp - eye, camDir)` — matching fog, NOT Euclidean, or off-axis
texels self-occlude). Occluded when `dTexel > dMap + bias`; the bias exceeds the
8-bit fog quantisation (≥ 1/255 of the range). The color `View` and the
occlusion `viewProj` are kept strictly separate (the depth map auto-frames the
camera, so its matrices differ from the live camera's).

### Undo
A one-shot photo/decal commit adds a new `LayerType::Generated` layer via
`PaintLayerStack::addFromBuffer` and pushes **one** `PaintLayerOpCommand` — the
whole thing is a single undo step. The stencil *brush* is an ordinary stroke and
uses the existing `TexturePaintStrokeCommand`.

---

## User guide — what each option does

Open **Material Mode → Paint**. The **Projection** group is collapsible (starts
collapsed; a "•" next to the header means a projection mode is active).

### Projection mode

| Control | What it does |
|---|---|
| **Off** | Projection disabled — normal painting. |
| **Stencil** | As you brush, each dab is projected **through the live camera** and masked by the stencil image's alpha. The brush footprint follows the cursor; *what* is painted is the stencil sampled at each surface point's projected screen position. Rotating the view re-projects live. (Blender "Stencil" texture-paint mode.) |
| **Locked** | Same projection, but frozen to the camera pose captured with **Snap**. Orbit the model and keep painting while the projected image stays pinned — reach around the sides without the image sliding. Re-Snap to re-pin to the current view. |

### Projection controls

- **Stencil…** — pick the image whose **alpha channel masks every projected
  dab**. Opaque stencil texels let paint through; transparent ones block it —
  the cut-out you paint through. (An image with no alpha paints everywhere the
  brush touches, tinted by the image.)
- **Snap** — (Locked mode) captures the current camera's view/projection *and*
  renders the occlusion depth map for that pose, freezing them as the locked
  projection frame until you Snap again.
- **Backface** — reject triangles facing away from the camera, so the projection
  only lands on surfaces the camera sees head-on (grazing/back faces rejected by
  a facing-angle test). Blender's "Cull backfaces" default.
- **Occlude** — render a depth map and **reject texels hidden behind nearer
  surfaces**, so painting the front doesn't bleed onto parts tucked behind it
  (through a hole, or a far wall behind a near one). Backface handles *facing*;
  Occlude handles *hidden-behind-something*. Slightly slower (a depth render), so
  it's off by default.
- **Depth limit** (`projDepthLimit`, backend value as a fraction of the mesh
  bounds radius; 0 = off) — even among visible surfaces, reject anything more
  than this distance *behind* the nearest visible surface. Useful to paint only
  the front shell, not a far surface seen edge-on. This is what makes the
  "sphere-with-hole" case paint only the near rim, not the inner back wall.
- **Project from photo…** — a one-shot action: pick an image and it's projected
  through the **current camera** onto the whole visible mesh at once and
  committed as a **new layer** (one undo step). Line the model up to a reference
  photo, then stamp the photo straight onto it; respects Backface/Occlude, so
  only the camera-facing, unoccluded surface receives it (the back is left
  transparent to fill from another angle).

### Decal tool

- **Place decal…** — pick an image; the next **click on the mesh** anchors a
  rectangle on the surface (oriented by the surface normal, up = camera-up).
- **In the viewport**: drag the **body** to move, a **corner** to rotate (about
  the surface normal), an **edge** to scale that axis. The rectangle is a
  world-anchored quad, so it stays put as you orbit.
- **Enter** (or **Commit**) rasterizes the decal onto the mesh into a **new
  layer** with a soft edge — occlusion + backface culling wrap it onto the
  visible front arc only (a decal larger than the front hemisphere truncates).
  **Esc** (or **Cancel**) discards it.

**Typical flows.** Stencil + Backface + Occlude → brush details through a
cut-out. Line up a reference and *Project from photo* for a base, orbit, project
again from the other side. Or drop a logo/label as a *Decal*. Each projection
lands in its own layer, so you can blend, mask, or delete them independently.

---

## Tests

Pure-data (headless): `ProjectionPainter_test` (front projection, backface cull,
stencil-alpha gating, dab footprint+accumulate, sphere-with-hole occlusion,
depth-limit, self-projection no-acne) and `DecalSession_test` (state
transitions, handle-zone hit-test, translate/rotate/scale, world↔rect-UV,
ortho-commit corner→NDC mapping, soft-edge feather). GL fixture:
projection-mode setters + graceful no-camera, decal begin/cancel plumbing.

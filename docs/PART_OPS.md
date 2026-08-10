# PartOps — AI segmented parts, split, explode & join

PartOps (epic #859) turns QtMeshEditor's AI **mesh segmentation** into real mesh
authoring operations: take one fused character mesh, detect its parts
(head / torso / arms / legs …), and **split**, **explode**, **join**, or
**solidify** them. Works in the GUI, the `qtmesh` CLI, and over MCP — all three
produce equivalent results. GUI and MCP operations run in the live editor and
are a single undo step (`Ctrl+Z`); the CLI runs headless and writes an output
file, so it produces the same geometry with no editor undo step.

> Segmentation itself (the model + the "Select by Part" Edit-Mode tool) is
> documented in [MESH_SEGMENTATION_STRATEGY.md](MESH_SEGMENTATION_STRATEGY.md).
> PartOps is the *authoring* layer on top of it.

## The operations

| Op | What it does |
|---|---|
| **Split into Parts** | Segments the mesh and replaces it with one **named submesh per part** (`head`, `torso`, `left_arm`, …). Boundary vertices are duplicated so parts are independent; normals / UVs / colours / tangents / skeleton + bone weights are preserved (skinned characters stay riggable). |
| **Explode Parts** | Splits every submesh of a multi-part mesh into its own **scene node**, offset outward from the assembly centre — an exploded view you can edit part-by-part. |
| **Join Parts** | Merges 2+ part entities back into **one fused mesh**, baking each part's world transform into its geometry. Same-material submeshes coalesce. |
| **Solidify** (opt-in) | Gives each part real **wall volume**. Thin game-asset shells are single-sided surfaces with no thickness, so an exploded part exposes its hollow interior at the cut; solidify offsets an inner shell + stitches a wall so the cut shows a solid cross-section, and seals each part watertight. |

## GUI

Object mode → Inspector:

- **Split into Parts (AI)** section: pick a *Category* (auto / body / vegetation /
  vehicle / building), tick **AI assisted** (uncheck for the offline
  geometric / rig-prior path), optionally tick **Solidify thin shells**, then
  **Split into Parts**.
- **Explode / Join Parts** section: set the **Explode distance** and click
  **Explode Parts** (needs one multi-part mesh selected), or select 2+ part nodes
  and click **Join Parts**.

Every operation is a single undo step (`Ctrl+Z`).

## CLI (`qtmesh segment`)

```bash
# Split into named per-part submeshes
qtmesh segment model.fbx --split-parts -o parts.glb
qtmesh segment model.fbx --split-parts --no-model -o parts.glb   # offline path
qtmesh segment model.fbx --split-parts --solidify -o parts.glb   # + wall volume

# Explode into a multi-node scene (splits first)
qtmesh segment model.fbx --explode-parts -o scene.glb
qtmesh segment model.fbx --explode-parts --explode-distance 0.25 --solidify -o scene.glb

# Just dump the labels (no geometry change)
qtmesh segment model.fbx --write-labels labels.json   # schema qtmesh-partops-labels-v1
qtmesh segment model.fbx --json                        # per-part vertex/face counts
```

`--explode-distance` is a multiplier on the assembly diagonal (default `0.15`;
`0` = parts coincident). Add `--json` to any command for a structured report
(part names, created counts, distance). Join is GUI/MCP only (it needs several
part entities at once, which doesn't fit the single-input `segment` CLI).

## MCP

| Tool | Args | Returns |
|---|---|---|
| `segment_mesh` | `entity_name?`, `no_model?`, `category?`, `up_axis?` | per-part counts + `face_labels` (always) |
| `split_mesh_by_segments` | `entity_name?`, `no_model?`, `category?`, `up_axis?`, `solidify?` | created submesh count + part names |
| `explode_mesh_parts` | `entity_name?`, `distance?` | exploded part count |
| `join_mesh_parts` | `entity_names?` (omit → all mesh entities) | joined part count + created submesh count |

All operate on the live editor scene through the same undoable commands as the
GUI buttons, so `Ctrl+Z` in the editor undoes an MCP-driven split/explode/join.

## Limitations

- **Body-centric labels.** The default part vocabulary is humanoid
  (head / torso / L+R arm / L+R leg). Non-body categories (vegetation / vehicle /
  building) have their own label sets — pass `--category` / `category`. See the
  segmentation strategy doc for the full vocabulary.
- **Model-unit dimensions.** Explode distance and solidify thickness are in the
  mesh's own units (relative to its bounding-box diagonal), not millimetres.
- **Join yields static geometry.** Skeletons are **not** reconciled across parts,
  so joining a skinned character produces a static mesh (bone weights are
  dropped). Split → edit → **join** is a geometry workflow, not a rigging one.
- **Thin shells look hollow at a cut** unless you **Solidify** — capping the cut
  ring alone is watertight but a zero-thickness game shell still shows its inner
  back-wall. Solidify is the fix.
- **No 3D-print alignment pegs / boolean cutting.** An earlier attempt at
  dowel/socket connectors was removed: there is no safe flat cut plane through an
  organic AI-segmented joint (a plane through a hip seam also slices the torso),
  and the leading image-to-3D tools (Meshy / Tripo) cut organic seams but ship no
  discrete pegs either. Out of scope for this epic.

## Export

Split/exploded parts round-trip through **FBX** (submesh boundaries + part names
preserved) and **glTF/glb** (same-material parts coalesce; a multi-node explode
exports as a multi-node scene). STL export works for single solid parts.

## Telemetry

Sentry breadcrumbs: `mesh.parts.segment_preview`, `mesh.parts.split_segments`,
`mesh.parts.explode`, `mesh.parts.join`.

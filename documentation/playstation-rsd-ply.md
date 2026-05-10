# PlayStation RSD, Psy-Q PLY, and MAT sidecars

QtMeshEditor treats **PlayStation RSD** (`.rsd`) as a small **descriptor** that points at a mesh and optional texture/material sidecars. The mesh is usually a **TMD** (`.tmd`) or a **Sony Psy-Q PLY** (`.ply`). These are **not** Stanford PLY files: Psy-Q PLY uses an `@PLY…` header, separate **vertex** and **normal** tables, and face lines with **independent** vertex and normal indices.

## RSD (`.rsd`)

An RSD file lists paths or basenames for:

- **Model** — typically `.tmd` or Psy-Q `.ply`
- **Texture / TIM** — PlayStation 4-bit / 8-bit / 16-bit images (`.tim`), applied when loading through the RSD path
- **Material** — optional `.mat` companion with per-face or material parameters used by classic toolchains

Importing `.rsd` resolves those references, loads the mesh (TMD or Psy-Q PLY), and attaches TIM-driven materials when possible. Exporting to `.rsd` writes the descriptor plus best-effort sidecar filenames (`.tmd` / `.tim` / etc.) next to the output.

## Psy-Q PLY import

1. **Header** — e.g. `@PLY940102` (variant digits allowed).
2. **Counts** — one line: `nV nN nF` (number of **positions**, **normals**, **faces**). Positions and normals are **separate pools**; `nV` and `nN` need not match.
3. **Vertices** — `nV` lines of `x y z` (model space; import applies the editor’s Psy-Q PLY world transform).
4. **Normals** — `nN` lines of `nx ny nz`.
5. **Faces** — per face:
   - `0 v0 v1 v2 pad n0 n1 n2 pad` — triangle
   - `1 v0 v1 v2 v3 n0 n1 n2 n3` — quad (PlayStation-style split into two triangles internally is `(v0,v1,v2)` and `(v1,v2,v3)`)

Corner attributes (position + normal + optional colour) are **welded** on import so Ogre’s indexed triangle list matches shading boundaries.

When the file encodes **quads or higher polygons**, the importer records logical face sizes and stores **n-gon topology** on the mesh (`qtme.faces.*`, same mechanism as FBX n-gons). That preserves artist intent for later export.

## Psy-Q PLY export

Export walks the renderable mesh and writes:

1. **Welded position table** — unique positions after **quantized** welding (fixed-point style bucket in world space).
2. **Welded normal table** — **independent** pool: same quantization, but normals deduplicate separately from positions. Many corners can share one quantized normal, so **`nN` is often smaller than `nV`**, even when the source file listed more normal lines. That is expected and usually means redundant normals collapsed to one index after quantization.
3. **Face lines** — triangles (`0 …`) or quads (`1 …`) with **separate** vertex and normal indices per corner.

### Why fewer normals than the original file is OK

Original Psy-Q assets often duplicate the same direction many times with tiny float differences. After import, Ogre may still carry one normal per corner. On export, each corner’s normal is quantized and **welded into the normal pool**. If four corners of a flat quad all map to the same quantized `(nx,ny,nz)`, the file shows **one** normal line and four indices pointing at it — **fewer lines than a verbose original**, but equivalent shading for that polygon.

### `qtme.faces` vs heuristic quad restore

- If **`qtme.faces.0`** (etc.) is present — for example after importing a Psy-Q file with quad lines, or after editing in mesh mode — export uses those **n-gons** directly. Quads stay quads; larger n-gons are fanned to triangles in the file as required by the format.
- If that metadata is **missing** — e.g. a mesh that only exists as two triangles in the index buffer — export runs a **heuristic**: pairs of adjacent triangles that form a convex quad, share an edge in the PS1 `(v0,v1,v2)+(v1,v2,v3)` pattern, and have **nearly parallel** face normals (dot ≥ 0.94) are merged back into a **single quad line**. That restores many flat quads lost when DCC tools triangulate, without inventing quads across sharp creases.

Together, explicit **n-gon** metadata and the **merge heuristic** are why round-trips through QtMeshEditor often match or **improve** on the original polygon layout: fewer spurious triangle pairs, and normal pools that collapse true duplicates after quantization.

## MAT sidecars

When vertex colours exist on all submeshes, Psy-Q PLY export can optionally emit **per-face colours** (for tooling that builds a `.mat` from face colours). RSD export paths wire this where the pipeline supplies a colour buffer.

## See also

- `PS1PLY.h` / `PS1PLY.cpp` — detection, import, export, merge heuristic
- `PS1RSD` module — RSD parse/load/save and sidecar resolution

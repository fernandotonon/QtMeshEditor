# Paint v2 — CLI & MCP reference (Slice J, #553)

Scriptable surfaces for the texture painter. The GUI remains the place to
actually paint strokes; these exist for the operations that are meaningful
without a cursor.

## `qtmesh paint`

### Queries (no mesh, no render system)

```bash
qtmesh paint --list-stamps    [--json]   # bundled + custom brush stamps
qtmesh paint --list-presets   [--json]   # brush presets
qtmesh paint --list-palettes  [--json]   # colour palettes + swatch counts
```

### Layers

```bash
qtmesh paint model.fbx --layer list [--json]
```

`--layer add`, `--layer merge-down` and `--layer flatten` are **deliberately not
implemented**. Paint layers live in a live in-memory session and are never
persisted to a mesh file, so a headless `--layer add` would create a layer,
write nothing to the output, and exit — a command that appears to work and does
nothing. Use the GUI to author layers, and `--bake` to write painted pixels to
disk. On a freshly imported mesh `--layer list` honestly reports zero layers,
plus which channels carry texture data.

### Bake

```bash
qtmesh paint model.fbx --bake --engine unreal -o out/
qtmesh paint model.fbx --bake --engine unity --resolution 2048 --prefix hero -o out/
```

`--engine` is an alias of `qtmesh paint-bake --target`; both delegate to the
same core, so the two commands cannot drift. See
`docs/PAINT_V2_SLICE_I_DESIGN.md` for the engine channel layouts.

### Stencil projection

```bash
qtmesh paint model.fbx --apply-stencil decal.png \
  --camera "0,1,4,0,1,0" --channel basecolor --resolution 1024 -o out.mesh
```

`--camera` is six comma-separated numbers: eye xyz then target xyz. It is
**required** — there is no viewport camera to fall back on headlessly. A camera
that cannot see the mesh reports "wrote no texels" and writes nothing, rather
than emitting a silently-unchanged file.

## MCP tools

All of these except `paint_bake` drive the **running editor's** paint session,
so they need a GUI-attached server (`--with-mcp`). `paint_bake` is
file-in/file-out and works headlessly.

| Tool | Purpose |
|---|---|
| `paint_set_enabled` | **Call this first.** Enters/leaves texture-paint mode; the others need a live session. |
| `paint_list_layers` | Layer names, visibility, opacity, blend mode |
| `paint_add_layer` | Add an empty layer above the active one |
| `paint_delete_layer` | Delete one layer |
| `paint_reorder_layer` | Move a layer (`from` → `to`); higher index draws on top |
| `paint_merge_down` | Merge a layer into the one beneath |
| `paint_flatten` | Collapse every layer into one |
| `paint_set_active_layer` | Choose which layer receives strokes |
| `paint_set_active_channel` | `basecolor` \| `normal` \| `roughness` \| `metallic` \| `ao` \| `emissive` |
| `paint_set_brush_preset` | Apply a preset; omit `name` to list them |
| `paint_set_color` | Set foreground (or `background`) colour |
| `paint_set_gradient` | Gradient colour source: `ramp`, `mode`, `enabled` |
| `paint_apply_stencil` | Project an image through a camera as a new layer |
| `paint_bake` | Repack a mesh's PBR textures into an engine layout |

Every mutating tool returns the resulting layer list, active layer and active
channel, so a caller can see the effect of its own call without a second round
trip.

`paint_set_active_channel` rejects an unknown channel id rather than falling
back to BaseColor: silently painting the wrong channel is very hard to notice.
It also rejects `height`, which is not a paintable channel — it shares the
Normal session (see #547).

### stdio framing

The stdio transport uses **LSP-style `Content-Length` framing**, not
newline-delimited JSON:

```
Content-Length: 57\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
```

A newline-only client hangs on the first read.

## On-disk layout

Custom paint assets live under `<AppData>/paint/`:

| Path | Contents | Slice |
|---|---|---|
| `presets/*.json` | brush presets | H (#551) |
| `palettes/*.json` | colour palettes | H (#551) |
| `ramps/*.json` | gradient ramps | A (#544) |
| `stamps/` | brush stamp images | B (#545) |
| `tilings/` | tiling source images | B (#545) |

Bundled assets are compiled into the binary so they cannot go missing from an
install. Custom entries override a bundled one of the same name, and filenames
use hashed stems so two names that sanitise alike cannot overwrite each other.

## Not implemented

- **Cavity / curvature / AO masks** — Slice G (#550) was closed as not planned;
  no implementation exists, so there is no `paint.derived_map.*` breadcrumb and
  no `DerivedMaps_test.cpp`.
- **Tablet pressure / tilt** — deliberately skipped in Slice H: the project is
  desktop-only, so a pressure curve could be written and unit-tested but never
  verified against real pen hardware. It should return as its own issue when
  there is a tablet to test on.
- **Live stroke authoring over CLI** — explicitly out of scope in #553.

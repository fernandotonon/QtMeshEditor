# USD (.usd / .usda / .usdc / .usdz) import + export

Slice J of the animation-interop epic (#527). Opt-in: configure with
`-DENABLE_USD=ON` (default OFF).

```bash
cmake . -B build_local -DENABLE_USD=ON ...
qtmesh convert model.fbx -o model.usd      # crate (binary)
qtmesh convert model.fbx -o model.usda     # ASCII
qtmesh convert model.fbx -o model.usdz     # package, textures embedded
qtmesh info rig.usdz
qtmesh convert rig.usda -o rig.glb
```

MCP: `load_mesh` and `export_mesh` take `.usd/.usda/.usdc/.usdz` paths (the
extension selects ASCII / Crate / Package). GUI: File → Open accepts the four
extensions; File → Export lists *USD ASCII*, *USD Crate* and *USD Package*.

## Why LightUSD and not OpenUSD

The issue suggested vendoring Pixar's OpenUSD. We use **LightUSD** (formerly
TinyUSDZ; Apache-2.0) instead:

| | OpenUSD | LightUSD |
|---|---|---|
| Dependencies | TBB, Boost-era pieces, plugin registry | none (C++17) |
| Build | multi-minute, per-platform recipes, no maintained MinGW path | `add_subdirectory`, ~1 min cold |
| Read / write USDA, USDC, USDZ | yes | yes |
| Completeness | reference implementation | smaller and newer: complex composition and Hydra-only schemas are less complete |

The surface this editor needs — meshes, UsdPreviewSurface materials,
skeletons, SkelAnimation, blend shapes, xform animation — is covered. If a
production USD file turns out to need OpenUSD-only behaviour, that is the
trigger to revisit the choice; the bridge (`src/UsdBridge.cpp`) is the only
file that touches the library.

## What is supported

| | Import | Export |
|---|---|---|
| Polygon meshes (n-gons triangulated on import) | yes | yes |
| Normals, UV0 | yes | yes |
| UsdPreviewSurface: diffuse colour / texture, normal map, opacity, metallic, roughness, emissive | yes | colour, diffuse texture, normal map |
| Skeleton + skin weights (top 8 influences) | yes | yes |
| SkelAnimation clips (every clip kept, names preserved) | yes | yes |
| Blend-shape targets (names preserved) | yes | yes |
| Blend-shape **weight animation** | **no** (reported as a warning) | yes |
| Node (xform) animation | yes | first root-node clip only |
| `.usdz` textures | unpacked to a temp directory | packaged |
| Z-up / centimetre stages | brought to Y-up metres through the root transform | written Y-up, metres |

Joint names that are not valid USD identifiers (`mixamorig:Hips`) are written
sanitised in `joints` and verbatim in a custom `qtmesh:jointNames` attribute, so
they round-trip. (Not in `jointNames`: Blender parses that attribute as
SdfPaths and a name with `:` invalidates the whole joint order.)

## LightUSD patches (`cmake/patches/`)

Two small patches are applied to the fetched sources (idempotently, by
`cmake/ApplyPatchesOnce.cmake`); both were found by opening our output in / our
input from Blender:

* `lightusd-uncompressed-token-arrays.patch` — LightUSD's crate writer compresses
  `token[]` arrays of 16+ elements; OpenUSD readers expect them raw and read
  garbage, so a rig with 16+ joints lost its skeleton in Blender.
* `lightusd-lenient-colorspace.patch` — an unknown texture `colorSpace` tag
  (Blender writes `srgb_rec709_display`) failed the whole import; it now warns.

A `-DFETCHCONTENT_SOURCE_DIR_QTMESH_LIGHTUSD=<dir>` checkout is not patched
automatically: apply the patches to it yourself.

## Verified against Blender

Rumba Dancing.fbx → `.usdc` / `.usdz` → Blender: 11 meshes / 5,828 vertices,
an armature with 70 bones, the action, outward-facing normals. Blender's own
USD export of that scene → `qtmesh`: 5,828 vertices, 10,220 triangles, 70
bones, 1 animation.

## Round trip

`UsdBridge_test.cpp` writes a rigged scene (two bones, per-vertex weights, a
blend shape, a textured material, two clips), reads it back for each of
`usda`, `usdc`, `usd` and `usdz`, and checks geometry, weights, hierarchy,
clip count / names / keys / durations, shape offsets and the material. A
second group exports a skinned Ogre entity through `MeshImporterExporter` and
re-imports it, asserting the same bone count, clip count and vertex count.

## Architecture

The bridge converts USD ⇄ `aiScene` and registers itself with Assimp as an
importer and an exporter, so the existing skeleton / skin / blend-shape / clip
processors (`src/Assimp/*`, `MeshImporterExporter::buildAiScene`) do the Ogre
side exactly as they do for glTF and FBX. `ENABLE_USD=OFF` builds compile the
same files with stubs: every USD request answers
"rebuild with -DENABLE_USD=ON" and nothing links against LightUSD.

## Binary size

LightUSD compiles to a ~44 MB static archive (unstripped, debug symbols
included); only the code the bridge reaches is linked.

Measured on macOS arm64, release app binary (`QtMeshEditor.app/Contents/MacOS/QtMeshEditor`),
same configuration with and without `-DENABLE_USD=ON`:

| | bytes | MiB |
|---|---|---|
| without USD | 47,837,088 | 45.6 |
| with USD | 65,772,400 | 62.7 |
| **delta** | **+17,935,312** | **+17.1 (+37 %)** |

That is why the flag defaults to OFF. Cold build cost of the library: about a minute on 8 cores.

## Known gaps

* Blend-shape weight animation is not imported (nothing in the Assimp → Ogre
  path reads `aiMeshMorphAnim`).
* Only the first root-node clip is exported as xform animation; USD has one
  timeline per prim, not named clips.
* Only the diffuse and normal textures are exported.
* References / payloads / variants are honoured only as far as LightUSD's
  Tydra converter flattens them.

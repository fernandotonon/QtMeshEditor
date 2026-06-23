
# <img width=30 align="top" src="https://user-images.githubusercontent.com/996529/209745977-7b797223-46ce-4bce-aa70-707a88f2aaf2.png"> QtMeshEditor

Automate your 3D asset pipeline — scan, validate, convert, fix, and merge 3D assets with GUI + CLI + CI/CD support.

**Open 3D files from your OS** — after install, double-click `.fbx`, `.glb`, `.gltf`, `.obj`, `.dae`, `.stl`, `.ply`, `.mesh`, and PS1 `.rsd`/`.tmd` in Finder, Explorer, or your Linux file manager (or use **Open With**). QtMeshEditor loads the model in the running window; a second launch focuses the existing instance instead of spawning another process. Portable Windows ZIP users can run `bin/scripts/register-windows-file-associations.ps1` to register handlers without admin rights.

[![GitHub stars](https://img.shields.io/github/stars/fernandotonon/QtMeshEditor.svg?style=social&label=Star&maxAge=2592000)](https://GitHub.com/fernandotonon/QtMeshEditor/stargazers) Star if you like it!

[![Github All Releases](https://img.shields.io/github/downloads/fernandotonon/QtMeshEditor/total.svg)]()
[![qtmesheditor](https://snapcraft.io/qtmesheditor/badge.svg)](https://snapcraft.io/qtmesheditor)
[![Coverage](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=coverage)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Maintainability Rating](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=sqale_rating)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Vulnerabilities](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=vulnerabilities)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Duplicated Lines (%)](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=duplicated_lines_density)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)

#### QtMesh Cloud Badges
[![qtmesh status](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-status.svg)](https://qtmesh.dev)
[![qtmesh score](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-score.svg)](https://qtmesh.dev)
[![qtmesh errors](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-errors.svg)](https://qtmesh.dev)
[![qtmesh warnings](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-warnings.svg)](https://qtmesh.dev)
[![qtmesh models](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-models.svg)](https://qtmesh.dev)
[![qtmesh animations](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-animations.svg)](https://qtmesh.dev)
[![qtmesh skeletons](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-skeletons.svg)](https://qtmesh.dev)
[![qtmesh materials](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-materials.svg)](https://qtmesh.dev)

---

### 🔌 CI/CD — GitHub Actions

Available on the [GitHub Actions Marketplace](https://github.com/marketplace/actions/qtmesheditor).

[![Latest action release](https://img.shields.io/github/v/release/fernandotonon/QtMeshEditor?label=latest%20action)](https://github.com/fernandotonon/QtMeshEditor/releases/latest)

**Versioning**

- **Always follow the latest GitHub release** — use the Marketplace floating tag `fernandotonon/QtMeshEditor@v1` (same pattern as the [Marketplace example](https://github.com/marketplace/actions/qtmesheditor)). The composite action defaults to `image-tag: latest`, so the Docker CLI tracks the newest published `ghcr.io/fernandotonon/qtmesh` image.
- **Reproducible builds** — pin the action and the container to the same semver as this repository’s `project(QtMeshEditor VERSION …)` in `CMakeLists.txt` (currently **3.9.0**). After bumping the version in CMake, run `./scripts/sync-doc-versions-from-cmake.sh` to refresh the pinned refs in `README.md` and the docs site fallback; CI enforces the match with `./scripts/sync-doc-versions-from-cmake.sh --check`.

Pinned workflow template (action + `ghcr.io` image aligned):

```yaml
name: QtMesh Scan

on:
  push:
    branches: [ "master" ]

jobs:
  scan-assets-qtmesh:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Run QtMesh scan
        uses: fernandotonon/QtMeshEditor@3.9.0
        with:
          command: scan
          image-tag: "3.9.0"
        env:
          QTMESH_CLOUD_TOKEN: ${{ secrets.QTMESH_CLOUD_TOKEN }}
```

Floating ref (auto-updates when the `v1` tag moves on release):

```yaml
      - name: Run QtMesh scan
        uses: fernandotonon/QtMeshEditor@v1
        with:
          command: scan
```

`QTMESH_CLOUD_TOKEN` is forwarded into the container and used by `scan` upload. GitHub Actions metadata (`GITHUB_*`) is also forwarded so uploads include `meta` fields (branch/commit/run and context used by QtMesh Cloud).

To fail CI when upload fails, add `qtmesh-strict-upload: true` (or pass `--strict-upload` in `options`).

Release tags are listed on the [releases page](https://github.com/fernandotonon/QtMeshEditor/releases/latest) (also referenced in QtMesh Cloud onboarding).

<details>
<summary>More CI examples</summary>

```yaml
# Validate a specific mesh
- uses: fernandotonon/QtMeshEditor@3.9.0
  with:
    command: validate
    input-file: ./models/character.fbx
    image-tag: "3.9.0"

# Convert FBX → glTF
- uses: fernandotonon/QtMeshEditor@3.9.0
  with:
    command: convert
    input-file: ./models/character.fbx
    output-file: ./output/character.gltf2
    image-tag: "3.9.0"

# Resample Mixamo animations (200+ keyframes → 30)
- uses: fernandotonon/QtMeshEditor@3.9.0
  with:
    command: anim
    input-file: ./animations/dance.fbx
    output-file: ./output/dance_optimized.fbx
    options: --resample 30
    image-tag: "3.9.0"

# Get mesh info as JSON
- uses: fernandotonon/QtMeshEditor@3.9.0
  id: info
  with:
    command: info
    input-file: ./models/character.fbx
    options: --json
    image-tag: "3.9.0"

# Docker (alternative — :latest tracks newest image; pin :3.4.0 to match semver action ref)
docker run --rm -v $(pwd):/workspace ghcr.io/fernandotonon/qtmesh:latest scan ./assets --fail-on error
```

</details>

### ☁️ QtMesh Cloud Badges

1. Sign in at [qtmesh.dev](https://qtmesh.dev) and create a project.
2. Create a project token in QtMesh Cloud.
3. Add the token as a GitHub secret named `QTMESH_CLOUD_TOKEN`.
4. Run the scan command from Github Actions example above.

Badge markdown (replace `<owner-slug>` and `<project-slug>`):

```md
[![qtmesh status](https://api.qtmesh.dev/v1/u/<owner-slug>/p/<project-slug>/badges/qtmesh-status.svg)](https://qtmesh.dev)
[![qtmesh errors](https://api.qtmesh.dev/v1/u/<owner-slug>/p/<project-slug>/badges/qtmesh-errors.svg)](https://qtmesh.dev)
[![qtmesh warnings](https://api.qtmesh.dev/v1/u/<owner-slug>/p/<project-slug>/badges/qtmesh-warnings.svg)](https://qtmesh.dev)
```

---

### 🔧 CLI Pipeline (`qtmesh`)

Same commands locally, in Docker, or in CI:

```bash
# Scan a directory for asset issues
qtmesh scan ./assets --fail-on warning
qtmesh scan ./assets --json --report report.json

# Inspect a mesh
qtmesh info model.fbx --json

# Validate geometry
qtmesh validate model.fbx

# Convert between formats
qtmesh convert model.fbx -o model.gltf2

# Fix / optimize
qtmesh fix model.fbx -o fixed.fbx --all

# Animation tools
qtmesh anim model.fbx --list
qtmesh anim model.fbx --rename "Take 001" "Idle" -o renamed.fbx
qtmesh anim model.fbx --resample 30 -o optimized.fbx
qtmesh anim base.fbx --merge walk.fbx run.fbx -o merged.fbx

# Export animation pose as static mesh (3D printing)
qtmesh pose model.fbx --animation "Dance" --count 4 -o pose_%02d.stl

# LOD generation
qtmesh lod model.fbx --auto

# Bake vertex colors → texture (with UV-seam dilation)
qtmesh bake-vertex-colors model.fbx -o color_map.png --resolution 1024 --dilation 4

# Auto UV unwrap (xatlas — same library Blender/Godot use)
qtmesh uv model.fbx --unwrap -o unwrapped.glb               # overwrite UV0
qtmesh uv model.fbx --unwrap --channel 1 -o lightmap.glb    # keep UV0, write UV1 (lightmap workflow)
qtmesh uv model.fbx --info --json                           # report UV channels + coverage

# Quad retopology (triangle-pairing — no new deps)
qtmesh retopo model.fbx -o quads.glb                        # pair every viable triangle into quads
qtmesh retopo model.fbx --target-faces 5000 -o lo.glb       # stop early once near target face count
qtmesh retopo model.fbx --max-angle 15 -o conservative.glb  # tighter coplanarity gate

# Compute skin weights (inverse-distance heuristic; mesh must have a skeleton)
qtmesh skin model.fbx -o skinned.glb                        # default 4 influences, falloff 4
qtmesh skin model.fbx --max-influences 8 --falloff 6 -o skinned.glb
qtmesh skin model.fbx --skip-unweighted --merge -o filled.glb  # fill missing weights only
```

---

### 🎨 Paint Tools

ZBrush-style polypaint and BaseColor texture painting, with a 2D preview
panel, multiple brush tools (paint, erase, fill, color picker, smudge),
texture-slot picker per submesh, and a one-click bake that turns vertex
colors into a UV-space texture.

**Quick start (texture paint, GUI):**

1. Switch to **Material Mode** (mode bar at the top).
2. Select a mesh entity. The Inspector's **Texture Paint** section
   shows a slot picker (every paintable TUS across submeshes) and a 256×256
   live preview of the active texture.
3. Click the **paint brush** button in the toolbar to enable painting. The
   first click on the mesh auto-creates a paint session against the active
   slot (or you can pre-create with *Create / Attach Texture*).
4. Pick a tool from the row at the top of the panel: ✏ Paint, ⌫ Erase, ⧉ Fill,
   ⊰ Pick (eyedropper), ∿ Smudge. Brush color/radius/strength/falloff comes
   from the shared Paint Brush section above the toolbar's brush popup.
5. Left-click-drag on **either** the 3D mesh or the 2D preview panel. Both
   drive the same paint buffer; the 2D preview updates in real time and a
   brush ring shows where the cursor maps on the 3D surface (and vice versa).
6. *Save…* / *Load…* round-trips the buffer to disk as PNG (or any format
   QImage can write).

**Vertex paint (GUI):**

Same flow, but vertex paint operates in **Edit Mode** instead. Press **Tab**
on a selected mesh to enter Edit Mode, then tick *Vertex Color Preview* in
the Edit Mode Tools section to see vertex colors live.

**Bake Vertex Colors → Texture** rasterizes the active mesh's vertex colors
into a UV-space PNG via barycentric interpolation, then dilates the result
outward by N pixels to mask UV-seam bleed at MIP-map time.

**Headless (CLI):**

```bash
qtmesh bake-vertex-colors model.fbx -o color_map.png --resolution 1024 --dilation 4
```

**Export.** Vertex colors are preserved on export to formats that support
them (glTF preferred — verified round-trip).

**Notes / limitations.**

- Brush size is in local mesh units (shared with vertex paint). For texture
  paint we divide by mesh bounding-box extent and clamp to a UV-friendly
  range, so 0.25 produces a sensible stamp on both a unit cube and a
  100-unit character.
- Painting auto-rebinds every TUS named `albedo` or `diffuse_map` on the
  active material — imported PBR materials alias the diffuse texture under
  both, and rebinding only one would leave the other pointing at the
  original.
- Bake uses fan triangulation of n-gon faces; concave faces should be
  pre-triangulated.

---

### ✨ Merge Mixamo Animations in Seconds

Download animations from [Mixamo](https://www.mixamo.com), drop them into QtMeshEditor, and merge into a single file — export as glTF, FBX, Collada, OBJ, or Ogre Mesh.

![Merge Animations Demo](https://github.com/user-attachments/assets/441f90c5-1968-4838-8001-4ca24856a501)

### 🎬 More in Action

Split View|Skeleton Animation Controls
---|---
![QtMeshEditor1 5 0](https://user-images.githubusercontent.com/996529/210196572-7b49da4c-c5db-406d-9ab4-7fa20bacb6ae.gif)|![QtMeshEditor1 6 0](https://user-images.githubusercontent.com/996529/218779819-0a61156d-c014-4ad1-aa8b-cee900c9da56.gif)
**MCP tools (AI Agent Control)**|**Bone Weight Visualization**
![MCP Demo](https://github.com/user-attachments/assets/ed3b7e9d-22ba-4e6e-a06c-868570db7a07)|![Bone Weights](https://github.com/user-attachments/assets/289403ac-8952-488c-bc65-0a768ab278e1)

#### 🤖 AI-enhanced Material Editor

![AI Materials](https://github.com/user-attachments/assets/c58978d7-7564-41f2-8c95-527ddf7ae78e)

---

### 🎮 Features

- **Asset scanning** — ESLint for 3D assets: check naming, complexity, skeletons, formats
- **40+ format support** — FBX, glTF, OBJ, Collada, STL, Ogre Mesh, and more
- **Animation merge** — combine Mixamo clips into one file
- **Animation resampling** — reduce keyframe density for game engines
- **Pose export** — bake animation frames as static meshes (3D printing)
- **LOD generation** — automatic level-of-detail mesh reduction
- **Paint tools** — vertex paint, texture paint (BaseColor), bake vertex colors to texture with seam dilation
- **Material editor** — visual editing with AI-assisted generation
- **Skeleton inspection** — bone weights, debug overlays, animation preview
- **Scene management** — duplicate (Ctrl+D), group (Ctrl+G), snap, pivot modes
- **AI chat** — natural language scene editing via local LLMs
- **MCP server** — 51 tools for AI agents (Claude, Cursor, etc.)
- **REST API** — HTTP interface for external automation

---

### 📦 Install

| Platform | Command |
|----------|---------|
| **Windows** | `winget install FernandoTonon.QtMeshEditor` |
| **macOS** | `brew tap fernandotonon/qtmesheditor && brew trust fernandotonon/qtmesheditor && brew install qtmesheditor` |
| **Linux** | `sudo snap install qtmesheditor` |
| **Docker** | `docker run --rm ghcr.io/fernandotonon/qtmesh --help` |

📥 [Download latest release](https://github.com/fernandotonon/QtMeshEditor/releases/latest) · 📖 [Website & docs](https://fernandotonon.github.io/QtMeshEditor/)

---

### 📋 Format Support

| Format | Extension | Import | Export | Skeleton/Animation |
|--------|-----------|--------|--------|--------------------|
| FBX Binary | .fbx | ✅ | ✅ | ✅ |
| glTF 2.0 | .gltf2 / .glb2 | ✅ | ✅ | ✅ |
| Collada | .dae | ✅ | ✅ | ✅ |
| OBJ | .obj | ✅ | ✅ | — |
| STL | .stl | ✅ | ✅ | — |
| Ogre Mesh | .mesh / .mesh.xml | ✅ | ✅ | ✅ |
| PlayStation TMD | .tmd | ✅ | ✅ | — |
| PlayStation RSD | .rsd | ✅ | ✅ | — |
| 3DS | .3ds | ✅ | ✅ | — |
| Stanford PLY | .ply | ✅ | ✅ | — |
| Psy-Q PLY (PlayStation) | .ply | ✅ | ✅ | — |

`.ply` is dispatched by content: **Stanford** (`ply` header) vs **Psy-Q** (`@PLY…` header). See [documentation/playstation-rsd-ply.md](documentation/playstation-rsd-ply.md).

Import supports all formats provided by Assimp (40+).

---

📖 [Documentation](https://fernandotonon.github.io/QtMeshEditor/docs.html) · 🛠 [Build from source](https://github.com/fernandotonon/QtMeshEditor/wiki/How-to-build) · 🐛 [Report issues](https://github.com/fernandotonon/QtMeshEditor/issues) · 💬 [Contribute](https://github.com/fernandotonon/QtMeshEditor)

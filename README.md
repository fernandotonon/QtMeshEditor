
# <img width=30 align="top" src="https://user-images.githubusercontent.com/996529/209745977-7b797223-46ce-4bce-aa70-707a88f2aaf2.png"> QtMeshEditor

Automate your 3D asset pipeline — scan, validate, convert, fix, and merge 3D assets with GUI + CLI + CI/CD support.

[![GitHub stars](https://img.shields.io/github/stars/fernandotonon/QtMeshEditor.svg?style=social&label=Star&maxAge=2592000)](https://GitHub.com/fernandotonon/QtMeshEditor/stargazers) Star if you like it!

[![Github All Releases](https://img.shields.io/github/downloads/fernandotonon/QtMeshEditor/total.svg)]()
[![qtmesheditor](https://snapcraft.io/qtmesheditor/badge.svg)](https://snapcraft.io/qtmesheditor)
[![Coverage](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=coverage)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Maintainability Rating](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=sqale_rating)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Vulnerabilities](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=vulnerabilities)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Duplicated Lines (%)](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=duplicated_lines_density)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)

#### QtMesh Badges
[![qtmesh status](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-status.svg)](https://qtmesh.dev)
[![qtmesh score](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-score.svg)](https://qtmesh.dev)
[![qtmesh errors](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-errors.svg)](https://qtmesh.dev)
[![qtmesh warnings](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-warnings.svg)](https://qtmesh.dev)
[![qtmesh models](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-models.svg)](https://qtmesh.dev)
[![qtmesh animations](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-animations.svg)](https://qtmesh.dev)
[![qtmesh skeletons](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-skeletons.svg)](https://qtmesh.dev)
[![qtmesh materials](https://api.qtmesh.dev/v1/u/u-28680b9c/p/qtmesheditor/badges/qtmesh-materials.svg)](https://qtmesh.dev)

---

### 🔌 CI/CD — Validate Assets on Every PR

Available on the [GitHub Actions Marketplace](https://github.com/marketplace/actions/qtmesheditor). Scan, validate, convert, and optimize 3D assets in any workflow:

```yaml
# Scan all assets for issues (fails on warnings)
- uses: fernandotonon/QtMeshEditor@v1
  with:
    command: scan
    input-file: ./assets
    options: --fail-on warning
```

<details>
<summary>More CI examples</summary>

```yaml
# Validate a specific mesh
- uses: fernandotonon/QtMeshEditor@v1
  with:
    command: validate
    input-file: ./models/character.fbx

# Convert FBX → glTF
- uses: fernandotonon/QtMeshEditor@v1
  with:
    command: convert
    input-file: ./models/character.fbx
    output-file: ./output/character.gltf2

# Resample Mixamo animations (200+ keyframes → 30)
- uses: fernandotonon/QtMeshEditor@v1
  with:
    command: anim
    input-file: ./animations/dance.fbx
    output-file: ./output/dance_optimized.fbx
    options: --resample 30

# Get mesh info as JSON
- uses: fernandotonon/QtMeshEditor@v1
  id: info
  with:
    command: info
    input-file: ./models/character.fbx
    options: --json

# Docker (alternative)
docker run --rm -v $(pwd):/workspace ghcr.io/fernandotonon/qtmesh scan ./assets --fail-on error
```

</details>

### ☁️ QtMesh Cloud Badges (Recommended)

Register your repository in [QtMesh Cloud](https://qtmesh.dev) to publish real scan badges from CI.

1. Sign in at [qtmesh.dev](https://qtmesh.dev) and create a project (choose a slug like `my-game-assets`).
2. Create a project token in QtMesh Cloud.
3. Add the token as a GitHub secret named `QTMESH_CLOUD_TOKEN`.
4. Upload each `scan` JSON report from CI to `https://api.qtmesh.dev/v1/ingest/scan`.

Example upload step:

```yaml
- name: Scan assets
  run: |
    docker run --rm \
      -v "${{ github.workspace }}:/workspace" \
      -w /workspace \
      ghcr.io/fernandotonon/qtmesh:latest \
      scan --config /workspace/qtmesh.yml --json > qtmesh-scan-report.json

- name: Upload scan to QtMesh Cloud
  env:
    QTMESH_CLOUD_TOKEN: ${{ secrets.QTMESH_CLOUD_TOKEN }}
    QTMESH_CLOUD_API_URL: https://api.qtmesh.dev
  run: |
    jq --arg branch "${GITHUB_REF_NAME}" \
       --arg sha "${GITHUB_SHA}" \
       --arg runId "${GITHUB_RUN_ID}" \
       '. + {meta: {branch: $branch, commitSha: $sha, runId: $runId}}' \
       qtmesh-scan-report.json > qtmesh-scan-upload.json

    curl --fail --silent --show-error \
      -X POST "${QTMESH_CLOUD_API_URL}/v1/ingest/scan" \
      -H "Authorization: Bearer ${QTMESH_CLOUD_TOKEN}" \
      -H "Content-Type: application/json" \
      --data-binary @qtmesh-scan-upload.json
```

Badge markdown (replace `<project-slug>`):

```md
[![qtmesh status](https://api.qtmesh.dev/v1/projects/<project-slug>/badges/qtmesh-status.svg)](https://qtmesh.dev)
[![qtmesh errors](https://api.qtmesh.dev/v1/projects/<project-slug>/badges/qtmesh-errors.svg)](https://qtmesh.dev)
[![qtmesh warnings](https://api.qtmesh.dev/v1/projects/<project-slug>/badges/qtmesh-warnings.svg)](https://qtmesh.dev)
```

### 🏷️ Self-Hosted Scan Badges (Legacy)

You can also generate Shields-compatible endpoint JSON badges from `scan` results and host them yourself (for example via GitHub Pages).

```yaml
name: QtMesh Badges

on:
  push:
    branches: [main]
  workflow_dispatch:

permissions:
  contents: write

jobs:
  scan-and-publish-badges:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Run scan + generate badge JSON files
        id: scan
        uses: fernandotonon/QtMeshEditor@v1
        with:
          command: scan
          input-file: .
          options: --config /workspace/qtmesh.yml --json
          generate-badges: true
          badge-output-dir: badges
          badge-label-prefix: qtmesh
          badge-base-url: https://<USER>.github.io/<REPO>/badges

      - name: Publish badges to gh-pages/badges
        uses: peaceiris/actions-gh-pages@v4
        with:
          github_token: ${{ secrets.GITHUB_TOKEN }}
          publish_dir: ./badges
          destination_dir: badges
          keep_files: true
```

Example badge markdown (after publishing `badges/*.json`):

```md
[![qtmesh status](https://img.shields.io/endpoint?url=https%3A%2F%2F<USER>.github.io%2F<REPO>%2Fbadges%2Fqtmesh-status.json)](https://github.com/<USER>/<REPO>/actions)
[![qtmesh errors](https://img.shields.io/endpoint?url=https%3A%2F%2F<USER>.github.io%2F<REPO>%2Fbadges%2Fqtmesh-errors.json)](https://github.com/<USER>/<REPO>/actions)
[![qtmesh warnings](https://img.shields.io/endpoint?url=https%3A%2F%2F<USER>.github.io%2F<REPO>%2Fbadges%2Fqtmesh-warnings.json)](https://github.com/<USER>/<REPO>/actions)
```

Generated files:

- `qtmesh-status.json`
- `qtmesh-errors.json`
- `qtmesh-warnings.json`
- `qtmesh-passed.json`
- `qtmesh-scanned.json`
- `qtmesh-skipped.json`

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
```

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
| **macOS** | `brew tap fernandotonon/qtmesheditor && brew install qtmesheditor` |
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
| 3DS | .3ds | ✅ | ✅ | — |
| PLY | .ply | ✅ | ✅ | — |

Import supports all formats provided by Assimp (40+).

---

📖 [Documentation](https://fernandotonon.github.io/QtMeshEditor/docs.html) · 🛠 [Build from source](https://github.com/fernandotonon/QtMeshEditor/wiki/How-to-build) · 🐛 [Report issues](https://github.com/fernandotonon/QtMeshEditor/issues) · 💬 [Contribute](https://github.com/fernandotonon/QtMeshEditor)

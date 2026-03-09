
# <img width=30 align="top" src="https://user-images.githubusercontent.com/996529/209745977-7b797223-46ce-4bce-aa70-707a88f2aaf2.png"> QtMeshEditor
A free, open-source 3D asset tool for indie game developers — merge animations from multiple files, convert between 40+ formats, edit materials with AI, and more.

[![GitHub stars](https://img.shields.io/github/stars/fernandotonon/QtMeshEditor.svg?style=social&label=Star&maxAge=2592000)](https://GitHub.com/fernandotonon/QtMeshEditor/stargazers) Star if you like it!

[![Github All Releases](https://img.shields.io/github/downloads/fernandotonon/QtMeshEditor/total.svg)]()
[![Deploy](https://github.com/fernandotonon/QtMeshEditor/actions/workflows/deploy.yml/badge.svg)](https://github.com/fernandotonon/QtMeshEditor/actions/workflows/deploy.yml)
[![Coverage](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=coverage)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Maintainability Rating](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=sqale_rating)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Vulnerabilities](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=vulnerabilities)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Technical Debt](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=sqale_index)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Duplicated Lines (%)](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=duplicated_lines_density)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)

### :sparkles: Merge Mixamo Animations in Seconds

Download individual animations from [Mixamo](https://www.mixamo.com), drop them into QtMeshEditor, and merge them into a single mesh — export as glTF, Collada, OBJ, or Ogre Mesh for your game engine of choice.

![Merge Animations Demo](https://github.com/user-attachments/assets/16e81f29-f64c-402d-aac4-dbb1bc2f3aef)

### :movie_camera: More in Action

Split View|Skeleton Animation Controls
---|---
![QtMeshEditor1 5 0](https://user-images.githubusercontent.com/996529/210196572-7b49da4c-c5db-406d-9ab4-7fa20bacb6ae.gif)|![QtMeshEditor1 6 0](https://user-images.githubusercontent.com/996529/218779819-0a61156d-c014-4ad1-aa8b-cee900c9da56.gif)
**MCP tools (AI Agent Control)**|**Bone Weight Visualization**
![Gravação de Tela 2026-02-18 às 20 57 42 (1)](https://github.com/user-attachments/assets/ed3b7e9d-22ba-4e6e-a06c-868570db7a07)|![Gravação de Tela 2026-02-18 às 21 20 55](https://github.com/user-attachments/assets/289403ac-8952-488c-bc65-0a768ab278e1)


#### :robot: AI-enhanced Material Editor

![Screencast From 2025-06-26 23-35-53](https://github.com/user-attachments/assets/c58978d7-7564-41f2-8c95-527ddf7ae78e)


### :video_game: Built for Indie Game Developers

QtMeshEditor helps you prepare 3D assets for your game or project:

- **Merge animations** — Combine multiple animation files (e.g. from Mixamo) into one mesh with all animations
- **Convert between 40+ formats** — Import FBX, glTF, OBJ, Collada, STL, and more; export to what your engine needs
- **Edit materials visually** — Real-time material preview with AI-assisted generation
- **Inspect skeletons & animations** — Visualize bones, bone weights, preview animations, rename them
- **AI agent integration** — Let AI tools like Claude, Cursor, or custom scripts control the editor via MCP protocol
- **Batch process via CLI** — Automate your asset pipeline from the command line

### :wrench: CLI Pipeline (`qtmesh`)

A `qtmesh` command is created alongside the GUI binary during build. Use it to automate your 3D asset pipeline without launching the editor.
On Windows, use the bundled `qtmesh.cmd` wrapper (or add the install directory to `PATH`):

```bash
# Inspect a mesh
qtmesh info model.fbx                # human-readable summary
qtmesh info model.fbx --json         # JSON output for scripting

# Convert between formats
qtmesh convert model.fbx -o model.gltf2
qtmesh convert model.dae -o model.mesh

# Fix / optimize a mesh
qtmesh fix model.fbx -o fixed.fbx              # standard optimizations
qtmesh fix model.fbx -o fixed.fbx --all        # all fixes (remove degenerates, merge materials)

# Animation tools
qtmesh anim model.fbx --list                   # list animations
qtmesh anim model.fbx --list --json            # list as JSON
qtmesh anim model.fbx --rename "Take 001" "Idle" -o renamed.fbx

# Merge animations from multiple files
qtmesh anim base.fbx --merge walk.fbx run.fbx -o merged.fbx
```

Use `--verbose` for engine debug output, `--help` for full usage.

### :package: Format Support

| Format | Extension | Import | Export | Skeleton/Animation |
|--------|-----------|--------|--------|--------------------|
| FBX Binary | .fbx | Yes | Yes | Yes |
| glTF 2.0 | .gltf2 | Yes | Yes | Yes |
| glTF 2.0 Binary | .glb2 | Yes | Yes | Yes |
| Collada | .dae | Yes | Yes | Yes |
| OBJ | .obj | Yes | Yes | No |
| STL | .stl | Yes | Yes | No |
| Ogre Mesh | .mesh | Yes | Yes | Yes |
| Ogre XML | .mesh.xml | Yes | Yes | Yes |
| DirectX X | .x | Yes | Yes | Yes |
| PLY | .ply | Yes | Yes | No |
| 3DS | .3ds | Yes | Yes | No |
| Assimp Binary | .assbin | No | Yes | Yes |

Import supports all formats provided by Assimp (40+). Export to older Ogre Mesh versions (v1.0-v1.10) is also available.

### :computer: Install from release binaries
#### :apple: macOS
##### Homebrew
`brew tap fernandotonon/qtmesheditor`

`brew install qtmesheditor`

Remove with

`brew remove qtmesheditor`

Upgrade with

`brew upgrade qtmesheditor`

<img width="502" alt="image" src="https://github.com/fernandotonon/QtMeshEditor/assets/996529/84f56be3-4522-45a7-9039-5a143de7313c">


##### DMG file
* Download and open the .dmg file;
* Drag and drop the QtMeshEditor to the Applications folder:

![install_macOS](https://user-images.githubusercontent.com/996529/216797862-2592a40b-5f3d-4907-bcad-dc1feae4ff2f.gif)

#### :penguin: Linux (ubuntu)

Download the .deb file;
Then there are a few options for installing it:
* Using apt
`sudo apt install ./qtmesheditor_amd64.deb`

* Using dpkg
`sudo dpkg -i qtmesheditor_amd64.deb`

* Using the Software Install:
<img width="600" src="https://user-images.githubusercontent.com/996529/216799515-51494c16-c420-4535-a898-5b915a340c88.png">

Remove with

`sudo apt remove qtmesheditor`

or

`sudo dpkg -r qtmesheditor`

Run it calling `qtmesheditor`

#### :window: Windows
Unpack the binaries and run the `QtMeshEditor.exe` file

### [Build-from-source](https://github.com/fernandotonon/QtMeshEditor/wiki/How-to-build)

Feel free to contact me, create issues or to contribute ;)

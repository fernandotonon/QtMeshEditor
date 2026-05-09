# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Configure (macOS arm64 example):**
```bash
cmake . -B build_local -DCMAKE_PREFIX_PATH="/path/to/Qt/6.9.1/macos;/path/to/ogre/SDK_arm64" -DCMAKE_OSX_ARCHITECTURES=arm64
```

**Build:**
```bash
cmake --build build_local --target QtMeshEditor -j4
```

**Run (macOS):**
```bash
./build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor
```

**Build and run tests:**
```bash
cmake . -B build_local -DBUILD_TESTS=ON -DCMAKE_PREFIX_PATH="..."
cmake --build build_local --target UnitTests -j4
./build_local/bin/UnitTests                    # all tests
./build_local/bin/UnitTests --gtest_filter="Manager*"  # single test suite
```

**Run with MCP server:**
```bash
./build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor --with-mcp          # GUI + MCP
./build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor --mcp               # headless MCP only
./build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor --with-mcp --http-port 8080  # with HTTP API
```

**CLI pipeline (`qtmesh`):**
```bash
# A 'qtmesh' symlink is created automatically during build
qtmesh info model.fbx                          # show mesh info (text)
qtmesh info model.fbx --json                   # show mesh info (JSON)
qtmesh convert model.fbx -o model.gltf2        # convert between formats
qtmesh fix model.fbx -o fixed.fbx              # re-import/export with standard optimizations
qtmesh fix model.fbx --all                     # apply all extra fixes (remove degenerates, merge materials)
qtmesh anim model.fbx --list                   # list animations
qtmesh anim model.fbx --list --json            # list animations (JSON)
qtmesh anim model.fbx --rename "Take 001" "Idle" -o out.fbx  # rename an animation
qtmesh anim base.fbx --merge walk.fbx run.fbx -o merged.fbx
qtmesh anim model.fbx --resample 30 -o optimized.fbx  # resample to 30 keyframes
qtmesh anim model.fbx --decimate-step 5 -o lighter.fbx  # keep every 5th keyframe
qtmesh anim model.fbx --resample 30 --animation "Walk" -o out.fbx  # resample specific animation
qtmesh anim model.fbx --bake-fps 30 -o uniform.fbx     # re-grid every track to uniform 30 FPS
qtmesh anim model.fbx --bake-fps 60 --animation "Run" -o out.fbx  # bake one animation at 60 FPS
qtmesh pose model.fbx --animation "Walk" --time 0.5 -o posed.stl  # export single frame
qtmesh pose model.fbx --animation "Dance" --count 4 -o pose_%02d.stl  # export N evenly spaced frames
qtmesh validate model.fbx                      # validate mesh (exit 1 if errors found)
qtmesh validate model.fbx --json               # validation results as JSON
qtmesh lod model.fbx --info                    # show LOD levels
qtmesh lod model.fbx --info --json             # LOD info as JSON
qtmesh lod model.fbx --count 3                 # generate 3 LODs → model_lod1.fbx, model_lod2.fbx, model_lod3.fbx
qtmesh lod model.fbx --count 2 --reductions 0.25,0.5 -o out.fbx  # custom reductions, named output
qtmesh lod model.fbx --auto                    # auto-generate LODs
qtmesh lod model.fbx --remove -o clean.fbx     # strip LODs and save
qtmesh material model.fbx --preset "Metallic-Roughness" -o out.fbx  # apply a built-in material preset (writes .material sidecar)
qtmesh material --list-presets                 # list built-in preset names (incl. PBR templates)
qtmesh scan ./assets                           # scan directory for asset issues
qtmesh scan ./assets --config qtmesh.yml       # use YAML config file
qtmesh scan ./assets --json                    # JSON output
qtmesh scan ./assets --report report.json      # write JSON report to file
qtmesh scan ./assets --sarif report.sarif      # write SARIF report to file
qtmesh scan ./assets --fix --dry-run           # preview auto-fixes
qtmesh scan ./assets --include "*.fbx,*.glb"   # filter by extension
qtmesh scan ./assets --fail-on warning         # exit 1 on warnings or errors
```

CLI mode is activated by: (1) invoking via the `qtmesh` symlink, (2) passing `--cli`, or (3) using a recognized subcommand (`info`, `fix`, `convert`, `anim`, `validate`, `lod`, `pose`, `scan`, `material`) as the first argument. Use `--verbose` to see Ogre/engine debug output. Use `--no-telemetry` to permanently opt out of anonymous usage data collection.

If Xcode SDK is updated, clear CMake cache (`rm build_local/CMakeCache.txt`) and reconfigure.

## Dependencies

- **Qt 6.9.3**: Core, Widgets, Gui, QuickWidgets, Quick, Qml, Network, QuickControls2, Test
- **Ogre3D 14.5.x**: 3D rendering engine
- **Assimp 6.0.4**: 3D model import/export
- **llama.cpp**: Optional local LLM inference (enabled by default, disable with `-DENABLE_LOCAL_LLM=OFF`)
- **stable-diffusion.cpp**: Optional AI texture generation (disabled by default, enable with `-DENABLE_STABLE_DIFFUSION=ON`)
- **Google Test**: Test framework (enabled with `-DBUILD_TESTS=ON`)
- **ogre-procedural**: Bundled in `src/dependencies/ogre-procedural/` for procedural mesh generation

## Architecture

### Singleton Pattern (Central to the codebase)

Three singletons manage core state. All run on the main thread. Access via `ClassName::getSingleton()` or `ClassName::getSingletonPtr()`. Destroy with `ClassName::kill()`.

- **Manager** (`src/Manager.h/cpp`): Owns Ogre::Root, SceneManager, tracks all SceneNodes and Entities. Emits `sceneNodeCreated`, `entityCreated`, `sceneNodeDestroyed` signals.
- **SelectionSet** (`src/SelectionSet.h/cpp`): Tracks selected SceneNodes, Entities, and SubEntities. Emits `selectionChanged` and related signals. Provides selection geometry (center, orientation, scale).
- **TransformOperator** (`src/TransformOperator.h/cpp`): Implements SELECT/TRANSLATE/ROTATE/SCALE modes with gizmos. Handles ray/box selection via Ogre scene queries. Implements `QtMouseListener` interface.

### Qt-Ogre Integration

- **OgreWidget** (`src/OgreWidget.h/cpp`): QWidget subclass that creates an Ogre::RenderWindow from the native window handle.
- **EditorViewport** (`src/EditorViewport.h/cpp`): Wraps OgreWidget, runs render loop via QTimer.
- **MainWindow** (`src/mainwindow.h/cpp`): QMainWindow + Ogre::FrameListener. Contains viewports, toolbars, dock widgets. Right sidebar hosts the QML Inspector panel directly (no tab widget). Animation Control dock at bottom auto-shows for animated entities.

### Material Editor (QML)

- **MaterialEditorQML** (`src/MaterialEditorQML.h/cpp`): QML_SINGLETON exposing full Ogre material property access (colors, lighting, depth, blending, fog, textures) with undo/redo. QML UI in `qml/`.

### Debug Overlays

- **NormalVisualizer** (`src/NormalVisualizer.h/cpp`): Draws vertex normals as colored lines (|X|=Red, |Y|=Green, |Z|=Blue). Toggled globally via Options → Show Normals menu or MCP `toggle_normals` tool. Supports real-time animation: requests software-skinned normals via `addSoftwareAnimationRequest(true)` and updates each frame for skeletal entities. Overlays attach to dedicated child scene nodes to avoid unsafe `static_cast<Entity*>` crashes in `ObjectItemModel` and `Manager::getEntities()`.
- **MeshInfoOverlay** (`src/MeshInfoOverlay.h/cpp`): Floating overlay showing mesh statistics (vertices, triangles, submeshes, materials, bones, animations) on the active viewport. Shows stats for selected entities or aggregated scene stats. Toggled via Options → Show Mesh Info menu or MCP `toggle_mesh_info` tool. Implemented as a top-level `Qt::Tool` window to avoid ghost-text artifacts from Ogre's direct-to-native rendering (`WA_PaintOnScreen`).
- **BoneWeightOverlay** (`src/BoneWeightOverlay.h/cpp`): Per-entity bone weight heat-map overlay.

### ViewCube

- **ViewCubeController** (`src/ViewCube/ViewCubeController.h/cpp`): QML_SINGLETON that bridges the 3D navigation cube overlay with the active OgreWidget/SpaceCamera. Tracks camera orientation quaternion, manages overlay positioning, and provides snap-to-face/corner/direction + arcball drag rotation.
- **ViewCubeWindow.qml** (`qml/ViewCubeWindow.qml`): QML Canvas2D rendering a 3D cube with face/edge/corner hit-testing. Uses quaternion-to-rotation-matrix conversion with negated qx to match Ogre's camera rig convention.
- Visibility requires both the toggle (`setVisible`) and an active widget — hides automatically when viewports are closed and reappears when a new viewport gets focus.
- The QML window uses `Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint` and software rendering (`QQuickWindow::setSceneGraphBackend("software")`) to avoid GL conflicts with Ogre.

### Transform System

- **TransformOperator** (`src/TransformOperator.h/cpp`): Singleton implementing SELECT/TRANSLATE/ROTATE/SCALE modes. Owns three gizmos (TranslationGizmo, RotationGizmo, ScaleGizmo). Supports WORLD/LOCAL transform space. Mouse interaction: ray-cast gizmo for axis selection, plane intersection for drag transforms.
- **ScaleGizmo** (`src/ScaleGizmo.h/cpp`): Scale gizmo with cube handles at axis endpoints. Follows TranslationGizmo pattern (ManualObject per axis, highlight/fade).
- **Keyboard shortcuts** (Unity convention): `Q`=Select, `W`=Translate, `E`=Rotate, `R`=Scale, `F`=Frame selection (in Edit Mode with a fillable selection: Fill instead), `X`=Toggle World/Local space (in Edit Mode with a selection: Delete; `Ctrl+X`=Dissolve).
- **SpaceCamera::frameSelection()**: Computes bounding sphere of selection and positions camera to fit it in view.

### Edit Mode (Phase 4 topology)

- **EditModeController** (`src/EditModeController.h/cpp`): QML_SINGLETON managing Object/Edit mode state. `Tab` toggles. In edit mode, `1`/`2`/`3` switch Vertex/Edge/Face component selection.
- **HalfEdgeMesh** (`src/HalfEdgeMesh.h/cpp`): Half-edge data structure built per-operation from EditableMesh, used for adjacency queries and topology mutations. Operations: `extrudeFaces`, `extrudeEdges`, `bevelEdges`, `bevelVertices`, `splitEdge`, `splitFace`, `cutPath` (knife), `mergeVertices`, `mergeVerticesByDistance`, `deleteFaces/Edges/Vertices`, `dissolveEdges/Vertices`, `subdivideFaces`, `fillSelection`. All push a single `EditMeshTopologyCommand` for undo.
- **Subdivide**: 1-to-4 triangle split. Adjacent non-selected faces are retriangulated against the new midpoints to avoid T-junctions (1/2/3 split-edge cases). Wired to a toolbar button (⊞) — face mode subdivides selected tris, edge mode subdivides every triangle incident to a selected edge.
- **Fill**: vertex mode fan-triangulates the selected verts (3 → triangle, 4 → quad, N → N-2 tris); edge mode detects a closed boundary loop via degree-2 walk and caps it. Toolbar button (◆) and `F` shortcut. Cross-submesh inputs and duplicates of existing triangles are rejected.

### Undo/Redo System

- **UndoManager** (`src/UndoManager.h/cpp`): Singleton wrapping `QUndoStack`. Push commands, undo/redo via `Ctrl+Z`/`Ctrl+Shift+Z`.
- **TransformCommands** (`src/commands/TransformCommands.h/cpp`): `TranslateCommand`, `RotateCommand`, `ScaleCommand`, `DeleteCommand`. Translate and Scale support command merging. State captured on mouse press, command pushed on mouse release in TransformOperator.

### QML Inspector Panel

- **PropertiesPanelController** (`src/PropertiesPanelController.h/cpp`): QML_SINGLETON providing transform values, selection state, scene tree model, primitive parameters, animation data (enable/loop/rename), skeleton debug toggles. Bridges all scene data to QML.
- **SceneTreeModel** (`src/SceneTreeModel.h/cpp`): QAbstractItemModel exposing hierarchical scene tree (Nodes → Entities → SubEntities) to QML. Supports multi-select, material name get/set on submeshes, debounced rebuild on scene changes.
- **PropertiesPanel.qml** (`qml/PropertiesPanel.qml`): Main inspector with collapsible sections:
  - **Scene** — recursive tree view (SceneTreeNode.qml) with expand/collapse, Ctrl+click multi-select, material typeahead dropdown on submeshes
  - **Transform** — position/rotation/scale spinbox fields with up/down arrow keys and buttons
  - **Primitive** — context-sensitive fields per primitive type (size, radius, height, segments, UV)
  - **Animations** — per-entity groups with enable/loop checkboxes, double-click rename, play/pause, skeleton/weights toggles
- **CollapsibleSection.qml**, **SceneTreeNode.qml**, **TransformField.qml** — reusable QML components.
- Loaded as QQuickWidget directly in the right dock (replaces old tab widget with Transform/Material/Edit/Animation tabs).

### Theme System

- **ThemeManager** (`src/ThemeManager.h/cpp`): QML_SINGLETON providing canonical theme colors synced from QPalette. All colors (window, panel, header, text, button, highlight, border, accent) derived from the active QPalette.

### Indie Game Dev Features

- **BatchExporter** (`src/BatchExporter.h/cpp`): Multi-file conversion wrapping CLIPipeline. Supports progress reporting.
- **MaterialPresetLibrary** (`src/MaterialPresetLibrary.h/cpp`): QML_SINGLETON providing one-click material presets (Plastic, Metal, Wood, Glass, Unlit, Wireframe).

### MCP Server

- **MCPServer** (`src/MCPServer.h/cpp`): JSON-RPC 2.0 over stdio + HTTP REST API on configurable port.
- Runs on main thread via QSocketNotifier. **Never use BlockingQueuedConnection** (causes deadlock).
- Launch modes: `--mcp` (headless), `--with-mcp` (GUI + MCP).
- stdout is redirected to stderr to isolate MCP JSON-RPC from Ogre/Qt debug output; original stdout fd saved for MCP responses.
- HTTP API uses QTcpServer with deferred tool execution (QTimer::singleShot) to avoid re-entrant crashes from Ogre event processing.

### CLI Pipeline

- **CLIPipeline** (`src/CLIPipeline.h/cpp`): Headless command-line interface for mesh operations. All static methods — entry point is `CLIPipeline::run(argc, argv)`.
- Subcommands: `info`, `fix`, `convert`, `anim` (list/rename/merge), `validate`, `lod`, `pose`, `scan`.
- Activated via `qtmesh` symlink (created at build time), `--cli` flag, or recognized subcommand as first arg.
- Redirects stdout to stderr (Ogre/Qt noise) and writes CLI output to the original stdout fd. Uses `_exit()` to avoid Ogre static destructor crashes on macOS.
- **AnimationMerger** (`src/AnimationMerger.h/cpp`): Public `renameAnimation()` static method used by both CLI and GUI for animation renaming.
- **ScanEngine** (`src/ScanEngine.h/cpp`): Directory scanner for 3D asset linting. Uses Assimp directly (no Ogre needed) for lightweight metadata extraction. Enumerates files via glob patterns, evaluates configurable rules (format restrictions, size/complexity limits, naming conventions, texture existence), produces text/JSON/SARIF reports. Supports auto-fix for naming convention violations.
- **ScanConfig** (`src/ScanConfig.h/cpp`): Config loader for `qtmesh.yml`/`.json`. Includes a minimal YAML parser for the specific config schema (scalars, inline/block lists, one level of section nesting). Supports scan paths, rule configuration, fix behavior, and report output settings.

### Mesh Import/Export

- **MeshImporterExporter** (`src/MeshImporterExporter.h/cpp`): Static methods. Supports .mesh, .obj, .dae, .gltf, .fbx via custom Assimp processors in `src/Assimp/`. Also provides `sceneExporter()`/`sceneImporter()` for saving/loading entire scenes (multiple entities with transforms, materials, skeletons, and animations) as glTF files. Multi-entity scenes use entity-name-prefixed bones to avoid cross-entity skeleton contamination when Assimp merges skins. **Auto-scales sub-unit meshes**: assets with bounding-box max-extent below 0.01 (mm-scale FBX, photogrammetry, etc.) get their parent SceneNode scaled by `1/maxExtent` so the largest dim lands at ~1 unit — without this they sit inside the camera near-clip plane and never render. `configureCamera()` reads `getWorldBoundingBox(derive=true)` so the camera distance accounts for the auto-scale.
- **FBXExporter** (`src/FBX/FBXExporter.h/cpp`): Custom FBX Binary v7300 exporter that writes directly from Ogre data. Handles geometry, skeleton, skin deformers, animations, and materials. Replaces Assimp's broken FBX exporter.
- **MaterialProcessor** (`src/Assimp/MaterialProcessor.h/cpp`): Builds Ogre::Material from Assimp aiMaterial. Reads legacy `aiTextureType_DIFFUSE` / `_NORMALS` / `_HEIGHT` plus PBR types (`_BASE_COLOR`, `_METALNESS`, `_DIFFUSE_ROUGHNESS` with `_SHININESS` fallback, `_AMBIENT_OCCLUSION`, `_EMISSIVE`) and binds them to the slice E canonical PBR slot names (`albedo`, `metallic`, `roughness`, `ao`, `emissive`) so PBR-aware tooling sees populated slots even on FBX/glTF imports. When no `BASE_COLOR` is exposed but a legacy `aiTextureType_DIFFUSE` was, the importer aliases the diffuse texture under `albedo` (non-FFP). Pass is **not** tagged `pbr_workflow` on import — that would auto-promote to `SRS_COOK_TORRANCE_LIGHTING` via the `applyNormalMap` redirect, producing dark output without IBL. A future slice may expose a "Convert to PBR" inspector action that adds the tag deliberately when IBL is in place.

### Local LLM

- **LLMManager** (`src/LLMManager.h/cpp`): QML_SINGLETON wrapping llama.cpp for local inference.
- **LLMWorker**: Runs inference in a worker thread.
- **ModelDownloader**: Downloads GGUF models from HuggingFace.

### AI Texture Generation

- **SDManager** (`src/SDManager.h/cpp`): QML_SINGLETON managing stable-diffusion.cpp for AI texture generation. Mirrors LLMManager pattern with worker thread, model management, QSettings persistence.
- **SDWorker** (`src/SDWorker.h/cpp`): Worker thread wrapping stable-diffusion.cpp C API. Handles model loading (`new_sd_ctx`), image generation (`generate_image`), and progress callbacks.
- Integration: `MaterialEditorQML` connects to SDManager signals. Generated textures are saved as PNG, registered as Ogre resource locations, and applied to the current material's texture unit.
- Models stored in `<AppData>/sd_models/`. Supports `.safetensors`, `.ckpt`, `.gguf` formats.
- `#ifdef ENABLE_STABLE_DIFFUSION` guards all sd.cpp includes/calls. Feature is OFF by default.
- When both features are enabled, sd.cpp and llama.cpp share the same ggml dependency managed by CMake.

## Development Guidelines

- **UI: QML over Widgets.** New UI should be built in QML (Qt Quick), not Qt Widgets. The Inspector panel (`qml/PropertiesPanel.qml`) and Material Editor (`qml/MaterialEditorWindow.qml`) are the reference for the QML approach. The old Transform/Material/Edit/Animation tabs have been replaced by the QML Inspector. AnimationWidget and PrimitivesWidget still exist as hidden backing widgets but are not user-visible tabs.
- **Cross-platform: Windows, Linux (Ubuntu), macOS.** All code must compile and run on all three. Guard platform-specific APIs with `#ifdef Q_OS_WIN`, `#ifdef Q_OS_MACOS`, `#ifdef Q_OS_LINUX`. Test the CI build across all three platforms before merging.
- **Sentry breadcrumbs.** All user-facing actions and significant operations must be tracked with `SentryReporter::addBreadcrumb(category, message)`. Use `"ui.action"` for toolbar/menu clicks, `"ai.tool_call"` for MCP tool invocations, `"file.import"` / `"file.export"` for I/O operations. This enables crash diagnostics and usage telemetry. Check existing patterns in `mainwindow.cpp`, `TransformOperator.cpp`, and `MCPServer.cpp`.
- **Unit tests.** Add Google Test unit tests for new functionality. Test files live alongside source in `src/` with the `_test.cpp` suffix (e.g., `Manager_test.cpp`). CI runs tests only on Linux to save budget, so:
  - Features that depend on optional components (e.g., local LLM / llama.cpp) may not be available in the test environment — guard with `#ifdef ENABLE_LOCAL_LLM` or skip gracefully.
  - Tests must work under Xvfb (headless X11) — avoid assumptions about a real display.

## Platform-Specific Notes

- **macOS**: `macBundlePath()` returns `.app` bundle root (not `Contents/MacOS/`). Ogre resource paths in `resources.cfg` resolve relative to this. AGL framework stub created at configure time for newer macOS.
- **Windows**: MinGW build. `<execinfo.h>` (`backtrace`, `backtrace_symbols_fd`) and `<unistd.h>` (`dup`, `STDERR_FILENO`, `SIGBUS`) are unavailable — guard with `#ifndef Q_OS_WIN`.
- **Linux**: Requires Xvfb for headless Qt testing in CI.

## Ogre API Pitfalls

- `Ogre::MaterialSerializer::parseScript()` does **not** exist in Ogre 14.x. Use `MaterialManager::getSingleton().create(name, group)` and set properties via the API.
- To serialize a material to string: `serializer.queueForExport(mat)` then `serializer.getQueuedAsString()`.
- `Manager::getEntities()` returns all attached objects — you must check `obj->getMovableType() == "Entity"` before casting to `Ogre::Entity*` (ManualObjects will crash otherwise).

## Versioning

The **single source of truth** for the application version is in `CMakeLists.txt`:
```cmake
project(QtMeshEditor VERSION X.Y.Z LANGUAGES CXX)
```
All other version references are auto-generated from this via CMake template substitution (`@PROJECT_VERSION@`):
- `src/Info.plist.in` — macOS bundle Info.plist
- `cfg/version.txt.in` — runtime version file
- `DEBIAN-control.in` — Debian package control file

**To bump the version**, only edit the `VERSION` in `CMakeLists.txt` line 16. The rest updates automatically on rebuild.

**Version format: `X.Y.Z` only — never prepend `v`.** GitHub release tags, update check comparisons, and all version strings use plain `X.Y.Z` (e.g., `2.21.0`, not `v2.21.0`). The update check feature compares the runtime version against the latest GitHub release tag, so a `v` prefix would break the comparison.

Note: `MCPServer.h` has a separate `SERVER_VERSION` ("1.0.0") for the MCP protocol — only bump that if the MCP interface changes.

## Docker

A Docker image is published on each release to both `ghcr.io/fernandotonon/qtmesh` and `fernandotr1/qtmesh` (Docker Hub).

**Run via Docker:**
```bash
docker run --rm -v $(pwd):/workspace ghcr.io/fernandotonon/qtmesh info model.fbx --json
docker run --rm -v $(pwd):/workspace ghcr.io/fernandotonon/qtmesh convert model.fbx -o model.gltf2
```

**Key files:**
- `Dockerfile` — Ubuntu 24.04 base, installs the `.deb` release artifact, Xvfb for headless GL
- `docker-entrypoint.sh` — Starts Xvfb, routes CLI commands via `--cli` flag, MCP commands to `qtmesheditor`
- `.github/workflows/docker-publish.yml` — Manual `workflow_dispatch` for rebuilding images
- `.github/actions/qtmesh/action.yml` — Reusable composite action for CI/CD pipelines

**Notes:**
- The entrypoint passes `--cli` explicitly because the launcher script's `exec` changes argv[0] to contain "editor", which breaks CLI mode detection by binary name.
- The Docker base must be Ubuntu 24.04 (not 22.04) to match the CI build runner's GLIBC version.

## WinGet (Windows Package Manager)

QtMeshEditor is available via WinGet: `winget install FernandoTonon.QtMeshEditor`.

**Key files:**
- `winget/manifests/f/FernandoTonon/QtMeshEditor/` — local copy of the WinGet manifest (version, locale, installer YAML)
- `scripts/update-winget.sh` — generates updated manifest files for a new release
- `.github/workflows/deploy.yml` — `winget-publish` job auto-submits to microsoft/winget-pkgs on release

**Updating for new release:**
The `winget-publish` CI job uses `wingetcreate --submit` to automatically submit a PR to microsoft/winget-pkgs when a GitHub Release is published. Requires a `WINGET_TOKEN` secret (GitHub PAT with `public_repo` scope). **Do not use the git database API** (blobs/trees/commits endpoints) — that requires `repo` scope. `wingetcreate --submit` uses the Contents API which works with `public_repo`.

Manual alternative: `./scripts/update-winget.sh <version>` generates the manifest locally.

## GitHub Action (Marketplace)

The `qtmesh` CLI is published as a GitHub Action on the [GitHub Actions Marketplace](https://github.com/marketplace/actions/qtmesheditor). The `action.yml` lives at the repo root.

```yaml
- uses: fernandotonon/QtMeshEditor@v1
  with:
    command: scan
    input-file: ./assets
    options: --fail-on warning
```

**Key files:**
- `action.yml` — root-level action definition (required for marketplace)
- `.github/actions/qtmesh/action.yml` — legacy local action (kept for backward compatibility)
- Docker image: `ghcr.io/fernandotonon/qtmesh` (built from this repo on each release)
- Redirect repo: `fernandotonon/qtmesh` points users to this repo

**When to update action.yml:**
- New CLI subcommand added → update `command` description
- Subcommand flags change → update `options` description
- Docker image name/registry changes → update the `docker run` command
- **No update needed** for: bug fixes, GUI changes, MCP tools, or features that don't change CLI interface

The action uses `image-tag: latest` by default, so users automatically get fixes without version bumps.

**Marketplace publishing:** When creating a GitHub Release, check "Publish this Action to the GitHub Marketplace". The `v1` tag should be kept pointing to the latest stable commit (force-push tag on each release).

## CI/CD

GitHub Actions workflow in `.github/workflows/deploy.yml` builds for Windows (MinGW), macOS, and Linux. Tests run on Linux with SonarCloud coverage. Releases auto-update the Homebrew cask, WinGet package, Snap Store, and Docker image. A `scan-assets-docker` job runs the `fernandotonon/qtmesh` action on the repo's own test assets to validate the Docker image and scan pipeline on every push/PR.

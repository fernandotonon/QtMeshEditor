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
```

CLI mode is activated by: (1) invoking via the `qtmesh` symlink, (2) passing `--cli`, or (3) using a recognized subcommand (`info`, `fix`, `convert`, `anim`) as the first argument. Use `--verbose` to see Ogre/engine debug output.

If Xcode SDK is updated, clear CMake cache (`rm build_local/CMakeCache.txt`) and reconfigure.

## Dependencies

- **Qt 6.9.3**: Core, Widgets, Gui, QuickWidgets, Quick, Qml, Network, QuickControls2, Test
- **Ogre3D 14.5.x**: 3D rendering engine
- **Assimp 6.0.4**: 3D model import/export
- **llama.cpp**: Optional local LLM inference (enabled by default, disable with `-DENABLE_LOCAL_LLM=OFF`)
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
- **MainWindow** (`src/mainwindow.h/cpp`): QMainWindow + Ogre::FrameListener. Contains viewports, toolbars, dock widgets.

### Material Editor (QML)

- **MaterialEditorQML** (`src/MaterialEditorQML.h/cpp`): QML_SINGLETON exposing full Ogre material property access (colors, lighting, depth, blending, fog, textures) with undo/redo. QML UI in `qml/`.

### Debug Overlays

- **NormalVisualizer** (`src/NormalVisualizer.h/cpp`): Draws vertex normals as colored lines (|X|=Red, |Y|=Green, |Z|=Blue). Toggled globally via Options → Show Normals menu or MCP `toggle_normals` tool. Supports real-time animation: requests software-skinned normals via `addSoftwareAnimationRequest(true)` and updates each frame for skeletal entities. Overlays attach to dedicated child scene nodes to avoid unsafe `static_cast<Entity*>` crashes in `ObjectItemModel` and `Manager::getEntities()`.
- **BoneWeightOverlay** (`src/BoneWeightOverlay.h/cpp`): Per-entity bone weight heat-map overlay.

### MCP Server

- **MCPServer** (`src/MCPServer.h/cpp`): JSON-RPC 2.0 over stdio + HTTP REST API on configurable port.
- Runs on main thread via QSocketNotifier. **Never use BlockingQueuedConnection** (causes deadlock).
- Launch modes: `--mcp` (headless), `--with-mcp` (GUI + MCP).
- stdout is redirected to stderr to isolate MCP JSON-RPC from Ogre/Qt debug output; original stdout fd saved for MCP responses.
- HTTP API uses QTcpServer with deferred tool execution (QTimer::singleShot) to avoid re-entrant crashes from Ogre event processing.

### CLI Pipeline

- **CLIPipeline** (`src/CLIPipeline.h/cpp`): Headless command-line interface for mesh operations. All static methods — entry point is `CLIPipeline::run(argc, argv)`.
- Subcommands: `info`, `fix`, `convert`, `anim` (list/rename/merge).
- Activated via `qtmesh` symlink (created at build time), `--cli` flag, or recognized subcommand as first arg.
- Redirects stdout to stderr (Ogre/Qt noise) and writes CLI output to the original stdout fd. Uses `_exit()` to avoid Ogre static destructor crashes on macOS.
- **AnimationMerger** (`src/AnimationMerger.h/cpp`): Public `renameAnimation()` static method used by both CLI and GUI for animation renaming.

### Mesh Import/Export

- **MeshImporterExporter** (`src/MeshImporterExporter.h/cpp`): Static methods. Supports .mesh, .obj, .dae, .gltf, .fbx via custom Assimp processors in `src/Assimp/`.
- **FBXExporter** (`src/FBX/FBXExporter.h/cpp`): Custom FBX Binary v7300 exporter that writes directly from Ogre data. Handles geometry, skeleton, skin deformers, animations, and materials. Replaces Assimp's broken FBX exporter.

### Local LLM

- **LLMManager** (`src/LLMManager.h/cpp`): QML_SINGLETON wrapping llama.cpp for local inference.
- **LLMWorker**: Runs inference in a worker thread.
- **ModelDownloader**: Downloads GGUF models from HuggingFace.

## Development Guidelines

- **UI: QML over Widgets.** New UI should be built in QML (Qt Quick), not Qt Widgets. The project is migrating from Widgets to QML. The Material Editor (`qml/`) is the reference for the QML approach. Existing Widget-based UI (`ui_files/`) remains but should not be extended.
- **Cross-platform: Windows, Linux (Ubuntu), macOS.** All code must compile and run on all three. Guard platform-specific APIs with `#ifdef Q_OS_WIN`, `#ifdef Q_OS_MACOS`, `#ifdef Q_OS_LINUX`. Test the CI build across all three platforms before merging.
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

Note: `MCPServer.h` has a separate `SERVER_VERSION` ("1.0.0") for the MCP protocol — only bump that if the MCP interface changes.

## CI/CD

GitHub Actions workflow in `.github/workflows/deploy.yml` builds for Windows (MinGW), macOS, and Linux. Tests run on Linux with SonarCloud coverage. Releases auto-update the Homebrew cask.

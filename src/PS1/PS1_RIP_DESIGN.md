# PS1 Runtime Geometry Extraction — Design

Parent epic: [GitHub #412](https://github.com/fernandotonon/QtMeshEditor/issues/412)

## Goal

Extract geometry, textures, UVs, and vertex colors from PlayStation 1 games **at runtime** by embedding an emulator and intercepting GPU/GTE commands, instead of parsing every proprietary per-game format.

Static parsers under `src/PS1/` (`PS1TMD`, `PS1TIM`, `PS1RSD`, `PS1PLY`, `PS1MAT`) remain for known file formats.

## Emulator core comparison (Phase 0 — #413)

| Criterion | mednafen-psx | PCSX-Redux | duckstation libcore |
|-----------|--------------|------------|---------------------|
| License | GPLv2 | GPLv3 | GPLv3 core; app CC-BY-NC-ND |
| QtMeshEditor main binary | Must **not** link — plugin only | Plugin only | Plugin only |
| Maturity / accuracy | High | High; strong GPU debugger | High |
| Headless / hook surface | Good; documented sources | Lua GPU debugger (reference UX) | libcore hooks possible |
| Distribution risk | Low if isolated `.so` | Low if isolated | NC-ND blocks app bundle use |

### Recommendation

**Primary: mednafen-psx (GPLv2) in a dynamically loaded `EmuCore` plugin** (separate build artifact, not linked into the main QtMeshEditor binary).

**Rationale:**

1. **License** — GPLv2 is compatible with plugin isolation; duckstation’s NC-ND application terms are unsuitable for redistribution with QtMeshEditor.
2. **Hookability** — Mature, readable C++ core; GPU command stream is small and well-documented (nocash psx-spx).
3. **Headless** — Supports automation for unit tests and future `qtmesh ps1` CLI (#431).

PCSX-Redux remains a **reference** for GPU-debugger UX (#425–#426), not a linked dependency.

## Architecture (summary)

```
PS1RipManager (main thread, singleton)
    ├── PS1RipWorker (QThread) ──► EmuCore plugin (future)
    └── CaptureBuffer / Reconstructor (future phases)
```

- Captured data finalized under `<AppData>/ps1_rip/captures/<sessionId>/`.
- Reconstruction runs on the main thread on demand (no `BlockingQueuedConnection`).
- Sentry breadcrumbs: category `ps1.rip.*`.

## Build flag

```cmake
option(ENABLE_PS1_RIP "Enable experimental PS1 runtime geometry extraction" OFF)
```

- OFF by default for release binaries.
- CI enables ON for Linux test jobs only.

## Milestones

See epic #412 for phased issues (#413–#431).

## Phase 1 status

- `EmuCore` + `IEmuCorePlugin` + `EmuCoreLoader` landed (#415).
- **Libretro host** `plugins/ps1core_libretro/` loads `mednafen_psx_libretro` / `beetle_psx_*` from `PS1Cores/`, system paths, or `QTMESH_PS1_LIBRETRO_CORE`. Runs a real ISO when BIOS + disc image are set.
- Stub plugin `plugins/ps1core_stub/` remains for CI (test pattern + synthetic capture).
- `PS1RipSessionWindow` + `EmuViewport` (#416): software framebuffer via `QPainter`, integer-scale and bilinear toggles (View menu), 4:3 NTSC/PAL letterbox mode, FPS overlay, frame pacing tied to `runFrame()` completion (~16 ms target). Hosted in a temporary session window from *Tools → Experimental → PS1 Runtime Ripper…* until #425 dock lands.
- Legality dialog + BIOS/ISO + keyboard/gamepad (#417): first-run acknowledgement (`ps1Rip/acknowledged`), BIOS SHA-256 fingerprint logged (not enforced), ISO picker with recent list (last 5), **Reload ISO** transport, configurable keyboard mapping (`Input → Keyboard mapping…`), Qt Gamepad for controller 1 when `Qt6::Gamepad` is available.
- Install helper: `scripts/install-ps1-libretro-core.sh` copies a distro libretro core into `build/bin/PS1Cores/`.

## Phase 2 status

- `CaptureTypes`, `GpuCommandParser`, `CaptureBuffer`, `RipperHooks`, `Gp0HookDispatch` (#418).
- `EmuHooks` included from `EmuCore.h` (issue #418 API surface).
- **GTE capture (#419):** On **Capture Frame**, `PsxGteInstructionCapture` scans RAM for COP2 **RTPS/RTPT**, executes setup sequences via `PsxMipsGteRunner` + `PsxGteEngine`, then `PsxGteRamScanner` supplements with matrix blobs. GP0 ingest tags primitives with `latestMatrixId`. Hash dedupe + `cameraMatrixId` heuristic in the session status bar after mesh build. GTE RAM scans run on final capture ingest only (not on per-frame live GPU ticks).
- **GP0 capture (#657):** Three paths feed `RipperHooks::onGpuPrim`:
  - **Direct hook (`gp0_hook`):** `EmuHooks::submitGp0Words` — used by the **stub** core (synthetic seven-flavor capture via `onGpuPrim`) and reserved for true in-core mednafen FIFO hooks. Sentry breadcrumb `ps1.rip.capture.gp0_hook` records `source:gp0_hook|ram_ot|ram_linear|ram_chain_root` and per-path counts.
  - **Libretro live frame (`ram_*`):** While capture is armed, each `retro_run()` end triggers a lightweight RAM ingest (OT + standalone chain roots + linear, no GTE scan). Primitives accumulate with cross-frame dedupe until **Capture Frame** runs a final GTE+RAM merge. Disable live ingest with `QTMESH_PS1_GP0_LIVE_CAPTURE=0`. Baseline RAM-only behavior (OT then linear, no chain-root pass) via `QTMESH_PS1_GP0_RAM_LEGACY=1`.
  - **Merged RAM scan:** `PsxOrderingTableScanner` → `PsxGp0ChainRootScanner` → linear fallback share one dedupe set (no early return after a weak OT). OT entries use **24-bit absolute RAM pointers** (libgpu `getaddr` layout), with a relative-to-OT-base fallback for synthetic tests. Linked DR tags carry the opcode in bits 24–31 and the next packet address in bits 2–23 (`PsxGp0Opcode.h`).
- **Stub core** (`coreId=stub`): direct `onGpuPrim` hook path for CI when `QTMESH_PS1_FORCE_STUB=1` or no libretro core is present.
- **Libretro core** (`coreId=libretro`): live merged RAM capture at frame boundary; true mednafen GP0 FIFO dispatch hooks remain future work ([#662](https://github.com/fernandotonon/QtMeshEditor/issues/662)).
- `armCapture` / `captureFrame` wire capture to the worker thread; CSV dump to temp for verification.
- Sentry breadcrumb `ps1.rip.capture.frame_armed` via `ui.action`.
- Tests: synthetic OT/homebrew RAM layout, seven-flavor CSV round-trip, disarmed capture &lt;1% `runFrame` overhead (stub + optional libretro integration).

## Phase 3 status (#420 / #421)

- `VramSnapshot` — full 1024×512×16-bit VRAM buffer, view modes (RGB555, 4bpp index, 8bpp index, CLUT preview), PNG export.
- `RipperHooks::onVramWrite` mirrors GPU uploads into the worker-owned snapshot.
- `TextureDecoder` — CLUT-aware 4/8/15 bpp decode keyed by `(TPAGE, CLUT, bit depth, semiTrans, draw mode)` with UV-bounds cache merge and STP/alpha via `PsxVramColor`.
- `PS1RipMeshBuilder` — pre-decodes capture textures, uploads to `PS1Rip_Session<N>` resource group, applies PS1 semi-transparency blend modes, Sentry `ps1.rip.texture.decoded`. Ogre material resources are scoped per capture (`PS1Rip_<captureId>_tpage_…`); prior capture meshes/materials/textures are purged before each rebuild. Runtime materials use the `PS1Rip_` prefix (listed in Material Editor, read-only; GPU thumbnails are skipped to avoid UI freezes).
- `dumpVRAM()` saves `<AppData>/ps1_rip/captures/<id>_vram.png` and feeds `VramViewerWidget` in the session window.
- **Libretro:** `syncVramFromCore()` mirrors live core VRAM every frame; capture snapshots include a VRAM cell copy for textured mesh export.
- Stub core fills CLUT + 4/8/15 bpp test regions via `stubFillVramPattern` when capture is armed or on `syncCaptureMirrors` (CI only).

## Phase 4 status (#422 / #423)

- `CaptureSnapshot` copies worker `CaptureBuffer` + VRAM cells to the main thread for reconstruction.
- `GteInverse` approximates GTE screen→model un-projection; PS1 Y-down → editor Y-up.
- `MeshReconstructor` groups primitives by `matrixId` + texture key, triangulates quads, emits vertex color + UV.
- `MeshTopologyHash` + `reconstructDeduped()` collapse identical topology (loose 0.01 snap vs strict bit-exact) into unique meshes with instance centroids (#423).
- `PS1RipMeshBuilder::attachCaptureSetToScene` places one `PS1Capture_<id>_instN` SceneNode per instance at the captured world position.
- Session toolbar **Strict dedupe** toggle (persisted in QSettings); status shows captured / unique / instance counts.
- Sentry breadcrumbs `ps1.rip.mesh.built` and `ps1.rip.dedupe.summary`.

## Per-draw matrix (#658)

- **`RipperHooks::submitMatrixId`** — frozen when a GP0 drawing-environment command (`0xE1`–`0xE6`) is processed via `onDrawMode`; primitives submitted before the next draw env keep the prior matrix even if `onGteMatrix` updated `latestMatrixId`.
- **`Gp0HookDispatch`** threads `currentMatrixId` through OT/chain/linear scans and `submitGp0Words`; `matrixIdForGpuSubmit` prefers chain-local id, then submit id, then latest.
- **`0xE4` drawing offset** — parsed in `GpuCommandParser`; `onDrawingOffset` clones the active submit matrix with updated OFX/OFY (fixed-point `x<<16`, `y<<16`).
- **`MeshReconstructionStats`** — counts GTE inverse vs `psxScreenToWorld` fallback vertices, bounds extent, and a `slabLike` heuristic (min/max axis ratio &lt; 0.12). Session status bar shows **GTE inverse %** and a slab warning; Sentry `ps1.rip.matrix.stats`.

### Limitations (#658)

- **RAM ingest ordering** — OT/chain walks start with `currentMatrixId = UINT32_MAX`; matrix association depends on draw-env packets appearing *before* primitives in the scanned buffer (typical for linked lists, not guaranteed for all titles).
- **Shared matrix across OT** — one `currentMatrixId` per chain walk; dual-pass / multi-buffer games may still share a matrix across unrelated draws.
- **GTE RAM supplement** — Capture Frame still merges COP2-scanned matrices; per-draw tagging applies at GP0 dispatch time, not retroactively to RAM-only captures without draw-env context.
- **Commercial golden scenes** — manual acceptance tracked in #659; #658 reduces blob fallback but does not replace a full matrix stack or FIFO-accurate stream (#662).

## GP0 FIFO follow-up (#662)

Post-#657 / #661 work: true mednafen GP0 FIFO dispatch (packet-for-packet as the core submits), stub routing through `submitGp0Words`, session UI for capture-source breakdown, and golden-scene validation vs `QTMESH_PS1_GP0_RAM_LEGACY=1`. See [issue #662](https://github.com/fernandotonon/QtMeshEditor/issues/662).

## Troubleshooting

### Viewport shows colored rectangles (RGB gradient), not the game

The **stub** core is active (`coreId=stub`). It draws a test pattern and synthetic capture data — not your disc.

1. Confirm `mednafen_psx_libretro` is next to the app:
   ```bash
   ls -la build_local/bin/PS1Cores/mednafen_psx_libretro.so   # Linux
   ```
2. If missing, install it:
   ```bash
   ./scripts/install-ps1-libretro-core.sh build_local/bin
   ```
   Rebuild also runs this via `qtmesh_ps1core_libretro` POST_BUILD when network is available.
3. Status bar should say **Running** with gamepad hints (libretro), not **Running (stub — test pattern only)**.
4. Unset `QTMESH_PS1_FORCE_STUB` (CI sets this; it forces the stub even when libretro is installed).
5. Use a valid BIOS (`scph1001.bin` / region-matched) and prefer **`.cue`** for `.bin`/`.iso` rips.

### VRAM dump says “empty”, then “No active PS1 session”

1. **Empty while the game viewport is black** — the mirror is filled from libretro each frame. Press **Start**, wait until you see gameplay in the PS1 viewport, then dump again.
2. **Session died after the warning** — non-fatal dump/capture issues use `sessionWarning` only. Older builds used `emulationError` and stopped the session, so a second dump showed “no session”.
3. **Hardware renderer (mednafen default)** — `RETRO_MEMORY_VIDEO_RAM` is often **not** exposed (VRAM stays on the GPU). QtMeshEditor falls back to mirroring the **visible framebuffer** into the top-left of the VRAM snapshot so dump/preview still work. This is **not** the full 1024×512 texture atlas — only what is on screen. Full VRAM needs a software-renderer core build that exports memory maps (future improvement).

### Capture mesh is a triangle “blob” (normal size, wrong shape)

Capture uses **live per-frame merged RAM ingest** while armed (libretro), then a final GTE+RAM pass on **Capture Frame**. RAM strategies: ordering-table chains, standalone linked GP0 chain roots, and linear opcode scan. Expect coarse geometry on titles that stream primitives outside RAM-visible layouts. Filters drop off-screen coordinates and cap at 2048 primitives per ingest pass. Per-draw matrix tagging (#658) reduces screen-space blob fallback when draw-environment packets precede primitives in the captured buffer. True in-core mednafen GP0 FIFO dispatch (packet-for-packet as the GPU receives them) is not wired yet — see [#662](https://github.com/fernandotonon/QtMeshEditor/issues/662).

### Libretro integration tests (local only)

`EmuCoreLoaderTest` libretro disc/VRAM cases compile only when `QTMESH_PS1_LIBRETRO_INTEGRATION_TESTS` is defined (not set in CI). CI runs `StubMirrorsVramAfterSync` instead. For local runs with BIOS + `.cue`:

```bash
cmake ... -DCMAKE_CXX_FLAGS="-DQTMESH_PS1_LIBRETRO_INTEGRATION_TESTS"
export QTMESH_PS1_TEST_BIOS=/path/scph1001.bin
export QTMESH_PS1_TEST_ISO=/path/game.cue
./build/bin/UnitTests --gtest_filter='EmuCoreLoaderTest.Libretro*'
./build/bin/UnitTests --gtest_filter='PS1RipManagerLibretroArmTest.*'
```

`PS1RipManagerLibretroArmTest` (arm/capture while a real libretro session runs) uses the same guard and env vars.

**GTE / matrix tests (CI):** `PsxGteCop2Test`, `PsxGteIsoDedupeTest.StaticSceneCop2ProgramDedupesThreeDrawables`, `MeshReconstructorTest.GtePipelineCubeRoundTripsToUnitCubeMesh`, `PsxPerDrawMatrixTest` (draw-env matrix freeze, E4 offset, reconstruction stats). Real-ISO matrix dedupe: set `QTMESH_PS1_TEST_BIOS` + `QTMESH_PS1_TEST_ISO` (or `QTMESH_PS1_TEST_HOMEBREW_ISO`) and run `PsxGteIsoDedupeTest.RealIsoCaptureHasBoundedMatrixCount`.

## Open questions

- libretro core packaging for Windows/macOS CI (Linux: apt `libretro-beetle-psx` + install script).

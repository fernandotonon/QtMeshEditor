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
- `PS1RipSessionWindow` + `EmuViewport` + legality dialog (#416–#417) — keyboard/gamepad input still TODO.
- Install helper: `scripts/install-ps1-libretro-core.sh` copies a distro libretro core into `build/bin/PS1Cores/`.

## Phase 2 status

- `CaptureTypes`, `GpuCommandParser`, `CaptureBuffer`, `RipperHooks` (#418).
- GTE matrix hash dedupe + `cameraMatrixId` heuristic (#419).
- **Libretro capture path:** `RipperHooks::ingestSystemRamForGpuCapture` runs `PsxGteRamScanner` (heuristic matrix find) + `PsxGpuRamScanner` (linear GP0 decode with correct low-byte opcodes) each frame while armed.
- **Stub core** still emits synthetic primitives/VRAM for CI when `QTMESH_PS1_FORCE_STUB=1` or no libretro core is present.
- `armCapture` / `captureFrame` wire capture to the worker thread; CSV dump to temp for verification.

## Phase 3 status (#420 / #421)

- `VramSnapshot` — full 1024×512×16-bit VRAM buffer, view modes (RGB555, 4bpp index, 8bpp index, CLUT preview), PNG export.
- `RipperHooks::onVramWrite` mirrors GPU uploads into the worker-owned snapshot.
- `TextureDecoder` — CLUT-aware 4/8/15 bpp tile decode with `TileKey` cache and STP/alpha via `PsxVramColor`.
- `dumpVRAM()` saves `<AppData>/ps1_rip/captures/<id>_vram.png` and feeds `VramViewerWidget` in the session window.
- **Libretro:** `syncVramFromCore()` mirrors live core VRAM every frame; capture snapshots include a VRAM cell copy for textured mesh export.
- Stub core fills CLUT + 4/8/15 bpp test regions each frame via `stubFillVramPattern` (CI only).

## Phase 4 status (#422)

- `CaptureSnapshot` copies worker `CaptureBuffer` + VRAM cells to the main thread for reconstruction.
- `GteInverse` approximates GTE screen→model un-projection; PS1 Y-down → editor Y-up.
- `MeshReconstructor` groups primitives by `matrixId` + texture key, triangulates quads, emits vertex color + UV.
- `PS1RipMeshBuilder` creates Ogre mesh/submeshes, binds decoded TPAGE/CLUT textures when VRAM is present, and attaches `PS1Capture_<id>` to the live scene via `Manager`.
- `captureFrame` builds mesh automatically; session toolbar adds **Arm Capture** / **Capture Frame**.
- Sentry breadcrumb `ps1.rip.mesh.built` with vertex/triangle counts.

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

Capture is **not** a true GPU hook — it heuristically scans main RAM for GP0 command packets. Expect coarse triangle soup, not level geometry. Filters drop off-screen coordinates and cap at 2048 primitives per ingest pass to reduce noise. Quality improvements need ordering-table / DMA hooks (future work).

## Open questions

- libretro core packaging for Windows/macOS CI (Linux: apt `libretro-beetle-psx` + install script).
- BIOS / ISO first-run legality dialog copy (#417) — requires legal review before release.

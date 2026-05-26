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
- **GP0 capture (#657 / #662):** Four paths feed `RipperHooks::onGpuPrim`, sorted by attribution priority in `Gp0CaptureStats::primarySource`:
  - **Direct hook (`gp0_hook`):** `EmuHooks::submitGp0Words` — the canonical FIFO code path. Used by the **stub** core (synthetic seven-flavor capture, post-#662) and by the **libretro live FIFO bridge** (`EmuHooks::submitFifoChainsFromRam`, `Gp0HookDispatch::submitChainsFromRam`) which walks RAM-resident DMA chains per frame and dispatches each chain's raw word range via `submitGp0Words`. `RipperHooks` saves/restores `m_ramCaptureActive` around the bridge call so prims it dispatches count toward `directHookPrims` even when nested inside a wrapping RAM-capture pass. Disable with `QTMESH_PS1_GP0_FIFO_BRIDGE=0` to A/B vs the legacy RAM-only attribution.
  - **OT chain (`ram_ot`):** `PsxOrderingTableScanner` walks ordering-table entries (libgpu `getaddr`) and follows linked DR-tag chains.
  - **Chain root (`ram_chain_root`):** `PsxGp0ChainRootScanner` finds standalone linked roots that no OT pointer touched.
  - **Linear (`ram_linear`):** opcode-by-opcode fallback for buffers without OT/chain headers.
  All four share one dedupe set (no early return after a weak OT). Linked DR tags carry the opcode in bits 24–31 and the next packet address in bits 2–23 (`PsxGp0Opcode.h`). Sentry breadcrumb `ps1.rip.capture.gp0_hook` records per-path counts; session status bar surfaces `GP0 <source> (hook X / ot Y / chain Z / linear W)` after each capture.
- **Per-core capability (#662, #674):**

  | Core | VRAM | GP0 FIFO source | TMD/HMD scan | Notes |
  |------|------|-----------------|--------------|-------|
  | `mednafen_psx_libretro` (software) | full 1024×512 (#660) | live FIFO bridge (`gp0_hook`) + merged RAM | TMD active, HMD opt-in | recommended |
  | `beetle_psx_libretro` (software) | full 1024×512 (#660) | live FIFO bridge (`gp0_hook`) + merged RAM | TMD active, HMD opt-in | recommended |
  | `beetle_psx_hw_*` | none | excluded — `gp0_hook` path unavailable | excluded (no RAM access) | rejected by `LibretroCoreOptions` |
  | `stub` (`qtmesh_ps1core_stub`) | synthetic test pattern | direct stub via `submitGp0Words` (`gp0_hook`) | not scanned (synthetic RAM is GP0-only) | CI / no-disc smoke |

- **Libretro live frame:** While capture is armed, each `retro_run()` end calls `captureGpuFromRam(false, true)` → `Gp0HookDispatch::captureFrameFromSystemRam`. Inside that pass, the FIFO bridge fires first (DirectHook attribution) and the merged RAM scan runs second (Ram* attribution). Primitives accumulate with cross-frame dedupe (`m_liveDedupe`) until **Capture Frame** runs a final GTE+RAM merge. Disable live ingest with `QTMESH_PS1_GP0_LIVE_CAPTURE=0`. Baseline RAM-only behavior (OT then linear, no chain-root pass, no FIFO bridge) via `QTMESH_PS1_GP0_RAM_LEGACY=1` + `QTMESH_PS1_GP0_FIFO_BRIDGE=0`.
- **Stub core** (`coreId=stub`): seven-flavor capture is now emitted as a contiguous GP0 word buffer routed through `submitGp0Words` (#662) — the production stub matches the same code path retail captures use.
- **Libretro core** (`coreId=libretro`): live FIFO bridge ships in this slice (#662). True packet-for-packet in-core mednafen GP0 hooks (i.e. patched `mednafen_psx_libretro` with a `RipperHooks`-aware GPU_Write callback) remain out of scope until a forked core ships — the bridge approximates FIFO ordering by walking DMA chains per frame, which is sufficient to surface `gp0_hook` attribution and to feed the canonical hook code path on retail captures.
- `armCapture` / `captureFrame` wire capture to the worker thread; CSV dump to temp for verification.
- Sentry breadcrumb `ps1.rip.capture.frame_armed` via `ui.action`.
- Tests: synthetic OT/homebrew RAM layout, seven-flavor CSV round-trip, disarmed capture &lt;1% `runFrame` overhead (stub + optional libretro integration).

## Phase 3 status (#420 / #421)

- `VramSnapshot` — full 1024×512×16-bit VRAM buffer, view modes (RGB555, 4bpp index, 8bpp index, CLUT preview), PNG export.
- `RipperHooks::onVramWrite` mirrors GPU uploads into the worker-owned snapshot.
- `TextureDecoder` — CLUT-aware 4/8/15 bpp decode keyed by `(TPAGE, CLUT, bit depth, semiTrans, draw mode)` with UV-bounds cache merge and STP/alpha via `PsxVramColor`.
- `PS1RipMeshBuilder` — pre-decodes capture textures, uploads to `PS1Rip_Session<N>` resource group, applies PS1 semi-transparency blend modes, Sentry `ps1.rip.texture.decoded`. Ogre material resources are scoped per capture (`PS1Rip_<captureId>_tpage_…`); prior capture meshes/materials/textures are purged before each rebuild. Runtime materials use the `PS1Rip_` prefix (listed in Material Editor, read-only; GPU thumbnails are skipped to avoid UI freezes).
- `dumpVRAM()` saves `<AppData>/ps1_rip/captures/<id>_vram.png` and feeds `VramViewerWidget` in the session window.
- **Libretro:** `syncVramFromCore()` mirrors live core VRAM every frame; capture snapshots include a VRAM cell copy for textured mesh export. Software renderer (`beetle_psx_renderer=software`, #660) is required for full TPAGE/CLUT decode on retail captures; framebuffer-only fallback remains for unsupported cores.
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
- **Commercial golden scenes** — manual acceptance tracked in [#659](https://github.com/fernandotonon/QtMeshEditor/issues/659) (`src/PS1/golden_captures.md`); #658 reduces blob fallback but does not replace a full matrix stack or FIFO-accurate stream (#662).

## Screen-space inverse projection (#675)

The screen-space capture paths (live FIFO bridge, OT chains, chain roots, linear scan)
all deliver post-projection GP0 vertex coordinates — to land them in editor world units
the reconstructor inverts the PSX GTE projection in `GteInverse::screenToModel`. The
forward / inverse math matters: when it's wrong, every screen-space vertex falls through
to `psxScreenToWorld` (a pure pixel-to-flat-XY mapping) and the reconstructed mesh is the
classic flat-plate "blob".

- **The math:** `GteInverse::modelToScreen` implements the psx-spx RTPS formula
  `IR[r] = (RT[r][:] · V) / 4096 + TR[r]`, `SX_pixel = H * IR[0] / IR[2] + OFX/65536`
  (similarly for SY), `SZ = IR[2]`. `GteInverse::screenToModel` inverts it via
  `IR[2] = sz`, `IR[i<2] = (screen_i - OF_i) * IR[2] / H`, then
  `V = RT^T * (IR - TR)` (the `RT^T` fast path exploits the fact that PS1 RT matrices
  are orthonormal — `RT^-1 == RT^T` for unit rotations). Pre-#675 both `modelToScreen`
  and `screenToModel` were diagonal-only (`vx / RT[0][0]`) which silently passed the
  identity-matrix roundtrip test in CI but rejected every real rotation matrix at the
  `kMaxVertexRadius` filter in `MeshReconstructor::vertexFromPsx`.
- **The orthonormal gate (`GteCapture::looksOrthonormalRotation`):** validates that an
  `MatrixRecord`'s 3×3 RT block satisfies `|row|^2 ≈ 4096^2`, `dot(row_i, row_j) ≈ 0` and
  `det ≈ +4096^3` (each within 5–10% slack so 12.4 quantisation of `cos`/`sin` tables
  doesn't false-reject). Used in two places:
  - `PsxGteRamScanner::looksLikeMatrixRecord` — drops false-positive matrix candidates.
    Combined with the narrowed `H ∈ [64, 2048]` and tightened RT entry magnitude
    (`|RT[r][c]| ≤ 8192`), pseudo-random 16 KB RAM blocks produce **zero** accepted
    matrices in the regression suite (`PsxGteRamScannerTest.RejectsPseudoRandomGarbage`).
  - Future: `GteInverse::screenToModel` callers can pre-check this if they want to skip
    the inverse for non-orthonormal matrices and force `psxScreenToWorld` instead — for
    now the `RT^T` solve is harmless on any input that survives the scanner gate.
- **Diagnostics (`MeshReconstructionStats`):** `primsTotal`, `primsWithMatrixId`,
  `gteInverseVertices`, `screenFallbackVertices`. Plumbed through
  `PS1RipManager::meshBuilt(... primsWithMatrixId, primsTotal ...)` into
  `PS1RipSessionWindow`'s status bar as
  `GTE inverse N% (matrix tag X/Y)`. Sentry breadcrumb `ps1.rip.matrix.stats` carries
  the same `gte_inverse=N%% prims_with_matrix=X/Y` for off-line analysis.
- **Acceptance tests** (`GteInverseTest`, `GteCaptureTest`, `PsxGteRamScannerTest`):
  90° Y rotation and arbitrary 3D Euler (`30° + 45° + 15°`) round-trip within 2–4
  fixed-point units; identity, real rotations accepted by the validator; scaled
  rotation, reflection (det = `-4096^3`) and pseudo-random garbage rejected.
- **Out of scope:** matrix→primitive *association* (which RT was active when this prim
  was drawn) remains heuristic via per-draw matrix tagging (#658). The math fix in #675
  makes the inverse correct when association is correct; ground-truth association on
  retail games still needs the forked-mednafen in-core GTE hook tracked in
  [#676](https://github.com/fernandotonon/QtMeshEditor/issues/676).

## Model-space RAM scanners (#674)

Screen-space GP0 prims always carry information loss because they're post-projection — the
GTE has applied rotation, translation, perspective divide, and 16-bit truncation by the
time they hit RAM. Inverse-projecting them requires recovering the exact GTE matrix used
for each draw, which on retail games is heuristic-grade and produces flat-XY "blob"
meshes when the math drifts. Sony's TMD / HMD / PMD asset formats sidestep this entirely:
the assets sit in RAM as **model-space** structures with 16-bit fixed-point vertex pools
and packet-encoded primitives. Scanning RAM for them and emitting the vertices directly
yields clean meshes with no inverse step.

- **`PsxTmdRamScanner`** — sweeps RAM (4-byte stride) for `0x00000041`-tagged TMD blobs.
  For each candidate validates `flags ∈ {0, 1}`, `numObj ∈ [1, 256]`, every object header's
  vertex/normal/primitive offsets resolve in-bounds, counts ≤ 8192, and the primitive walk
  produces ≥ 1 emitted triangle (the false-positive filter). Both `flag=0` (offsets
  relative to file-byte 12, on-disk form) and `flag=1` (offsets are KSEG0 RAM pointers,
  masked with `0x001FFFFF`) are supported. Coordinate transform mirrors the on-disk
  `PS1TMD::importTmd`: PSX 12.4 fixed × editor uniform scale × 180° Z rotation. Primitive
  set covers `0x20`, `0x30` (ilen 4 & 6), `0x24`, `0x34`, `0x28` (flag 0 & 4), `0x25`,
  `0x35` — the bread-and-butter Sony SDK packets. Each unique TMD found emits via
  `EmuHooks::onModelMesh(CapturedModelMesh)`; the `CaptureBuffer` dedupes by FNV-1a 64-bit
  hash of the TMD byte span so the same asset encountered every frame only stores once.
  Material names for textured packets use the standard `MeshReconstructor::textureMaterialName`
  format (`PS1Rip_tpage_XXXX_clut_YYYY_stN_dmN`), so the existing capture texture-decode
  path (#421) binds them with zero extra wiring.

- **`PsxHmdRamScanner`** — v1 stub for `0x00000050`-tagged HMD blobs. The HMD layout
  embeds a hierarchical primitive-node tree whose tag types are significantly more
  involved than TMD's flat object table, and parsing it speculatively from RAM is a
  documented source of false positives. v1 ships the scaffold + a magic-bytes candidate
  count so per-title testing can be enabled via `QTMESH_PS1_HMD_SCANNER=1` without code
  changes; the actual primitive walk lands once a known-good HMD asset is available.

- **`MeshReconstructor::buildParts`** — after the screen-space `buildMatrixGroups` pass,
  every `snapshot.modelMeshes[i].mesh` is appended as an additional part. They flow
  through the same `MeshTopologyHash`-based dedupe in `reconstructDeduped`, so byte-identical
  TMDs collapse to one unique mesh + N instance transforms (matching the screen-space dedupe
  semantics). Stats: `MeshReconstructionStats::modelMeshVertices` plus `gteInversePercent`
  now treats model-mesh verts as trusted, so the quality indicator flips from 0% to ~100%
  on TMD-using games.

- **Stats / UI / Sentry**:
  - `Gp0CaptureStats::ramTmdMeshes` / `ramHmdMeshes` track per-frame **emitted** mesh
    counts (i.e. successful `EmuHooks::onModelMesh` calls). HMD v1 always reports 0
    here — it doesn't emit yet.
  - `Gp0CaptureStats::ramHmdCandidates` is the v1 diagnostics count of plausible HMD
    magic-bytes hits and **does not** flip the primary source. It exists so testers can
    confirm the magic-bytes scan works on HMD-using titles before the walker lands.
  - `Gp0CaptureSource::RamModelMesh` (label `ram_model_mesh`) becomes the primary
    source only when actual model-mesh emissions happened (`ramTmdMeshes > 0`
    or `ramHmdMeshes > 0`) — bare candidate counts never promote the label.
  - Status bar: `GP0 <source> (hook X / ot Y / chain Z / linear W / tmd T / hmd H / hmd_cand C)`.
  - Sentry breadcrumbs: `ps1.rip.capture.gp0_hook` and `ps1.rip.capture.summary` carry
    `tmd=…  hmd=…  hmd_cand=…`; new `ps1.rip.capture.modelmesh` fires only when meshes
    were actually emitted (not candidate-only); `ps1.rip.matrix.stats` adds
    `model_meshes=…`.

- **Per-format coverage:**

  | Format | Magic | Where used | v1 scanner |
  |--------|-------|-----------|------------|
  | TMD | `0x00000041` | static models — most Sony SDK titles (Tekken 1/2/3, Ridge Racer 1/RR, Net Yaroze homebrew, Wipeout 1/2097, R-Type Delta, Klonoa, many Square pre-FF7) | **emits meshes** |
  | HMD | `0x00000050` | hierarchical/skinned models — Sony SDK animations | stub (counts candidates only) |
  | PMD | n/a (engine) | Net Yaroze homebrew TMD subset — handled by TMD scanner | — (TMD path) |
  | TIM | `0x00000010` | textures — bound via the VRAM mirror, not model-space | — (texture path) |
  | RSD / PLY / MAT / GRP | text | disc/artist formats — compiled to TMD before being loaded; not present in RAM during gameplay | — (offline) |

- **Out of scope (#675, #676, #677):**
  - `#675` — heuristic + math fix for the screen-space inverse path (tighten matrix scanner,
    real-rotation inverse roundtrip tests, surface `prims_with_matrix=X/N`).
  - `#676` — forked `mednafen_psx_libretro` with an in-core RTPS/RTPT callback for
    ground-truth model-space recovery on any game including custom engines (Crash, Spyro,
    FFVII field models, MGS).
  - `#677` — disc/ISO scanner for RSD/PLY/MAT/GRP/TIX off-line ripping (separate CLI/MCP).

- **Disable:** `QTMESH_PS1_TMD_SCANNER=0` skips the TMD pass entirely (debug / golden
  baselines). HMD scanner is opt-in (`QTMESH_PS1_HMD_SCANNER=1`).

## GP0 FIFO follow-up (#662)

Shipped in this slice:
- **Stub via `submitGp0Words`** — seven-flavor synthetic capture builds a contiguous GP0 word buffer (`StubCaptureSynth.cpp`) and submits via `hooks->submitGp0Words`, matching the canonical hook code path (`Gp0CapturePathsTest.StubSeven*` regressions).
- **Live FIFO bridge for libretro** — `EmuHooks::submitFifoChainsFromRam` (override on `RipperHooks`) calls `Gp0HookDispatch::submitChainsFromRam`, which sweeps the first 512 KiB of mednafen system RAM for contiguous DMA chain roots and dispatches each chain's word range through `submitGp0Words`. Bridge runs inside `captureFrameFromSystemRam` *before* the merged RAM scan; `m_ramCaptureActive` is cleared so prims it produces are attributed as `Gp0CaptureSource::DirectHook`. Disabled with `QTMESH_PS1_GP0_FIFO_BRIDGE=0`.
- **Session UI / Sentry** — `PS1RipSessionWindow` status bar now appends `GP0 <primary> (hook X / ot Y / chain Z / linear W)`; `PS1RipWorker` emits a `ps1.rip.capture.summary` Sentry breadcrumb with the same breakdown.

Still open work:
- **In-core mednafen GP0 hook** — patching a fork of `mednafen_psx_libretro` with a callback that pushes each `GPU_Write32` to `submitGp0Words` would give true packet-for-packet ordering (vs the per-frame RAM walk the bridge approximates today). Tracked in [#662](https://github.com/fernandotonon/QtMeshEditor/issues/662) for a follow-up.
- **Golden-scene regression vs the legacy RAM-only path** — see `src/PS1/golden_captures.md`. A/B with `QTMESH_PS1_GP0_FIFO_BRIDGE=0` to confirm the bridge's `gp0_hook` attribution doesn't regress prim counts on known scenes.

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
3. **Hardware renderer (mednafen default)** — `beetle_psx_hw` and hardware GL/Vulkan renderers keep VRAM on the GPU and do **not** expose `RETRO_MEMORY_VIDEO_RAM`. QtMeshEditor forces **`beetle_psx_renderer = software`** via libretro core options (override + `Beetle PSX.cfg` in the BIOS folder) and **excludes** `beetle_psx_hw_*` from auto-discovery. With software mednafen/beetle, texture decode (#421) reads the full 1024×512 VRAM snapshot at capture time.

   If the status bar still shows **VRAM: framebuffer mirror only**, the loaded core is not exposing VRAM — confirm `PS1Cores/mednafen_psx_libretro.*` (not `_hw_`), unset `QTMESH_PS1_LIBRETRO_RENDERER=auto` unless debugging, and re-run `scripts/install-ps1-libretro-core.sh`. Optional env: `QTMESH_PS1_LIBRETRO_RENDERER=software|hardware|auto` (default **software**).

   **Partial tier:** when only the visible framebuffer is mirrored, GP0 VRAM-upload packets during armed capture may still patch texture pages outside the FB rect (**framebuffer + GP0 texture patches**). Golden-scene geometry-only passes are documented in `src/PS1/golden_captures.md`.

### Capture mesh is a triangle “blob” (normal size, wrong shape)

The screen-space GP0 path (live FIFO bridge + ordering-table chains + standalone chain
roots + linear scan) used to **always** produce a flat-XY blob because both the GTE
forward and inverse transforms in `GteInverse` were diagonal-only and silently rejected
every non-identity rotation matrix (the existing roundtrip test only exercised identity).
**#675** replaces them with the real psx-spx math (`IR = (RT * V + TR)`, with `RT^T`
inverse for orthonormal rotations) and adds an orthonormal validator
(`GteCapture::looksOrthonormalRotation`) that gates both the `screenToModel` fast path
and the `PsxGteRamScanner` candidate filter. Combined effect:

- Real rotations (90° Y, mixed XYZ Euler, etc.) now round-trip within ~2 fixed-point
  units instead of producing radius-million garbage rejected by `kMaxVertexRadius`.
- The matrix scanner drops false positives — single-box retail captures that previously
  reported ~192 "matrices" should land in the single-digit-to-low-double-digit range
  because pseudo-random RAM bytes cannot satisfy `|row|^2 ≈ 4096^2 ∧ row_i · row_j ≈ 0
  ∧ det ≈ +4096^3` all at once.

**Diagnostic reading order** (post-#675 status bar):
1. `GTE inverse N%` — fraction of vertices that successfully ran through the inverse.
   Healthy retail capture ≥ ~50%. If 0%, see step 2.
2. `matrix tag X/Y` — how many primitives were associated with a captured matrix.
   `0/Y` means matrix association is the bottleneck (no `0xE4` draw-environment packets
   captured, or per-draw matrix tagging from #658 didn't run). `Y/Y` with low inverse %
   means the math is rejecting the matrices — usually because they aren't orthonormal,
   often because the title uses a custom transform stack.
3. `tmd N / hmd N` — model-space scanner hits (#674). If ≥ 1, the primary source flips
   to `ram_model_mesh` and you're on the clean-mesh path regardless of the screen-space
   stats.

The recommended path for "real meshes from real games" remains the **model-space
TMD/HMD RAM scanner** (#674) for Sony SDK titles. #675's screen-space math fix
**unblocks** the screen-space path when matrix association is correct, but the
matrix-to-draw linkage on retail games is still heuristic — true ground-truth recovery
requires the forked-mednafen in-core GTE hook tracked in
[#676](https://github.com/fernandotonon/QtMeshEditor/issues/676).

- **TMD-using games (clean meshes today via #674):** Tekken 1/2/3, Ridge Racer 1/RR,
  Net Yaroze SDK demos, Wipeout 1/2097, R-Type Delta, Klonoa, many pre-FF7 Square titles.
- **Custom-engine games (still partial recovery):** Crash, Spyro, FFVII field models,
  MGS post-Yaroze. These games author their own packed mesh layouts and bespoke transform
  stacks; only #676 covers them.
- Tune via `QTMESH_PS1_TMD_SCANNER=0` to disable TMD scanning for a baseline, and
  `QTMESH_PS1_HMD_SCANNER=1` to opt into the v1 HMD candidate counter.

If the scanner produces meshes but they look wrong (wrong axis flip, wrong scale), the
coordinate transform in `PsxTmdRamScanner` mirrors `PS1TMD::importTmd` — if the on-disk
TMD importer is also wrong, fix it in `PS1TMD.cpp` and the RAM scanner picks it up via
the matching constants. Per-draw matrix tagging (#658) and the live FIFO bridge (#662)
still help on the screen-space side but cannot reconstruct topology from screen-space
prims alone.

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

**GTE / matrix tests (CI):** `PsxGteCop2Test`, `PsxGteIsoDedupeTest.StaticSceneCop2ProgramDedupesThreeDrawables`, `MeshReconstructorTest.GtePipelineCubeRoundTripsToUnitCubeMesh`, `PsxPerDrawMatrixTest` (draw-env matrix freeze, E4 offset, reconstruction stats), `MeshReconstructorGoldenTest` (slab vs volume heuristics; always on). Real-ISO matrix dedupe: set `QTMESH_PS1_TEST_BIOS` + `QTMESH_PS1_TEST_ISO` (or `QTMESH_PS1_TEST_HOMEBREW_ISO`) and run `PsxGteIsoDedupeTest.RealIsoCaptureHasBoundedMatrixCount`.

### Golden capture suite (#659)

- **Doc:** `src/PS1/golden_captures.md` — homebrew + two retail acceptance scenes, manual checklist, env var names.
- **Env:** `QTMESH_PS1_GOLDEN_SCENE_ID`, `QTMESH_PS1_GOLDEN_HOMEBREW_ISO`, `QTMESH_PS1_GOLDEN_RETAIL_A_ISO`, `QTMESH_PS1_GOLDEN_RETAIL_B_ISO` (plus legacy `QTMESH_PS1_TEST_*` aliases).
- **CI:** `MeshReconstructorGoldenTest.SlabMetricHeuristic*` / `ScreenCubeReconstructionHasVolume` run without ROMs. `ConfiguredGoldenIsoReconstructsWithVolume` is a no-op when BIOS/ISO paths are unset.
- **Telemetry:** `ps1.rip.capture.golden` and `ps1.rip.matrix.stats` include `golden_id=` when `QTMESH_PS1_GOLDEN_SCENE_ID` is set or `PS1RipManager::setGoldenSceneId()` is used (session UI picker: #425).

### Full VRAM / textures (#660)

- **Software renderer:** libretro host forces `beetle_psx_renderer=software` (env override `QTMESH_PS1_LIBRETRO_RENDERER`, default software) and rejects `beetle_psx_hw_*` cores.
- **Status bar:** after capture, `VRAM: full VRAM` vs `framebuffer mirror only` vs `framebuffer + GP0 texture patches`.
- **Sentry:** `ps1.rip.vram.sync` breadcrumb on capture with mode label.
- **Tests:** `LibretroCoreOptionsTest`, `VramSnapshotTest.HasNonZeroOutsideRectDetectsTpageRegion`, stub VRAM tests unchanged.

## Open questions

- libretro core packaging for Windows/macOS CI (Linux: apt `libretro-beetle-psx` + install script).

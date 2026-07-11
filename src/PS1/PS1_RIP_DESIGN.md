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
- `PS1RipSessionWindow` + `EmuViewport` (#416 / #425): software framebuffer via `QPainter`, integer-scale and bilinear toggles (View menu), 4:3 NTSC/PAL letterbox mode, FPS overlay, frame pacing tied to `runFrame()` completion (~16 ms target). Hosted in a session window from *Tools → Experimental → PS1 Runtime Ripper…* — see [Capture controls + scene capture (#425)](#capture-controls--scene-capture-425) for the full capture surface (transport toolbar, Capture Frame / Scene / Stop / VRAM, Normalize dock, live status footer, `C`/`Shift+C`/`V` hotkeys).
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
  | `beetle_psx_qtmesh_libretro` (rip fork, software) | full 1024×512 (#660) | **in-core hooks (`gp0_incore`)** — true packet stream + GTE records (#813–#815); RAM passes suppressed | TMD active, HMD opt-in | **recommended for model extraction**; in-core hooks: active |
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
- **GP0 carries no depth (`sz == 0` guard):** PS1 GP0 polygon packets store *only* 2D
  screen-space XY for each vertex. The GTE writes Z into a separate `SZ` FIFO that the
  CPU drains *before* assembling the GP0 word, so `PsxVertex::z` is identically 0 for
  every prim ingested from the GP0 hook or the live FIFO bridge. With `sz == 0` the
  inverse degenerates — `IR[0] = IR[1] = 0`, every vertex of a given matrix tag maps to
  the same point `RT^T · (-TR) / 4096`, and the resulting per-matrix sub-mesh has zero
  extent (invisible in the viewport but still reported as `GTE inverse 100%`).
  `GteInverse::screenToModel` therefore early-returns `false` when `sz == 0`, forcing
  `MeshReconstructor::vertexFromPsx` to fall back to `psxScreenToWorld` (the flat-XY
  blob). The fallback is intentional: an ugly-but-visible mesh beats a degenerate
  zero-extent one. Real per-vertex depth recovery for GP0-only captures requires the
  in-core GTE hook (#813–#815 in-core capture issue set — the design doc's original
  `#676` reference is stale; that number was taken by a merged PR) or future RAM scanner
  work that recovers the SZ FIFO contents alongside the matrix snapshot. With the
  qtmesh beetle fork loaded, tracked vertices carry `viewW` and the inverse runs
  well-posed (Tier 1, #816).
- **When the inverse *does* run:** model-space test fixtures (`PsxPerDrawMatrixTest`,
  `MeshReconstructorCubePipelineTest`) inject vertices via `modelToScreen` so they carry
  the forward-projected `sz` — the math fix is verified on those paths. On retail GP0
  captures the inverse stays gated by `sz == 0`, so the status bar reports
  `GTE inverse 0% (matrix tag X/Y)`. That is the *correct* signal: matrix association is
  fine (`X/Y` nonzero), but depth was lost at the GP0 boundary.
- **Out of scope:** matrix→primitive *association* (which RT was active when this prim
  was drawn) remains heuristic via per-draw matrix tagging (#658). The math fix in #675
  makes the inverse correct when association is correct *and* depth is available;
  ground-truth depth + association on retail games is delivered by the in-core capture
  chain ([#813](https://github.com/fernandotonon/QtMeshEditor/issues/813)–[#817](https://github.com/fernandotonon/QtMeshEditor/issues/817)
  — the old `#676` link is stale, that number belongs to a merged PR).

## In-core rip capture (#813–#817) — the primary path

Every capture path above is host-side RAM scanning after `retro_run()` and has a hard
information ceiling: GP0 packets carry no depth (the SZ FIFO is drained before the packet
is assembled), and matrix→draw association from RAM scans is heuristic-grade. The fix —
proven at scale by PGXP (iCatButler 2016) — is to capture **inside the emulator core**, at
the GTE, where the game hands over its original object-space vertices and per-draw matrix,
and to value-track those through CPU/memory to the GP0 packet.

### The fork

`fernandotonon/beetle-psx-libretro`, branch `qtmesh-rip`. All rip changes live behind
`HAVE_QTMESH_RIP` (new files under `rip/`); the artifact is renamed
`beetle_psx_qtmesh_libretro.{so,dylib,dll}` and the core self-identifies as
`Beetle PSX (QtMesh rip)`. Build it into `PS1Cores/` with
`scripts/build-ps1-rip-core.sh` (pinned to a recorded fork commit) or configure with
`-DENABLE_PS1_RIP_CORE_BUILD=ON`. With a stock core everything below is inert and the
RAM-scan paths run unchanged.

### The rip ABI (v1)

`rip/qtmesh_rip_abi.h` in the fork; byte-identical vendored copy at
`src/PS1/runtime/libretro/qtmesh_rip_abi.h`. Pure C, version-checked at registration:

| Export | Purpose |
|--------|---------|
| `qtmesh_rip_abi_version()` | compile-time ABI version; host refuses to register on mismatch |
| `qtmesh_rip_set_interface(iface)` | registers host callbacks (`NULL` unregisters — must happen before core unload) |
| `qtmesh_rip_set_armed(armed)` | arms/disarms capture; disarmed hot paths are one predictable branch |

| Callback | Fires | Payload |
|----------|-------|---------|
| `on_gp0_draw` | per executed GP0 command, in submission order | complete packet words (quads re-assembled from the two-fragment command-buffer flow) + one `qtmesh_rip_vertex_shadow` per vertex word: PGXP precise `sx,sy`, view depth `w`, validity flags, GTE record ring index |
| `on_gte_records` | once per frame, before `on_frame_end` | every `qtmesh_rip_gte_record` captured this frame: raw object-space V register (`vx,vy,vz`, s16), rotation matrix (`rt[9]`, 4.12), `tr[3]`, `ofx/ofy/h`, precise outputs, `frame`, `seq` |
| `on_frame_end` | once per armed `retro_run` | frame counter |

The GTE record ring holds `QTMESH_RIP_GTE_RING_ENTRIES` (65536) records — vertex shadows
reference ring slots, so a draw's tags always resolve against the frame's flush (the host
buffers draws until the flush arrives). All callbacks fire synchronously inside
`retro_run()` on the worker thread — the same thread that already drives `RipperHooks`.

### Fork-side mechanics (#814/#815)

- **GTE hook:** the RTPS/RTPT perspective transform (`mednafen/psx/gte.c`, the PGXP
  precise-push site) records the full transform context into the ring and tags the SXY2
  PGXP shadow with `ring index + 1` (`rip_tag`, a new `PGXP_value` field; 0 = untagged).
- **Tag propagation:** tags ride PGXP's own value tracking (MFC2/SWC2/CPU moves/memory
  shadow). Any operation that *recomputes or splices* shadow components drops the tag
  (arithmetic and shifts in `pgxp_cpu.c`, halfword paths in `pgxp_mem.c`,
  `SetValue`/`MakeValid`); pure moves (`addiu rt,rs,0`, `or rd,rs,zero`, loads/stores of
  whole words) keep it. A dropped tag is not fatal — the precise x/y/w still reach the
  GPU and the vertex degrades to depth-only reconstruction.
- **GP0 hook:** one site in `ProcessFIFO` (`mednafen/psx/gpu.c`) covers CPU-direct writes
  AND DMA (both drain through `GPU_WriteCB`). Quad second-fragments are re-assembled in
  the rip layer so the host receives complete wire packets. Covered: polygons 0x20–0x3F,
  sprites 0x60–0x7F, draw-env 0xE1–0xE6 (in-stream, so per-draw TPAGE association is
  exact), lines 0x40–0x5F (delivered; host ignores).
- **PGXP mode forcing:** while armed, PGXP runs with
  `MEMORY|CPU|GTE|TEXTURE_CORRECTION` regardless of the user-facing core option (CPU
  mode costs perf — armed only); the user's configured modes are restored on disarm.
  Disarmed sessions run at stock speed.

### Host-side ingest

- `LibretroHost` resolves the three `qtmesh_rip_*` symbols best-effort;
  `LibretroEmuCore` registers trampolines after `retro_init`, mirrors the worker's armed
  flag into the core each frame, and unregisters before unload. `EmuCore::inCoreHooksActive()`
  surfaces the state (session status bar: `in-core hooks: active / unavailable (stock core)`;
  Sentry `ps1.rip.core.incore_hooks`).
- `RipperHooks::onGpuDrawTracked` buffers draws; `onGteRecords` appends to
  `CaptureBuffer::gteRecords` (session cap 256k with drop counter) and feeds each unique
  `(rt, tr, ofx/ofy/h)` through the existing matrix dedupe; `onCoreFrameEnd` parses each
  buffered packet with the **existing** `GpuCommandParser::stepGp0`, overlays the vertex
  shadows, resolves ring indices against the delivered records (with an sx/sy backstop —
  a tag whose record no longer matches the shadow's precise coords degrades to
  DepthOnly), and ingests via the normal `addPrim` path with per-vertex
  `PsxVertexProvenance` (`GteTracked` / `DepthOnly` / `None`) and `PrimRecord::frame`.
- **Suppression:** while the in-core stream is active, every heuristic screen-space pass
  is skipped for the session — `PsxGteInstructionCapture`, `PsxGteRamScanner`, the FIFO
  bridge, and the OT/chain/linear RAM scans. Model-space TMD/HMD scanners stay on
  (complementary exact source). Attribution: `Gp0CaptureSource::InCoreHook`
  (`gp0_incore`) outranks `DirectHook`.
- **Caps:** the in-core path accepts 16384 draws/frame (RAM paths keep 2048); overflow
  warns via `ps1.rip.capture.overflow` instead of silently truncating.

### Env vars

| Var | Effect |
|-----|--------|
| `QTMESH_PS1_RIP_INCORE=0` | skip rip-interface registration even when the fork is loaded — A/B against the RAM-scan heuristics |
| `QTMESH_RIP_CORE_REPO` / `QTMESH_RIP_CORE_COMMIT` | override fork source/pin for `scripts/build-ps1-rip-core.sh` (testing only) |

### Reconstruction tiers (#816)

`MeshReconstructor::vertexFromPsx` picks per vertex:

- **Tier 0 — GteTracked:** the record's raw `(vx,vy,vz)` → `modelToEditor`. No inversion
  at all; exact model-space geometry. Prims group by their record's real `(rt, tr)`
  matrix (majority vote across vertices; `mixedMatrixPrims` counts disagreements), and
  instances store the full matrix (`ReconstructedInstance::rot/trWorld/hasMatrix`) so
  scene assembly can apply real rotations.
- **Tier 1 — DepthOnly:** PGXP-precise screen `(preciseX, preciseY)` + `viewW` make the
  inverse well-posed — this is exactly the `sz` the #675 math was built for.
- **Tier 2 — None:** the pre-fork world, byte-identical behavior (screen-space fallback).

The fixed `kMaxVertexRadius` gate applies to Tier 2 only; tracked tiers use a
percentile-based outlier policy (`outlierDroppedVertices`). Status bar shows
`tracked N% · depth M%`; the `slabLike` warning remains the regression canary — any
tracked capture that still reports slabLike is a bug.

### Testing without ROMs

`tests/fake_rip_core/` builds a fake libretro core exporting the rip ABI; UnitTests load
it through the real plugin + trampolines and stream a scripted tracked cube
(`InCoreRipCapture_test.cpp`): handshake, ABI-mismatch refusal, `QTMESH_PS1_RIP_INCORE=0`,
armed mirroring, record→draw correlation, provenance tiers, RAM-pass suppression.

### MCP drive-and-verify surface

Seven `ps1rip_*` MCP tools (`ps1rip_start` / `run_frames` / `capture` / `stats` / `status`
/ `stop` / `clear`, `MCPServer.cpp`, `ENABLE_PS1_RIP`-guarded) drive a full capture
headlessly over the MCP stdio/HTTP API and return the tier breakdown, so the pipeline can
be validated without a human at the GUI. `save_scene` then exports the captured nodes to
glTF. Verified end-to-end on Crash Warped: start → play → capture → export a real
`.gltf2`+`.bin`+`.material`.

### Observed capture quality (real retail titles)

Live runs on Crash Bandicoot Warped (proper MODE2/2352 `.cue`, in-core hooks active)
confirm the chain produces recognizable, non-slab 3D at scale (single frames up to ~800k
tris). Two title-dependent characteristics to be aware of:

- **Tracked-vs-depth ratio is a function of capture LENGTH, not tag survival.** A short
  single-frame capture on Warped reconstructs ~90-93% GteTracked; a long accumulation drops
  to single digits, the rest arriving as DepthOnly (PGXP-precise screen + view depth,
  inverted per-draw — still real 3D via the well-posed inverse, just not the exact
  object-space record). **Root cause (measured, Step 3 investigation):** the `rip_tag`
  itself survives perfectly — instrumenting the fork showed MFC2 GTE→CPU tag survival at
  **100%** and GP0-draw-site tag validity at **100% of XY-valid vertices**. The loss is
  entirely host-side and entirely the sx/sy *backstop* in `RipperHooks::resolveTrackedDraws`:
  the host maps a shadow's ring index (`seq % 65536`) to a captured record, but the core's
  65536-entry ring wraps within a busy multi-frame accumulation (a scene pushes >200k
  records), so `m_gteRingToBuffer` — which persists across the whole capture — ends up
  resolving an earlier frame's draw to a *later* frame's record that reused the slot; the
  sx/sy mismatch then correctly rejects it and the vertex degrades to DepthOnly. A short
  capture never wraps, so it stays ~100% tracked.
  - **Practical guidance:** for the highest tracked ratio, **arm briefly and capture ONE
    frame** (or a short scene) of the object you want. Long accumulations still reconstruct
    fully — they just tilt toward the DepthOnly tier, which is equally usable geometry.
  - **A proper fix is non-trivial:** the naive "reset the ring map per frame" makes it
    *worse* (0% tracked), because the worker's accumulate path interleaves `onGteRecords` /
    `onGpuDrawTracked` / `onCoreFrameEnd` across `retro_run` ticks in a way that doesn't line
    up with the record `frame` counter — a frame's buffered draws are resolved against a map
    populated by a different tick. The real fix needs the ABI to carry the full monotonic
    `seq` in each vertex shadow (not just `seq & MASK`) so the host can validate the exact
    record identity instead of a wrapping ring slot — an ABI-version bump and fork change.
    Deferred; the short-capture path already delivers ~90%+ tracked today.
  - The `tracked_only` clean-up filter drops the screen-space Tier-2 junk regardless of ratio.

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

- **Related paths:**
  - `#675` — heuristic + math fix for the screen-space inverse path (landed; tighten matrix scanner,
    real-rotation inverse roundtrip tests, surface `prims_with_matrix=X/N`).
  - `#813`–`#817` — the forked-beetle **in-core capture** issue set (GTE RTPS/RTPT records +
    PGXP-tagged GP0 correlation + tiered reconstruction) for ground-truth model-space
    recovery on any game including custom engines (Crash, Spyro, FFVII field models, MGS).
    (The doc's original `#676`/`#677` links are stale — those numbers were taken by merged PRs;
    disc/ISO RSD/PLY/MAT/GRP/TIX off-line ripping remains a separate future CLI/MCP follow-up.)

- **Disable:** `QTMESH_PS1_TMD_SCANNER=0` skips the TMD pass entirely (debug / golden
  baselines). HMD scanner is opt-in (`QTMESH_PS1_HMD_SCANNER=1`).

## Coordinate normalization & affine-UV handling (#424)

Captures land in the editor through three different source-specific conversions
that all share the same target basis (right-handed Y-up, CCW front-face winding,
~FBX/glTF magnitude):

| Source path | Conversion | Comment |
|-------------|-----------|---------|
| `GteInverse::modelToEditor` | `(mx, my, mz) × 0.01 → (wx, -wy, -wz)` | Y- and Z-negated → determinant +1 → CCW winding preserved |
| `GteInverse::psxScreenToWorld` | `(sx − 160, sy − 120, sz) × 0.01 → (wx, -wy, wz)` | fallback path used when sz=0 (see #675) — single Y flip |
| `PsxTmdRamScanner` | `(x, y, z) × 10/4096`, 180° Z-rotation, CCW index swap | mirrors `PS1TMD::importTmd` so on-disk and RAM paths agree |

`Ps1CoordinateNormalizer` (`runtime/Ps1CoordinateNormalizer.{h,cpp}`) layers a
user-controllable **scale + per-axis flip + perspective-correct UV** override
on top of those built-in conversions. The state lives on `PS1RipManager` and is
edited from the new "Normalize" dock in `PS1RipSessionWindow`.

### How each control composes

- **Scale (`userScale`, default 1.0)** — multiplied into BOTH the SceneNode
  scale AND its position alongside the placement auto-fit, so multi-instance
  deduped captures (a city of buildings, a row of props) scale as one coherent
  assembly instead of each mesh growing/shrinking around its own pivot.
  `placementScale` AND the raw capture-time `(inst.px, inst.py, inst.pz)` are
  stashed in `node->getUserObjectBindings()` as `ps1RipPlacementScale` /
  `ps1RipBasePosition`, and `Ps1CoordinateNormalizer::composeNodeTransform`
  rebuilds both transforms from those base values on every live toggle. The
  issue's stated "default 1/4096" is the
  PSX 12.4 fixed-point divisor; we expose it as a slider rather than as the
  baked default because freshly-loaded captures need to land at the same
  magnitude as FBX/glTF imports (acceptance criterion). Users who want raw
  PSX-native magnitude can dial the slider down to ≈0.024 (= 1/4096 / 0.01).
- **Per-axis flip (`flipX/Y/Z`, default off)** — applied as ±1 multipliers in
  the SceneNode scale AND mirrored into the SceneNode position so two
  instances at `(x, +1, 0)` and `(x, -1, 0)` swap places under a Y flip rather
  than collapsing onto each other. An odd number of negated axes produces a
  negative transform determinant; Ogre auto-flips back-face culling for those
  nodes so the visible front face stays correct **without any per-vertex
  winding swap in the mesh data**. This is what makes "per-axis flip toggles
  work without re-capturing" — no mesh rebuild needed.
- **Perspective-correct UVs (default off)** — when on, **textured** prims whose
  vertex depth ratio max(sz)/min(sz) exceeds `perspectiveTolerance` (default
  1.3) get split into 4 sub-tris via midpoint triangulation in
  `MeshReconstructor::emitTriSubdivided`. New midpoint UVs use screen-space
  linear interpolation (the PS1 affine convention) so Ogre's perspective-
  correct rendering of the resulting fine mesh approximates what the original
  PS1 GPU showed — the "warped quad fix" used by modern PSX remasters.
  Mono / shaded prims (no UV channel — HUDs, flat-shaded geometry) are
  skipped, so the triangle count of non-textured geometry never inflates.
  Recursion is bounded by `perspectiveMaxDepth` (default 3 → 4³ = 64 sub-tris
  per input prim worst case). Bakes into mesh data, so this **does** require a
  fresh capture after toggling. Prims with sz=0 (GP0-only captures, #675) skip
  subdivision because there's no usable depth signal.

### Live updates

`PS1RipManager::setNormalizerSettings` calls
`Ps1CoordinateNormalizer::applyToCaptureNodes`, which walks every
`PS1Capture_*` SceneNode in `Manager::getSceneNodes()` and re-applies the new
scale tuple. Cost: O(scene nodes), no Ogre mesh / material rebuild. Emits a
`ps1.rip.coord.normalize` Sentry breadcrumb with the compact descriptor
returned by `Ps1CoordinateNormalizer::describe` and the node-touched count.

Settings persist to `QSettings` under `ps1Rip/normalize/*` so a user's
per-game tweaks survive across sessions. Values are clamped on load
(`userScale ∈ [0.001, 1000]`, `perspectiveTolerance ∈ [1.0, 1000]`,
`perspectiveMaxDepth ∈ [0, 6]`) so a corrupted ini can't bake invisible or
explosively-scaled capture nodes.

### Bounds & framing

`PS1RipMeshBuilder::buildMeshResources` already calls
`ogreMesh->_setBounds(bounds)` per unique mesh (#658), so `SpaceCamera::
frameSelection()` (`F` shortcut) picks the captured AABB up via
`getWorldBoundingBox(true)` without any extra plumbing. Hit `F` on any
`PS1Capture_*_inst*` node to frame it.

### Test coverage

- `Ps1CoordinateNormalizer_test.cpp` — settings roundtrip, describe, isDefault,
  per-axis sign math, YDownQuad → YUp winding behaviour, clamp on corrupted
  ini values.
- `MeshReconstructor_test.cpp` — perspective-correct subdivision tessellates a
  warped quad, no-ops on flat prims, gracefully handles sz=0 GP0 captures.

## Capture controls + scene capture (#425)

The session window (`PS1RipSessionWindow`) hosts the full capture surface:
emulator viewport, transport, capture actions, Normalize dock (#424), VRAM
viewer, status bar. Capture is split into three lanes, all routed through the
single `PS1RipManager` singleton:

- **Capture Frame** (`captureFrame()` → worker `finalizeFrameCapture`) — the
  legacy single-shot path. Requires the buffer to already be armed; takes a
  snapshot of whatever the worker has accumulated since arming and emits
  `frameCaptured` → `meshBuilt`. Sentry category: `ps1.rip.capture.frame`.
  Hotkey: `C`.
- **Capture Scene** (`captureScene(seconds)`) — multi-frame capture. Auto-arms
  if not already armed, starts a 1 Hz countdown on the GUI thread, emits
  `sceneCaptureStarted/Progress` so the UI footer can show the timer, and on
  expiry queues `finalizeFrameCapture` on the worker — the same code path
  Capture Frame uses, so deduping + mesh build + breadcrumbs are identical.
  When the worker's `frameCaptureReady` returns the manager sees the
  `m_sceneCaptureAwaitingResult` flag and emits `sceneCaptured(captureId)` +
  `sceneCaptureFinished(false, captureId)` so the UI can flip out of scene
  mode. Sentry category: `ps1.rip.capture.scene` (with `started` / `finalised`
  / `cancelled` payloads). Hotkey: `Shift+C`. Default duration: 5 s, range
  1–60 s, persisted in `QSettings ps1Rip/sceneCaptureSeconds`.
- **Dump VRAM** (`dumpVRAM()`) — snapshot the GPU VRAM mirror to PNG. Sentry
  category: `ps1.rip.capture.vram`. Hotkey: `V`.

**Stop Capture** (`stopSceneCapture()` + `armCapture(false)` chained in the UI)
cancels an in-flight scene capture and disarms in one click. The toolbar
action is disabled while no scene capture is running; the same cancellation
path fires on session stop and on Arm Capture untoggled mid-countdown so the
UI never gets stuck in "scene capture in flight" state.

**Live status footer** — the worker emits a throttled `captureProgress(prims,
triangles, texPages, bytes)` signal every 15 frames (~4 Hz at 60 FPS) while
armed; the manager forwards it to the GUI, which renders a separate
right-aligned status-bar label. Format: `Armed · 1234 tris · 8 tex pages ·
256 KiB` (idle) or `Scene capture 3/5 s · 1234 tris · …` (during a scene
capture). On disarm / session stop the counters reset and the footer clears.

**Hotkeys** are `QShortcut`s scoped to `Qt::WindowShortcut`, so they only fire
when the session window has keyboard focus — they don't shadow `C/V/Shift+C`
in the rest of the editor.

The "Auto-show *Extracted Asset Browser* on first capture" line in the issue's
scope depends on #426 (Extracted Asset Browser) which is a separate epic
deliverable. Once #426 lands, the natural hook is `sceneCaptured` /
`frameCaptured` in `PS1RipManager` → show the browser dock; the signals are
already in place.

### QML migration note

The issue's scope mentions a `qml/PS1RipperDock.qml` rewrite. The session UI
currently lives in `QMainWindow + QWidget` (the central `EmuViewport` renders
emulator framebuffers via `QPainter` — converting it to a `QQuickItem` is a
non-trivial follow-up). The functional acceptance criteria — Reset / Pause /
Step / Reload / Capture Frame / Capture Scene / Dump VRAM / Stop, dedupe
toggle + scale + perspective-correct UVs (#424), status footer numbers,
Sentry breadcrumbs — are all met in the existing widget shell. A pure-QML
port can ship later without changing the manager API.

## Geometry inspector + extracted-asset browser (#426)

Built on top of the captured data flow finalised in #424/#425. The goal is
to let the user see *what was captured*, drill from a draw call down to a
sub-mesh in the live viewport, and pick which captured assets become
permanent (i.e. survive a Stop or the next Capture).

### Captured asset store

`PS1CapturedAssets` is a GUI-thread singleton that mirrors the last
successful `meshBuilt` into three data flat collections:

- `rows`: one `CapturedAssetRow` per `PrimRecord` in the capture
  (`#`, `frame`, `type`, `verts`, `tpage`, `clut`, `matrix#`,
  `material`, `triangles` — the columns the issue specs).
- `uniqueMeshes` + `instances`: pass-through of the dedupe output
  from `MeshReconstructor::reconstructDeduped` (#423).
- `instanceNodeNames`: instance index → Ogre SceneNode name
  (`PS1Capture_<id>_inst<N>`), so the inspector resolves a click into a
  real node without re-walking the scene tree.
- `textureImages`: decoded texture page (256×256 RGBA) keyed by logical
  material name. Populated by `PS1RipMeshBuilder` from the `TextureDecoder`
  cache so the browser doesn't have to re-decode VRAM.

Update flow: the manager assembles the set via
`PS1CapturedAssets::buildFromCapture()` on the main thread immediately
before emitting `meshBuilt`, so any UI listening to that signal is
guaranteed to see a fully-populated store.

### Geometry Inspector dock

`PS1GeometryInspectorPanel` (`PS1GeometryInspectorModel` +
`PS1GeometryInspectorFilter` + Qt UI) lives inside `PS1RipSessionWindow`
as a bottom dock. Single-click on a row routes through
`PS1RipSessionWindow::highlightInspectorRow`, which resolves the row's
instance to its SceneNode and calls `SelectionSet::selectOne` so the
editor outlines the sub-mesh.

Right-click context menu (the issue's three required actions):

- **Hide / Show this submesh** — toggles `SceneNode::setVisible`. The
  store records `row.hidden` so the inspector keeps the hidden rows
  in-view but greyed out.
- **Promote to permanent entity** — clones the underlying Ogre mesh into
  a fresh `PS1Imported_<id>_meshN_pK` resource and creates a brand new
  `SceneNode` carrying the same world transform as the capture node.
  Survives the next Capture / Stop because the live capture cleanup
  only walks `PS1Capture_*` names. Emits a
  `ps1.rip.inspector.promote ok mesh=... node=...` Sentry breadcrumb.
- **Discard** — hides the SceneNode and grey-outs the row. Reversible
  via "Restore (un-discard)" which re-shows the node.

Filter chips above the table: `Textured`, `Colored`, `Instanced`, plus a
"Hide discarded" toggle and a free-text Material search box. The filter
is a `QSortFilterProxyModel` so toggling a chip is a single
`invalidateFilter()` — no model rebuild.

Counts header: `Captured: N prims · M unique meshes · K textures`,
recomputed from the store on every `captureSetChanged()`.

### Extracted Asset Browser dock

`PS1ExtractedAssetBrowser` is a separate right-dock with a QTabWidget
holding three `QListView` (IconMode) grids:

- **Meshes** — one tile per `uniqueMesh` with a colored-swatch
  placeholder thumbnail (Sobel-style hue derived from the first
  submesh's material name, so the same mesh shows the same swatch across
  captures). Tiles are drag-enabled (`Qt::ItemIsDragEnabled`); the model
  exposes the asset id under MIME type `application/x-ps1rip-mesh` and
  the mesh index under `application/x-ps1rip-meshindex`.
- **Textures** — one tile per decoded texture page, thumbnail is the
  256×256 page itself scaled to 96 px.
- **Materials** — one tile per logical material name (textured pages +
  the synthetic `PS1Rip_color`). Textured materials use the page image;
  solid materials use a hue swatch.

Drag-and-drop: `MainWindow::dropEvent` recognises the two MIME types and
calls `PS1RipSessionWindow::promoteUniqueMeshById(meshIndex, assetId)`,
which routes through the same `promoteUniqueMesh` impl as the inspector
context menu. So both "double-click in the browser" and "drag a mesh
tile into the editor viewport" satisfy the acceptance criterion
"Drag-and-drop creates a permanent entity that persists after Stop".

Filter chips (`Textured`, `Colored only`, `Instanced`) and a search box
share state across all three tabs — same `QSortFilterProxyModel`
template, one chip toggle invalidates every tab's filter.

### Sentry breadcrumbs

`ps1.rip.inspector.promote` is emitted from both the inspector right-click
and the asset-browser instantiation paths, with the source tag
(`row-context-menu`, `browser-doubleclick`, `dropEvent`) baked into the
message so telemetry can tell them apart. `ui.action` breadcrumbs cover
each chip toggle and search edit for follow-up analytics.

### Row provenance (post-review #679)

A `materialName → first uniqueMeshIndex` lookup was the original
attribution path inside `buildFromCapture`. It collapsed every solid-
color row onto the first mesh that contained `PS1Rip_color`, so two
prims drawn by different matrix groups would compete for the same
inspector handle — highlight, hide, discard, and promote all targeted
the same node, breaking the per-row semantics #426 promised.

The corrected path uses **per-prim provenance** emitted by
`MeshReconstructor::reconstructDeduped` in `ReconstructedCaptureSet::primProvenance`,
parallel to `CaptureSnapshot::prims`:

- `meshFromMatrixGroup` records `(texKey → subMeshIndex)` for the part
  it builds.
- `buildParts` records `(matrixId → partIndex)` and stashes the inner
  per-part `(texKey → subMeshIndex)` map.
- `reconstructDeduped` walks parts in order, so `instanceIndex == partIndex`,
  and a `(partIndex → uniqueMeshIndex)` table folds the dedupe step in.
- For each `PrimRecord`, `(matrixId, texKey)` resolves to
  `(uniqueMeshIndex, subMeshIndex, instanceIndex)` — that's the row's
  authoritative scene location.

`PS1CapturedAssets::buildFromCapture` consumes the provenance when its
length matches `snapshot.prims`; otherwise it falls back to the legacy
material-name lookup so hand-built `ReconstructedCaptureSet` fixtures
(unit tests, tooling experiments) keep working.

The inspector's row handlers consume `subMeshIndex` to scope
`setVisible` / `selectOne` to the exact `Ogre::SubEntity`, and
`promoteUniqueMesh` accepts an optional `instanceIndex` so right-click
**Promote** on row N copies that row's specific matrix transform
instead of the first instance using the mesh.

### Promoted-asset material lifetime

`beginCaptureAttach()` purges every Ogre resource matching the
`PS1Rip_*` material / `ps1rip_*` texture pattern at the start of the
next capture. A promoted mesh that still referenced those materials
would render untextured/black the moment the user took a second
capture. `rebindPromotedMaterials()` (called inside `promoteUniqueMesh`
after `Mesh::clone`) walks the cloned submeshes, clones any
capture-scoped material under `PS1Imported_<captureId>_p<promoId>_*`,
copies its texture buffers into a sibling `ps1imported_*` texture,
rebinds the TUS, and binds the cloned material back on the submesh.
The clones live in the default resource group and are out of scope of
the purge regex, so promoted entities survive an arbitrary number of
subsequent captures.

### Tests

`PS1CapturedAssets_test.cpp` exercises:

- `buildFromCapture` row attribution (textured/colored prim → matching
  unique mesh + instance index) via the legacy material-name fallback.
- `instanceNodeNames` matches `PS1RipMeshBuilder`'s scene-node naming.
- `setCaptureSet` fires `captureSetChanged` exactly once.
- `setRowHidden` / `setRowDiscarded` fire `rowChanged` on real changes
  and no-op for redundant / out-of-range mutations.
- Provenance disambiguates rows that share a material across multiple
  unique meshes (regression test for the original bug).
- Provenance carries per-row submesh indices when two prims drawn by
  the same matrix use different materials.
- `PS1ExtractedAssetBrowser::buildTilesForKind` produces the expected
  tiles for Mesh / Texture / Material kinds.

`MeshReconstructor_test.cpp::DedupesIdenticalInstances` also pins the
per-prim instance ordinal so a future refactor of the part-to-instance
ordering doesn't silently regress provenance.

These are pure-data unit tests — no Ogre, no QWidget tree — so they run
on the headless CI fixture without an X server.

### Open follow-ups

- Mesh thumbnails are currently colored-swatch placeholders. An Ogre
  off-screen RTT (similar to `ModelTurntableRenderer`) would produce
  real renders without changing the public model/view API.
- Promotions inherit the capture node's auto-fit scale and normalizer
  flips. A "Reset to identity transform" option in the right-click menu
  would let users decouple a promoted entity from those view-time
  adjustments.

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

**First question: does the status bar say `in-core hooks: active`?** If not, you are on
a stock core and every capture is screen-space heuristics — install the rip fork
(`scripts/build-ps1-rip-core.sh <build>/bin/PS1Cores`) and re-capture. With the fork
armed, the mesh stats line shows `tracked N% · depth M%`; `tracked 0%` there means the
in-core stream isn't flowing (check `QTMESH_PS1_RIP_INCORE`, and that capture was armed
while the scene rendered).

**`in-core hooks: active` but `tracked 0%` AND `gte records: 0`?** The fork handshook
but its GTE/GP0 hooks are compiled out. beetle's Makefile does not track
`HAVE_QTMESH_RIP` per object, so an **incremental** build over objects compiled without
the flag silently `#ifdef`s the capture code away while the ABI exports still link — the
core reports active and records nothing. Fix: clean-build the fork
(`make clean && make HAVE_QTMESH_RIP=1 …`, which `scripts/build-ps1-rip-core.sh` now
always does). Confirm with `nm pgxp/pgxp_gte.o | grep SetRipTag` — the symbol must be
present.

**`in-core hooks: active`, hooks compiled in, but still zero GTE records?** Then the CPU
executed no `RTPS`/`RTPT` — the game isn't rendering GTE geometry in the frames you
captured. Two common causes: (1) you captured during an **FMV / 2D title / BIOS shell**
(MDEC + sprites use no GTE) — play into an actual 3D scene before **Capture Frame**;
(2) the disc didn't boot. A cooked **MODE1/2048 `.iso`** mounts and shows a picture but
some rips stall the BIOS shell; `QTMESH_PS1_SKIP_BIOS=0` boots through the real BIOS
(watch for a live, changing framebuffer vs a frozen one), and `.cue`/`.chd` images boot
more reliably than bare `.iso`. The `qtmesh` GTE hook lives in the **interpreter**
(`mednafen/psx/gte.c`); if a future build enables the lightrec dynarec by default the
hook is bypassed — keep `cpu_dynarec` unset/`run_interpreter` while ripping.

For a headless boot-and-capture with tier numbers, build UnitTests with
`-DENABLE_PS1_LIBRETRO_INTEGRATION_TESTS=ON` and run
`QTMESH_PS1_TEST_BIOS=… QTMESH_PS1_TEST_ISO=… ./UnitTests --gtest_filter='InCoreRipLiveTest.*'`
(env: `QTMESH_PS1_RIP_BOOT_FRAMES`, `QTMESH_PS1_RIP_CAPTURE_FRAMES`, `QTMESH_PS1_SKIP_BIOS`).
Reaching a specific in-game 3D scene is far easier in the GUI where you can see the
screen and drive the pad — the headless harness is best for scenes that render 3D
immediately on boot.

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
   On GP0-only captures this is **expected to be 0%** because the GP0 stream carries no
   per-vertex depth (see the "GP0 carries no depth" bullet in the section above) — the
   inverse refuses to run, callers fall back to `psxScreenToWorld`, and the result is a
   flat-XY blob *that you can still see in the viewport*. Healthy non-GP0 capture
   (model-space scanner, model-projected test fixture) is ≥ ~50%. If 0% **and** you
   expected real model-space, jump straight to step 3.
2. `matrix tag X/Y` — how many primitives were associated with a captured matrix.
   `0/Y` means matrix association is the bottleneck (no `0xE4` draw-environment packets
   captured, or per-draw matrix tagging from #658 didn't run). `Y/Y` with low inverse %
   means either depth is missing (GP0-only — expected) **or** the math is rejecting the
   matrices because they aren't orthonormal (often because the title uses a custom
   transform stack).
3. `tmd N / hmd N` — model-space scanner hits (#674). If ≥ 1, the primary source flips
   to `ram_model_mesh` and you're on the clean-mesh path regardless of the screen-space
   stats. **This is the only path that produces ground-truth model-space meshes on retail
   games today.**

### Captured scene tree has nodes but viewport is empty (mesh exists, invisible)

This is the failure mode the first build of #675 hit: the math fix made
`screenToModel` correct, but with `sz == 0` on every GP0 vertex the inverse collapsed
every prim of a given matrix to a single point. The mesh existed (`PS1Capture_*_inst*`
nodes in the scene tree, non-zero vertex/triangle counts in the status bar) but had
zero extent and rendered as nothing. The `sz == 0` guard in `GteInverse::screenToModel`
now forces the `psxScreenToWorld` fallback in this case — verify by looking at the
status bar:

- `GTE inverse 0% (matrix tag X/Y)` with `X == Y` and a visible blob: **correct
  post-guard behaviour** on GP0-only captures. The blob is ugly but visible, and is the
  best the screen-space path can do without depth.
- `GTE inverse 100%` with an invisible scene-tree mesh: the guard is bypassed
  somewhere. Confirm `PsxVertex::z` is 0 on the inputs (it should be — nothing in the
  GP0 capture path writes it) and that `screenToModel` returns false for `sz == 0` in
  `GteInverseTest.ScreenToModelRefusesZeroDepth`.

The recommended path for "real meshes from real games" is now the **in-core capture
chain** (#813–#817) with the qtmesh beetle fork — check that the session status bar says
`in-core hooks: active` first. The **model-space TMD/HMD RAM scanner** (#674) remains a
complementary exact source for Sony SDK titles and the fallback for stock cores, where
matrix-to-draw linkage stays heuristic.

- **TMD-using games (clean meshes today via #674):** Tekken 1/2/3, Ridge Racer 1/RR,
  Net Yaroze SDK demos, Wipeout 1/2097, R-Type Delta, Klonoa, many pre-FF7 Square titles.
- **Custom-engine games:** Crash, Spyro, FFVII field models, MGS post-Yaroze. These
  games author their own packed mesh layouts and bespoke transform stacks; only the
  in-core capture chain (#813–#817, qtmesh beetle fork) covers them — the GTE hook sees
  their vertices regardless of engine layout.
- Tune via `QTMESH_PS1_TMD_SCANNER=0` to disable TMD scanning for a baseline, and
  `QTMESH_PS1_HMD_SCANNER=1` to opt into the v1 HMD candidate counter.

If the scanner produces meshes but they look wrong (wrong axis flip, wrong scale), the
coordinate transform in `PsxTmdRamScanner` mirrors `PS1TMD::importTmd` — if the on-disk
TMD importer is also wrong, fix it in `PS1TMD.cpp` and the RAM scanner picks it up via
the matching constants. Per-draw matrix tagging (#658) and the live FIFO bridge (#662)
still help on the screen-space side but cannot reconstruct topology from screen-space
prims alone.

### Captured mesh is the wrong size, flipped, or has visible texture warping

The "Normalize" dock in the rip session window (#424) is the user-facing
override for these:

- **Wrong size** — drag the **Scale** spinbox. 1.0 (default) matches FBX/glTF
  magnitude; ≈0.024 gives raw PSX-native (1/4096) for engine-side math.
- **Wrong axis / upside-down** — toggle **Flip X / Y / Z**. The change applies
  live to existing capture nodes; no re-capture needed. Pair two flips to keep
  CCW winding when chasing a Z-up game into editor Y-up.
- **Texture wobble looks wrong on near-camera surfaces** — enable
  **Perspective-correct UVs** and re-capture. Triangles with high depth
  variance get subdivided + midpoint-resampled so Ogre's perspective-correct
  rendering reproduces the artist's affine intent (the PSX-remaster fix).

All four are persisted to `QSettings` under `ps1Rip/normalize/*` so per-game
tweaks survive across sessions. Sentry: each setter call emits
`ps1.rip.coord.normalize` with the compact descriptor.

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

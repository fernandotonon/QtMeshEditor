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

## Open questions

- Exact plugin ABI for `EmuCore` (#415).
- BIOS / ISO first-run legality dialog copy (#417) — requires legal review before release.

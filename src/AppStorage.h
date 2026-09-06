#pragma once

#include <QString>
#include <QStringList>

/// Where QtMeshEditor keeps on-disk data.
///
/// Under Snap, `QStandardPaths::AppDataLocation` resolves inside
/// `$SNAP_USER_DATA` (`~/snap/<app>/<revision>/…`). Snap **copies that
/// tree on every refresh**, so multi-GB AI weights there fill the disk.
/// Snap's recommendation for large / durable user data is
/// `$SNAP_USER_COMMON` (`~/snap/<app>/common/`), which is **not** copied
/// between revisions.
///
/// Non-Snap builds keep the historical AppDataLocation layout (unchanged).
namespace AppStorage {

/// Running inside a snap confinement (SNAP / SNAP_USER_COMMON set).
bool isSnap();

/// Revision-scoped app data — fine for tiny state; avoid large downloads.
QString revisionScopedRoot();

/// Durable root across snap refreshes (`$SNAP_USER_COMMON/QtMeshEditor` when
/// snap; otherwise `revisionScopedRoot()`).
QString persistentRoot();

/// `persistentRoot()/ai_models` — ONNX/GGUF catalog downloads.
QString aiModelsRoot();
/// Stable-Diffusion weights (`sd_models`).
QString sdModelsRoot();
/// Local LLM GGUFs (`models`).
QString llmModelsRoot();
/// User-downloaded HDRIs (`hdri`).
QString hdriRoot();
/// IBL bake cache (`hdr_cache`).
QString hdrCacheRoot();

/// Subdirectory names that must live under `persistentRoot()` (snap-safe).
QStringList heavySubdirNames();

/// One-shot: move heavy subdirs from revision-scoped AppData (and a known
/// accidental nested `…/QtMeshEditor/QtMeshEditor/…` layout) into
/// `persistentRoot()`. No-op when already migrated (marker present and
/// current revision has no leftover heavy dirs) or not snap. Also rewrites
/// persisted LLM/SD `modelsDirectory` settings that still point at legacy
/// revision-scoped defaults. Prefer rename (same filesystem); never copies
/// multi-GB trees.
void migrateHeavyDataFromRevisionScopedStorage();

} // namespace AppStorage

#pragma once

// Apply one or more interchange motions (Kimodo variations, or saved
// motion_NN.npz files) to a LIVE scene entity as new skeletal clips: the
// in-session counterpart of AIMotionCLI. Shared by the MCP tool and the
// Animation panel so the two cannot drift.
//
// Every variation becomes its own clip (`<base>`, `<base>_v2`, ...; names are
// made unique against the skeleton, existing clips are never overwritten) and
// the whole batch is ONE undo step (a macro of RetargetAnimationCommand).

#include "SomaRetarget.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <array>
#include <string>
#include <vector>

namespace Ogre { class Entity; class Skeleton; }

namespace AIMotion {

struct SceneApplyOptions {
    QString baseName = QStringLiteral("generated_kimodo");
    bool rootMotion = true;
    bool footPin = true;
    bool undoable = true;      ///< push one undo macro (off for headless one-shots)
    int smoothFps = 12;        ///< canonical clips: sparse smoothing bake rate (0 = off)
};

struct SceneApplyResult {
    bool ok = false;
    QString error;
    QStringList clips;                 ///< written clip names, one per motion
    std::vector<ApplyOutcome> outcomes;
    int undoIndexAfter = -1;           ///< the undo stack index right after this batch's macro (-1 = not undoable)
};

/// Take back a batch applyMotionsToEntity wrote (e.g. when the caller's export then failed): undo its macro when it
/// is still the newest undo step, else remove the clips directly.
void rollbackAppliedMotions(Ogre::Entity* entity, const SceneApplyResult& applied);

/// The clip name for variation `index` (0-based) before uniquing: base, base_v2, ...
QString variantClipName(const QString& base, int index);

SceneApplyResult applyMotionsToEntity(Ogre::Entity* entity, const std::vector<Motion>& motions,
                                      const SceneApplyOptions& options = {});

/// QtMesh T2M output: 22-role canonical WORLD-frame clips, retargeted through AnimationMerger::applyMotionClip (the
/// template-library path) + the sparse smoothing bake + foot pinning. Same naming, rollback and one-undo-step
/// contract as applyMotionsToEntity; `outcomes` stays empty (no SOMA joint report).
struct CanonicalMotion {
    std::vector<std::vector<std::array<float, 4>>> quats;   ///< [frames][22]
    int fps = 30;
    std::vector<std::array<float, 4>> restWorld;
    std::vector<std::array<float, 3>> restDir;
};
/// Root travel for a rotation-only clip: per hips keyframe the LOWER foot is taken as planted and the hips move
/// by the opposite of its horizontal slide (plane across the bind hips->head axis). Feet must map to canonical
/// roles 17/21. False (with `error`) when the rig or clip cannot take it; the clip is then left unchanged.
bool addFootDrivenRootMotion(Ogre::Skeleton* skel, const std::string& clip, QString* error = nullptr);

/// `options.rootMotion` adds addFootDrivenRootMotion (the model itself is rotation-only).
SceneApplyResult applyCanonicalMotionsToEntity(Ogre::Entity* entity, const std::vector<CanonicalMotion>& motions,
                                               const SceneApplyOptions& options = {});

/// Per-clip report (mapped / unmapped / excluded joints) shared by the CLI and MCP JSON.
QJsonObject outcomeJson(const ApplyOutcome& out);

/// Load every file; stops at the first failure with "<file>: <reason>".
bool loadMotionFiles(const QStringList& files, std::vector<Motion>* out, QString* error);

} // namespace AIMotion

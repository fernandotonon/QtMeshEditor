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

#include <vector>

namespace Ogre { class Entity; }

namespace AIMotion {

struct SceneApplyOptions {
    QString baseName = QStringLiteral("generated_kimodo");
    bool rootMotion = true;
    bool footPin = true;
    bool undoable = true;      ///< push one undo macro (off for headless one-shots)
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

/// Per-clip report (mapped / unmapped / excluded joints) shared by the CLI and MCP JSON.
QJsonObject outcomeJson(const ApplyOutcome& out);

/// Load every file; stops at the first failure with "<file>: <reason>".
bool loadMotionFiles(const QStringList& files, std::vector<Motion>* out, QString* error);

} // namespace AIMotion

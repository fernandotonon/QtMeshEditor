#include "SkinWeightsController.h"
#include "GamificationManager.h"
#include "SkinWeights.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/ComputeSkinWeightsCommand.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>

SkinWeightsController* SkinWeightsController::m_pSingleton = nullptr;

SkinWeightsController* SkinWeightsController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new SkinWeightsController();
    return m_pSingleton;
}

SkinWeightsController* SkinWeightsController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void SkinWeightsController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

SkinWeightsController::SkinWeightsController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &SkinWeightsController::selectionChanged);
}

bool SkinWeightsController::hasSkinnedSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty()) return false;
    Ogre::Entity* first = entities.first();
    if (!first || !first->getMesh()) return false;
    return first->getMesh()->getSkeleton() != nullptr;
}

QVariantMap SkinWeightsController::computeWeightsForSelected(int maxInfluencesPerVertex,
                                                              double falloff,
                                                              double maxInfluenceDistance,
                                                              bool skipUnweightedBones,
                                                              bool replaceExisting,
                                                              const QString& algorithm,
                                                              int voxelResolution,
                                                              int smoothIterations)
{
    QVariantMap result;

    // Breadcrumb the UI action up front so failed attempts (no
    // selection, no skeleton, etc.) still reach Sentry — the
    // per-operation `ai.assist.skin_weights` breadcrumb below is
    // only emitted once we've passed validation.
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Compute Skin Weights requested"));

    auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        const auto msg = QStringLiteral("No selection set.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty()) {
        const auto msg = QStringLiteral("No mesh selected.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }
    Ogre::Entity* entity = entities.first();
    // `getResolvedEntities()` may return a stale / null first entry
    // (e.g. selection resolved against an entity that was just
    // destroyed). `hasSkinnedSelection()` already treats it as
    // nullable; guard here too so we return a user-facing error
    // instead of crashing on `entity->getName()` / computeAndApply.
    if (!entity || !entity->getMesh()) {
        const auto msg = QStringLiteral("Selected entity is no longer valid.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }
    // Pre-check the skeleton here so a skeleton-less mesh fails
    // cleanly WITHOUT leaving a no-op entry on the undo stack
    // (QUndoStack::push runs the command's redo() unconditionally).
    if (!entity->getMesh()->getSkeleton()) {
        const auto msg = QStringLiteral("Mesh has no skeleton attached.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = maxInfluencesPerVertex;
    opts.falloff                = falloff;
    opts.maxInfluenceDistance   = maxInfluenceDistance;
    opts.skipUnweightedBones    = skipUnweightedBones;
    opts.replaceExisting        = replaceExisting;
    opts.voxelResolution        = voxelResolution;
    opts.smoothIterations       = smoothIterations;
    const SkinWeights::Algorithm algo
        = SkinWeights::algorithmFromString(algorithm);

    SentryReporter::addBreadcrumb(
        QStringLiteral("ai.assist.skin.%1")
            .arg(SkinWeights::algorithmToString(algo)),
        QString("UI skin entity=%1 maxInf=%2 falloff=%3 maxDist=%4 voxelRes=%5 smooth=%6")
            .arg(QString::fromStdString(entity->getName()))
            .arg(maxInfluencesPerVertex)
            .arg(falloff).arg(maxInfluenceDistance)
            .arg(voxelResolution).arg(smoothIterations));

    m_busy = true;
    emit busyChanged();

    SkinWeightsReport report;
    try {
        // Run through an undo command so the auto-skin can be
        // reverted with Ctrl+Z. The command snapshots the pre-skin
        // bone assignments, runs `computeAndApply` on its first
        // redo, and restores the snapshot on undo. We construct it,
        // push it (which executes redo() synchronously), then read
        // the report it captured.
        auto* cmd = new ComputeSkinWeightsCommand(
            entity->getName(), opts, algo);
        UndoManager::getSingleton()->push(cmd);
        report = cmd->report();
    } catch (const Ogre::Exception& e) {
        m_busy = false;
        emit busyChanged();
        const auto msg = QString::fromStdString(e.getFullDescription());
        emit error(QStringLiteral("Ogre error: %1").arg(msg));
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }

    m_busy = false;
    emit busyChanged();

    result["applied"]               = report.applied;
    result["meshName"]              = report.meshName;
    result["skeletonName"]          = report.skeletonName;
    result["totalBones"]            = report.totalBones;
    result["totalVerticesProcessed"] = report.totalVerticesProcessed;
    result["totalAssignmentsBefore"] = report.totalAssignmentsBefore;
    result["totalAssignmentsAfter"]  = report.totalAssignmentsAfter;
    result["algorithmUsed"]          = report.algorithmUsed;
    result["fallbackReason"]         = report.fallbackReason;
    if (report.bleedFraction >= 0.0)
        result["bleedFraction"] = report.bleedFraction;

    if (report.applied) {
        GamificationManager::noteOperation(
            QStringLiteral("skin_weights"),
            {{QStringLiteral("verts_weighted"), report.totalVerticesProcessed},
             {QStringLiteral("max_influences"), maxInfluencesPerVertex}});
        emit weightsApplied(result);
    } else {
        // Always populate `error` in the result map, not just when
        // the report carries one — otherwise SkinWeightsDialog's
        // synchronous return path overwrites the emitted message
        // with "unknown error".
        const QString msg = report.error.isEmpty()
            ? QStringLiteral("Skin weights failed")
            : report.error;
        result["error"] = msg;
        emit error(msg);
    }

    return result;
}

#include "SkinWeightsController.h"
#include "GamificationManager.h"
#include "SkinTokensPredictor.h"
#include "SkinWeights.h"
#include "SkinningDisplay.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/ComputeSkinWeightsCommand.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>

#include <QCoreApplication>
#include <QPointer>

#include <algorithm>
#include <thread>

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

    // Validate the algorithm string instead of letting
    // algorithmFromString's default swallow a typo silently, and
    // clamp the new knobs to the same ranges the CLI/MCP enforce.
    const QString algoName = algorithm.trimmed().toLower();
    if (algoName != QLatin1String("skintokens")
        && algoName != QLatin1String("geodesic-voxel")
        && algoName != QLatin1String("inverse-distance")
        && algoName != QLatin1String("unirig")) {   // deprecated alias
        const auto msg = QStringLiteral(
            "Unknown algorithm '%1' — expected 'skintokens', "
            "'geodesic-voxel', or 'inverse-distance'.").arg(algorithm);
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
    opts.voxelResolution        = std::clamp(voxelResolution, 8, 256);
    opts.smoothIterations       = std::clamp(smoothIterations, 0, 50);
    const SkinWeights::Algorithm algo
        = SkinWeights::algorithmFromString(algoName);

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

QString SkinWeightsController::skinningDisplayMode() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return SkinningDisplay::modeToString(SkinningDisplay::Mode::Linear);
    const auto entities = sel->getResolvedEntities();
    Ogre::Entity* entity = entities.isEmpty() ? nullptr : entities.first();
    return SkinningDisplay::modeToString(SkinningDisplay::current(entity));
}

bool SkinWeightsController::setSkinningDisplayMode(const QString& mode)
{
    auto* sel = SelectionSet::getSingleton();
    const auto entities = sel ? sel->getResolvedEntities()
                              : QList<Ogre::Entity*>{};
    Ogre::Entity* entity = entities.isEmpty() ? nullptr : entities.first();
    if (!entity) {
        emit error(QStringLiteral("No mesh selected."));
        return false;
    }

    const auto m = SkinningDisplay::modeFromString(mode);
    SentryReporter::addBreadcrumb(QStringLiteral("render.skinning"),
        QStringLiteral("display mode %1 on %2")
            .arg(SkinningDisplay::modeToString(m),
                 QString::fromStdString(entity->getName())));

    QString err;
    if (!SkinningDisplay::apply(entity, m, &err)) {
        emit error(err);
        return false;
    }
    return true;
}

bool SkinWeightsController::mlSkinnerReady() const
{
    return SkinTokensPredictor::isAvailable()
        && SkinTokensPredictor::modelsPresent();
}

void SkinWeightsController::cancelSkin()
{
    if (m_skinCancel) m_skinCancel->store(true);
}

bool SkinWeightsController::computeWeightsForSelectedAsync(
    int maxInfluencesPerVertex, double falloff, double maxInfluenceDistance,
    bool skipUnweightedBones, bool replaceExisting,
    const QString& algorithm, int voxelResolution, int smoothIterations)
{
    if (m_busy) {
        emit error(QStringLiteral("A skin compute is already running."));
        return false;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Compute Skin Weights (async) requested"));

    auto* sel = SelectionSet::getSingleton();
    const auto entities = sel ? sel->getResolvedEntities()
                              : QList<Ogre::Entity*>{};
    Ogre::Entity* entity = entities.isEmpty() ? nullptr : entities.first();
    if (!entity || !entity->getMesh()) {
        emit error(QStringLiteral("No mesh selected."));
        return false;
    }
    if (!entity->getMesh()->getSkeleton()) {
        emit error(QStringLiteral("Mesh has no skeleton attached."));
        return false;
    }
    const QString algoName = algorithm.trimmed().toLower();
    if (algoName != QLatin1String("skintokens")
        && algoName != QLatin1String("geodesic-voxel")
        && algoName != QLatin1String("inverse-distance")
        && algoName != QLatin1String("unirig")) {
        emit error(QStringLiteral(
            "Unknown algorithm '%1' — expected 'skintokens', "
            "'geodesic-voxel', or 'inverse-distance'.").arg(algorithm));
        return false;
    }

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = maxInfluencesPerVertex;
    opts.falloff                = falloff;
    opts.maxInfluenceDistance   = maxInfluenceDistance;
    opts.skipUnweightedBones    = skipUnweightedBones;
    opts.replaceExisting        = replaceExisting;
    opts.voxelResolution        = std::clamp(voxelResolution, 8, 256);
    opts.smoothIterations       = std::clamp(smoothIterations, 0, 50);
    const SkinWeights::Algorithm algo
        = SkinWeights::algorithmFromString(algoName);

    SentryReporter::addBreadcrumb(
        QStringLiteral("ai.assist.skin.%1")
            .arg(SkinWeights::algorithmToString(algo)),
        QString("UI async skin entity=%1 maxInf=%2 voxelRes=%3 smooth=%4")
            .arg(QString::fromStdString(entity->getName()))
            .arg(maxInfluencesPerVertex)
            .arg(voxelResolution).arg(smoothIterations));

    // MAIN thread: snapshot everything the compute needs (locks Ogre
    // hardware buffers) — milliseconds even on 100k-vert meshes.
    auto job = std::make_shared<SkinWeights::ComputeJob>();
    QString prepErr;
    if (!SkinWeights::prepareJob(entity, opts, algo, *job, &prepErr)) {
        emit error(prepErr);
        return false;
    }

    m_busy = true;
    emit busyChanged();
    m_skinDownloading = (algo == SkinWeights::Algorithm::SkinTokens)
        && SkinTokensPredictor::isAvailable()
        && !SkinTokensPredictor::modelsPresent();
    m_skinProgress = 0;
    m_skinTotal = 0;
    emit skinProgressChanged();

    m_skinCancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_skinCancel;
    const std::string entName = entity->getName();
    QPointer<SkinWeightsController> self(this);

    std::thread([self, job, opts, algo, cancel, entName]() {
        auto progress = [self, cancel](int done, int total) -> bool {
            if (cancel->load()) return false;
            QMetaObject::invokeMethod(qApp, [self, done, total]() {
                if (!self) return;
                self->m_skinDownloading = false;
                self->m_skinProgress = done;
                self->m_skinTotal = total;
                emit self->skinProgressChanged();
            }, Qt::QueuedConnection);
            return true;
        };
        auto result = std::make_shared<SkinWeights::JobResult>(
            SkinWeights::runJob(*job, opts, algo, progress));

        QMetaObject::invokeMethod(qApp, [self, job, result, opts, cancel,
                                         entName]() {
            if (!self) return;
            self->m_busy = false;
            self->m_skinDownloading = false;
            emit self->busyChanged();
            emit self->skinProgressChanged();
            if (cancel->load()
                || result->error == QLatin1String("cancelled")) {
                emit self->error(QStringLiteral("Skin compute cancelled."));
                return;
            }
            if (!result->ok) {
                emit self->error(result->error.isEmpty()
                    ? QStringLiteral("Skin weights failed")
                    : result->error);
                return;
            }
            // Commit through the undoable command (main thread).
            auto* cmd = new ComputeSkinWeightsCommand(entName, opts, job,
                                                      result);
            UndoManager::getSingleton()->push(cmd);
            const SkinWeightsReport& report = cmd->report();

            QVariantMap map;
            map["applied"]                = report.applied;
            map["meshName"]               = report.meshName;
            map["skeletonName"]           = report.skeletonName;
            map["totalBones"]             = report.totalBones;
            map["totalVerticesProcessed"] = report.totalVerticesProcessed;
            map["totalAssignmentsBefore"] = report.totalAssignmentsBefore;
            map["totalAssignmentsAfter"]  = report.totalAssignmentsAfter;
            map["algorithmUsed"]          = report.algorithmUsed;
            map["fallbackReason"]         = report.fallbackReason;
            if (report.bleedFraction >= 0.0)
                map["bleedFraction"] = report.bleedFraction;
            if (report.applied) {
                GamificationManager::noteOperation(
                    QStringLiteral("skin_weights"),
                    {{QStringLiteral("verts_weighted"),
                      report.totalVerticesProcessed},
                     {QStringLiteral("max_influences"),
                      opts.maxInfluencesPerVertex}});
                emit self->weightsApplied(map);
            } else {
                emit self->error(report.error.isEmpty()
                    ? QStringLiteral("Skin weights failed") : report.error);
            }
        }, Qt::QueuedConnection);
    }).detach();
    return true;
}

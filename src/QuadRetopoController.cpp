#include "QuadRetopoController.h"
#include "GamificationManager.h"
#include "QuadRetopo.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "QuadRemesh/QuadRemeshEntity.h"
#include "QuadRemesh/QuadriFlowRunner.h"
#include "commands/ReplaceEntityMeshCommand.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QThread>

#include <Ogre.h>
#include <OgreEntity.h>

QuadRetopoController* QuadRetopoController::m_pSingleton = nullptr;

QuadRetopoController* QuadRetopoController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new QuadRetopoController();
    return m_pSingleton;
}

QuadRetopoController* QuadRetopoController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void QuadRetopoController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

QuadRetopoController::QuadRetopoController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &QuadRetopoController::selectionChanged);
}

bool QuadRetopoController::hasSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    return !sel->getResolvedEntities().isEmpty();
}

QVariantMap QuadRetopoController::retopologizeSelected(int targetFaces,
                                                       double maxAngleDeg,
                                                       double shapeToleranceDeg,
                                                       double maxAspectRatio)
{
    QVariantMap result;

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

    QuadRetopoOptions opts;
    opts.targetFaces        = targetFaces;
    opts.maxAngleDeg        = maxAngleDeg;
    opts.shapeToleranceDeg  = shapeToleranceDeg;
    opts.maxAspectRatio     = maxAspectRatio;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.retopo"),
        QString("UI retopo entity=%1 target=%2 maxAngle=%3 shape=%4 aspect=%5")
            .arg(QString::fromStdString(entity->getName()))
            .arg(targetFaces)
            .arg(maxAngleDeg).arg(shapeToleranceDeg).arg(maxAspectRatio));

    m_busy = true;
    emit busyChanged();

    QuadRetopoReport report;
    try {
        report = QuadRetopo::retopologize(entity, opts);
    } catch (const Ogre::Exception& e) {
        m_busy = false;
        emit busyChanged();
        emit error(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
        result["applied"] = false;
        result["error"]   = QString::fromStdString(e.getFullDescription());
        return result;
    }

    m_busy = false;
    emit busyChanged();

    result["applied"]              = report.applied;
    result["meshName"]             = report.meshName;
    result["totalTrianglesBefore"] = report.totalTrianglesBefore;
    result["totalFacesAfter"]      = report.totalFacesAfter;
    result["totalQuadsAfter"]      = report.totalQuadsAfter;
    result["totalTrianglesAfter"]  = report.totalTrianglesAfterRetopo;
    result["quadDominance"]        = report.quadDominance();
    if (!report.error.isEmpty()) result["error"] = report.error;

    if (report.applied)
        GamificationManager::noteOperation(
            QStringLiteral("retopo"),
            {{QStringLiteral("tris_before"), report.totalTrianglesBefore},
             {QStringLiteral("tris_after"), report.totalTrianglesAfterRetopo},
             {QStringLiteral("quad_ratio_after"), report.quadDominance()}});

    if (report.applied) emit retopoApplied(result);
    else                emit error(report.error.isEmpty()
                                    ? QStringLiteral("Quad retopology failed")
                                    : report.error);

    return result;
}

// ---- QuadriFlow remesh (async) -------------------------------------------------

bool QuadRetopoController::quadriFlowAvailable() const
{
    return !QuadriFlowRunner::defaultHelperPath().isEmpty();
}

int QuadRetopoController::suggestedQuadriFlowTarget() const
{
    auto* sel = SelectionSet::getSingleton();
    const auto ents = sel ? sel->getResolvedEntities() : QList<Ogre::Entity*>{};
    int tris = 0;
    if (!ents.isEmpty() && ents.first() && ents.first()->getMesh())
        for (unsigned short i = 0; i < ents.first()->getMesh()->getNumSubMeshes(); ++i)
            if (const auto* sm = ents.first()->getMesh()->getSubMesh(i); sm && sm->indexData)
                tris += int(sm->indexData->indexCount / 3);
    return QuadRemesh::defaultTargetFaces(tris);
}

void QuadRetopoController::cancelRemesh()
{
    if (m_cancel) *m_cancel = true;
}

bool QuadRetopoController::remeshSelectedQuadriFlow(const QVariantMap& o)
{
    if (m_busy) return false;
    auto* sel = SelectionSet::getSingleton();
    const auto ents = sel ? sel->getResolvedEntities() : QList<Ogre::Entity*>{};
    if (ents.isEmpty() || !ents.first()) {
        emit error(QStringLiteral("No mesh selected."));
        return false;
    }
    Ogre::Entity* entity = ents.first();

    QuadRemesh::Options opts;
    opts.targetFaces = o.value(QStringLiteral("targetFaces"), suggestedQuadriFlowTarget()).toInt();
    if (opts.targetFaces <= 0) opts.targetFaces = suggestedQuadriFlowTarget();
    opts.preserveSharp = o.value(QStringLiteral("preserveSharp"), true).toBool();
    opts.preserveBoundary = o.value(QStringLiteral("preserveBoundary"), false).toBool();
    opts.adaptiveScale = o.value(QStringLiteral("adaptive"), true).toBool();
    opts.bakeTextures = o.value(QStringLiteral("bakeTextures"), true).toBool();
    opts.bakeDetailNormal = o.value(QStringLiteral("detailNormal"), true).toBool();
    opts.seed = o.value(QStringLiteral("seed"), 0).toInt();
    opts.voxel = QuadRemeshEntity::voxelModeFromString(o.value(QStringLiteral("voxel")).toString());

    // Ogre reads stay on the main thread.
    auto src = std::make_shared<QuadRemesh::SourceMesh>();
    auto info = std::make_shared<QuadRemeshEntity::SourceInfo>();
    QString err;
    if (!QuadRemeshEntity::readSource(entity, *src, *info, &err)) {
        emit error(err);
        return false;
    }
    const std::string name = entity->getName();
    const Ogre::Mesh* sourceMesh = entity->getMesh().get();

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.retopo"),
        QStringLiteral("UI quadriflow entity=%1 target=%2 bake=%3 voxel=%4")
            .arg(QString::fromStdString(name)).arg(opts.targetFaces)
            .arg(opts.bakeTextures).arg(int(opts.voxel)));

    m_busy = true;
    m_progress = 0.0;
    m_progressText = QStringLiteral("Preparing…");
    emit busyChanged();
    emit progressChanged();

    m_cancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_cancel;
    QPointer<QuadRetopoController> self(this);
    opts.progress = [self, cancel](const QString& stage, double f) {
        if (f >= 0 && !stage.isEmpty())
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, stage, f]() {
                if (!self) return;
                self->m_progress = f;
                self->m_progressText = stage;
                emit self->progressChanged();
            }, Qt::QueuedConnection);
        return !cancel->load();
    };

    auto result = std::make_shared<QuadRemesh::Result>();
    QThread* worker = QThread::create([src, opts, result]() { *result = QuadRemesh::remesh(*src, opts); });
    connect(worker, &QThread::finished, this, [this, worker, result, info, name, sourceMesh]() {
        worker->deleteLater();
        auto finish = [this]() {
            m_busy = false;
            m_cancel.reset();
            emit busyChanged();
        };
        if (!result->ok) {
            finish();
            emit error(result->cancelled ? QStringLiteral("Cancelled.")
                                         : QStringLiteral("Quad remesh failed: %1").arg(result->error));
            return;
        }
        // The entity must still be the one we read (not deleted/replaced
        // while the worker ran).
        Ogre::Entity* live = nullptr;
        if (auto* sel = SelectionSet::getSingleton())
            for (Ogre::Entity* e : sel->getResolvedEntities())
                if (e && e->getName() == name) live = e;
        if (!live) {
            // Not selected any more — still apply if it exists by name.
            for (Ogre::Entity* e : Manager::getSingleton()->getEntities())
                if (e && e->getMovableType() == "Entity" && e->getName() == name) live = e;
        }
        if (!live || live->getMesh().get() != sourceMesh) {
            finish();
            emit error(QStringLiteral("The mesh changed while it was being remeshed — nothing applied."));
            return;
        }
        const QuadRemeshEntity::Built built = QuadRemeshEntity::buildMesh(*result, *info, name);
        if (!built.ok) {
            finish();
            emit error(QStringLiteral("Quad remesh failed: %1").arg(built.error));
            return;
        }
        auto* cmd = new ReplaceEntityMeshCommand(name, built.mesh, QStringLiteral("Quad Remesh (QuadriFlow)"));
        UndoManager::getSingleton()->push(cmd);
        finish();
        if (!cmd->ok()) {
            emit error(QStringLiteral("Could not apply the remeshed mesh."));
            return;
        }
        QVariantMap report = QuadRemeshEntity::reportToJson(*result).toVariantMap();
        report[QStringLiteral("textures")] = built.texturePaths;
        report[QStringLiteral("applied")] = true;
        GamificationManager::noteOperation(
            QStringLiteral("retopo"),
            {{QStringLiteral("tris_before"), result->inputTriangles},
             {QStringLiteral("tris_after"), int(result->indices.size() / 3)},
             {QStringLiteral("quad_ratio_after"),
              result->quads + result->triangles > 0
                  ? double(result->quads) / (result->quads + result->triangles) : 0.0}});
        emit remeshFinished(report);
    });
    worker->start();
    return true;
}

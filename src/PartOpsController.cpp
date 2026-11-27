#include "PartOpsController.h"

#include "SelectionSet.h"
#include "UndoManager.h"
#include "SentryReporter.h"
#include "commands/SplitMeshCommand.h"
#include "commands/ExplodePartsCommand.h"
#include "commands/JoinPartsCommand.h"
#include "AutoRig.h"
#include "Manager.h"
#include "MeshSegmenter.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>

#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSceneManager.h>

#include <thread>

PartOpsController* PartOpsController::m_pSingleton = nullptr;

PartOpsController* PartOpsController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new PartOpsController();
    return m_pSingleton;
}

PartOpsController* PartOpsController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void PartOpsController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

PartOpsController::PartOpsController() : QObject(nullptr)
{
    if (auto* sel = SelectionSet::getSingleton())
        connect(sel, &SelectionSet::selectionChanged, this, &PartOpsController::selectionChanged);
}

bool PartOpsController::hasSelection() const
{
    const auto* sel = SelectionSet::getSingleton();
    return sel && sel->getResolvedEntities().size() == 1;
}

bool PartOpsController::canExplode() const
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel)
        return false;
    const QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    if (entities.size() != 1 || !entities.first() || !entities.first()->getMesh())
        return false;
    // Nothing to explode from a single-submesh mesh.
    return entities.first()->getMesh()->getNumSubMeshes() >= 2;
}

bool PartOpsController::canJoin() const
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel)
        return false;
    // Count only real mesh entities — getResolvedEntities() can contain nulls
    // (mixed selections with non-mesh nodes), and canExplode() already guards
    // that. Join needs >= 2 ACTUAL parts, else the button would enable on a
    // 1-mesh+1-null selection and the command would be built with one name.
    int meshCount = 0;
    for (Ogre::Entity* e : sel->getResolvedEntities())
        if (e && e->getMesh())
            ++meshCount;
    return meshCount >= 2;
}

void PartOpsController::splitSelectedIntoParts(const QString& upAxis, const QString& category,
                                               bool noModel, bool solidify,
                                               const QString& granularity)  // NOLINT
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        emit splitFinished(tr("No selection."), true);
        return;
    }
    const QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    if (entities.size() != 1 || !entities.first()) {
        emit splitFinished(tr("Select a single mesh to split."), true);
        return;
    }

    int axis = 1;
    const QString a = upAxis.trimmed().toLower();
    if (a == QStringLiteral("x")) axis = 0;
    else if (a == QStringLiteral("z")) axis = 2;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("split_into_parts%1")
                                      .arg(noModel ? QStringLiteral(" offline") : QString()));
    const std::string entName = entities.first()->getName();
    if (MeshSegmenter::categoryFromName(category) == MeshSegmenter::Category::Generic) {
        if (noModel) {
            emit splitFinished(tr("Generic segmentation needs the SAM 2.1 model; it has no offline mode."), true);
            return;
        }
        splitGeneric(entName, axis, solidify, granularity);
        return;
    }
    // push() runs redo() synchronously (AutoRigController pattern); read the
    // result back. A failed split leaves a harmless no-op on the undo stack.
    auto* cmd = new SplitMeshCommand(entName, axis, category, noModel,
                                     QStringLiteral("Body"), solidify);
    UndoManager::getSingleton()->push(cmd);

    if (!cmd->ok()) {
        emit splitFinished(cmd->error().isEmpty() ? tr("Split failed.") : cmd->error(), true);
        return;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.split_segments"),
                                  QStringLiteral("gui parts=%1").arg(cmd->createdSubMeshes()));
    emit splitFinished(tr("Split into %1 part submeshes.").arg(cmd->createdSubMeshes()), false);
}

void PartOpsController::setSplitState(bool busy, bool downloading, int progress, int total)
{
    m_splitBusy = busy;
    m_splitDownloading = downloading;
    m_splitProgress = progress;
    m_splitTotal = total;
    emit splitProgressChanged();
}

void PartOpsController::cancelSplit()
{
    if (m_splitCancel) m_splitCancel->store(true);
}

namespace {
Ogre::Entity* findEntity(const std::string& name)
{
    auto* mgr = Manager::getSingletonPtr();
    auto* sm = mgr ? mgr->getSceneMgr() : nullptr;
    return (sm && sm->hasEntity(name)) ? sm->getEntity(name) : nullptr;
}
} // namespace

void PartOpsController::splitGeneric(const std::string& entityName, int axis, bool solidify,
                                     const QString& granularity)
{
    if (m_splitBusy) {
        emit splitFinished(tr("A split is already running."), true);
        return;
    }
    Ogre::Entity* entity = findEntity(entityName);
    if (!entity) {
        emit splitFinished(tr("Select a single mesh to split."), true);
        return;
    }
    // MAIN thread: geometry (locks Ogre buffers) and the model download
    // (ModelDownloader lives here; its nested loop keeps the UI painting).
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    if (!AutoRig::gatherGeometry(entity, verts, indices) || verts.empty()) {
        emit splitFinished(tr("No readable geometry to segment."), true);
        return;
    }
    // Snapshot BEFORE the download: its nested event loop lets the user delete
    // or replace the entity, so `entity` must not be dereferenced after it.
    const Ogre::Mesh* meshAtStart = entity->getMesh().get();
    entity = nullptr;
    // The cancel token exists before the download so Cancel (shown as soon as
    // the split is busy) also stops a first-use download.
    m_splitCancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_splitCancel;
    const MeshSegmenter::Category cat = MeshSegmenter::Category::Generic;
    setSplitState(true, !MeshSegmenter::modelPresent(cat), 0, 0);
    const QString modelPath = MeshSegmenter::ensureModelBlocking(cat, [cancel]() { return cancel->load(); });
    if (cancel->load()) {
        setSplitState(false, false, 0, 0);
        emit splitFinished(tr("Split cancelled."), false);
        return;
    }
    if (modelPath.isEmpty()) {
        setSplitState(false, false, 0, 0);
        emit splitFinished(tr("Generic segmentation is unavailable: %1")
                               .arg(MeshSegmenter::genericModelError()), true);
        return;
    }
    Ogre::Entity* still = findEntity(entityName);
    if (!still || still->getMesh().get() != meshAtStart) {
        setSplitState(false, false, 0, 0);
        emit splitFinished(tr("The mesh changed while the model was downloading; split again."), true);
        return;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.segment"),
        QStringLiteral("gui split generic granularity=%1").arg(granularity));
    setSplitState(true, false, 0, 0);
    QPointer<PartOpsController> self(this);
    // Labels are per triangle in THIS order; the completion step checks the
    // live index buffer still matches (an in-place reorder such as the vertex
    // cache optimisation keeps the mesh and the triangle count).
    auto inputIndices = std::make_shared<const std::vector<uint32_t>>(indices);

    std::thread([self, verts = std::move(verts), indices = std::move(indices), modelPath, cancel,
                 granularity, axis, solidify, entityName, meshAtStart, inputIndices]() {
        QElapsedTimer clock;
        clock.start();
        MeshSegmenter::Options opts;
        opts.upAxis = axis;
        opts.category = MeshSegmenter::Category::Generic;
        if (!granularity.isEmpty()) opts.granularity = granularity;
        auto progress = [self, cancel](int done, int total) -> bool {
            if (cancel->load()) return false;
            QMetaObject::invokeMethod(qApp, [self, done, total]() {
                if (self) self->setSplitState(true, false, done, total);
            }, Qt::QueuedConnection);
            return true;
        };
        const MeshSegmenter::Result r = MeshSegmenter::predict(
            verts.data(), int(verts.size() / 3), indices.data(), int(indices.size()),
            modelPath, opts, nullptr, progress);
        const qint64 ms = clock.elapsed();
        QMetaObject::invokeMethod(qApp, [self, r, cancel, solidify, axis, entityName, meshAtStart,
                                         granularity, ms, inputIndices]() {
            if (!self) return;
            self->setSplitState(false, false, 0, 0);
            if (cancel->load() || r.fallbackReason == QLatin1String("cancelled")) {
                emit self->splitFinished(tr("Split cancelled."), false);
                return;
            }
            if (!r.ok) {
                emit self->splitFinished(tr("Segmentation failed: %1").arg(r.error), true);
                return;
            }
            // The mesh may have changed (or gone) during the minute it took.
            Ogre::Entity* e = findEntity(entityName);
            std::vector<float> liveVerts;
            std::vector<uint32_t> liveIndices;
            if (!e || e->getMesh().get() != meshAtStart || r.faceLabels.size() != inputIndices->size() / 3
                || !AutoRig::gatherGeometry(e, liveVerts, liveIndices) || liveIndices != *inputIndices) {
                emit self->splitFinished(tr("The mesh changed while it was being segmented; split again."), true);
                return;
            }
            auto* cmd = new SplitMeshCommand(entityName, axis, QStringLiteral("generic"), false,
                                             QStringLiteral("Body"), solidify);
            cmd->setPrecomputedLabels(r.faceLabels, r.labelNames);
            UndoManager::getSingleton()->push(cmd);
            if (!cmd->ok()) {
                emit self->splitFinished(cmd->error().isEmpty() ? tr("Split failed.") : cmd->error(), true);
                return;
            }
            SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.split_segments"),
                QStringLiteral("gui generic parts=%1 granularity=%2 ms=%3")
                    .arg(cmd->createdSubMeshes()).arg(granularity).arg(ms));
            emit self->splitFinished(tr("Split into %1 part submeshes.").arg(cmd->createdSubMeshes()), false);
        }, Qt::QueuedConnection);
    }).detach();
}

void PartOpsController::explodeSelected(double distance)
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        emit explodeFinished(tr("No selection."), true);
        return;
    }
    const QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    if (entities.size() != 1 || !entities.first() || !entities.first()->getMesh()) {
        emit explodeFinished(tr("Select a single mesh to explode."), true);
        return;
    }
    if (entities.first()->getMesh()->getNumSubMeshes() < 2) {
        emit explodeFinished(tr("Mesh has a single part — split it first."), true);
        return;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("explode_parts"));
    const std::string entName = entities.first()->getName();
    auto* cmd = new ExplodePartsCommand(entName, static_cast<float>(distance));
    UndoManager::getSingleton()->push(cmd);

    if (!cmd->ok()) {
        emit explodeFinished(cmd->error().isEmpty() ? tr("Explode failed.") : cmd->error(), true);
        return;
    }
    emit explodeFinished(tr("Exploded into %1 part nodes.").arg(cmd->createdParts()), false);
}

void PartOpsController::joinSelected()
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        emit joinFinished(tr("No selection."), true);
        return;
    }
    // Collect only real mesh entities — getResolvedEntities() can contain nulls
    // (non-mesh nodes in a mixed selection), so gating on the raw size would let
    // a 1-mesh+1-null selection through with a single real part. Build the name
    // list first, then require >= 2 ACTUAL parts.
    std::vector<std::string> names;
    QString fusedBase;
    for (Ogre::Entity* e : sel->getResolvedEntities()) {
        if (!e || !e->getMesh())
            continue;
        names.push_back(e->getName());
        // The fused node is named after the first selected part with a "_fused"
        // suffix (Manager uniquifies), so it's obviously the join result.
        if (fusedBase.isEmpty())
            fusedBase = QString::fromStdString(e->getName()) + QStringLiteral("_fused");
    }
    if (names.size() < 2) {
        emit joinFinished(tr("Select two or more parts to join."), true);
        return;
    }
    const int partCount = static_cast<int>(names.size());

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("join_parts"));
    auto* cmd = new JoinPartsCommand(std::move(names), fusedBase);
    UndoManager::getSingleton()->push(cmd);

    if (!cmd->ok()) {
        emit joinFinished(cmd->error().isEmpty() ? tr("Join failed.") : cmd->error(), true);
        return;
    }
    emit joinFinished(tr("Joined %1 parts into one mesh (%2 submeshes).")
                          .arg(partCount).arg(cmd->createdSubMeshes()), false);
}

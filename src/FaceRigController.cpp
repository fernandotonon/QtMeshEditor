#include "FaceRigController.h"

#include "FaceRig/ArkitTemplate.h"
#include "FaceRig/FaceRigAttach.h"
#include "GamificationManager.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/MorphCommands.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>

#include <QCoreApplication>
#include <QPointer>

#include <cstdint>
#include <memory>
#include <thread>

FaceRigController* FaceRigController::m_pSingleton = nullptr;

FaceRigController* FaceRigController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new FaceRigController();
    return m_pSingleton;
}

FaceRigController* FaceRigController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void FaceRigController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

FaceRigController::FaceRigController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &FaceRigController::selectionChanged);
}

void FaceRigController::setStatus(const QString& s)
{
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

bool FaceRigController::hasMeshSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty()) return false;
    Ogre::Entity* first = entities.first();
    return first && first->getMesh();
}

bool FaceRigController::addArkitBlendshapesAsync(int maxShapes, double maxResidualPct)
{
    if (m_busy) {
        emit error(QStringLiteral("A face-rig is already running."));
        return false;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Add ARKit Blendshapes requested"));

    auto* sel = SelectionSet::getSingleton();
    const auto entities = sel ? sel->getResolvedEntities()
                              : QList<Ogre::Entity*>{};
    Ogre::Entity* entity = entities.isEmpty() ? nullptr : entities.first();
    if (!entity || !entity->getMesh()) {
        emit error(QStringLiteral("No mesh selected."));
        return false;
    }

    // MAIN thread: read the entity geometry (locks Ogre buffers — ms).
    auto geo = std::make_shared<FaceRig::FaceRigGeometry>(
        FaceRig::extractGeometry(entity));
    if (!geo->valid()) {
        emit error(QStringLiteral("Could not read the mesh geometry."));
        return false;
    }

    // MAIN thread: ensure + load the bundled template (may download on first
    // use — surface that to the UI). ensureModelBlocking() can take a while on
    // a first-run download, so show "Downloading…" first.
    m_downloading = !FaceRig::ArkitTemplate::present();
    setStatus(m_downloading ? QStringLiteral("Downloading face template…")
                            : QStringLiteral("Preparing…"));
    const QString path = FaceRig::ArkitTemplate::ensureModelBlocking();
    m_downloading = false;
    if (path.isEmpty()) {
        setStatus(QString());
        emit error(QStringLiteral(
            "ARKit template unavailable (offline and not yet downloaded, or "
            "this build has no face-rig model)."));
        return false;
    }
    auto tmpl = std::make_shared<FaceRig::ArkitTemplate>();
    QString loadErr;
    if (!tmpl->load(path, &loadErr)) {
        setStatus(QString());
        emit error(QStringLiteral("Failed to load ARKit template: %1").arg(loadErr));
        return false;
    }

    FaceRig::FaceRigOptions opts;
    opts.maxShapes = maxShapes;
    opts.maxFitResidualPct = maxResidualPct;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.face_rig"),
        QStringLiteral("face_rig entity=%1 verts=%2 template=%3v")
            .arg(QString::fromStdString(entity->getName()))
            .arg(geo->userV.size() / 3).arg(tmpl->vertexCount()));

    m_busy = true;
    emit busyChanged();
    m_progress = 0;
    m_progressTotal = 0;
    emit progressChanged();
    setStatus(QStringLiteral("Fitting face template…"));

    m_cancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_cancel;
    const std::string entName = entity->getName();
    QPointer<FaceRigController> self(this);

    // WORKER thread: the heavy Ogre-free fit + transfer over all 52 shapes.
    std::thread([self, geo, tmpl, opts, entName, cancel]() {
        // Progress callback: marshal the counters to the main thread for the
        // progress bar; return false to stop the worker when cancel is set.
        auto progress = [self, cancel](int done, int total,
                                       const char* phase) -> bool {
            if (cancel->load()) return false;
            const QString ph = QString::fromUtf8(phase);
            QMetaObject::invokeMethod(qApp, [self, done, total, ph]() {
                if (!self) return;
                self->m_progress = done;
                self->m_progressTotal = total;
                emit self->progressChanged();
                self->setStatus(ph);
            }, Qt::QueuedConnection);
            return true;
        };
        auto result = std::make_shared<FaceRig::FaceRigResult>(
            FaceRig::buildFaceRig(geo->userV, geo->userF, *tmpl, opts,
                                  geo->headMask, progress));

        // MAIN thread: attach (touches Ogre + the undo stack).
        QMetaObject::invokeMethod(qApp, [self, geo, result, entName]() {
            if (!self) return;
            self->m_busy = false;
            emit self->busyChanged();
            self->m_progress = 0;
            self->m_progressTotal = 0;
            emit self->progressChanged();
            self->setStatus(QString());

            if (!result->ok) {
                emit self->error(result->error == "cancelled"
                    ? QStringLiteral("Face-rig cancelled.")
                    : QString::fromStdString(result->error));
                return;
            }

            // Resolve the entity again by name — the selection may have changed
            // while the worker ran; only attach if it's still around.
            Ogre::Entity* entity = nullptr;
            for (Ogre::Entity* e : Manager::getSingleton()->getEntities()) {
                if (e && e->getMovableType() == "Entity"
                    && e->getName() == entName) { entity = e; break; }
            }
            if (!entity) {
                emit self->error(QStringLiteral(
                    "The mesh went away before the face-rig could be applied."));
                return;
            }

            // Undoable group: one macro so Ctrl+Z removes all shapes at once.
            FaceRig::AttachReport rep;
            rep.userVertexCount = result->userVertexCount;
            rep.fitMeanResidualPct = result->fitMeanResidualPct;
            rep.fitMaxResidualPct = result->fitMaxResidualPct;

            // The attach adds poses + a VAT_POSE clip to a LIVE entity and
            // re-initialises it per shape. If the entity is mid-skeletal-
            // animation, the render loop's _updateAnimation can run against the
            // half-rebuilt pose buffers between our steps and crash. Disable
            // every enabled animation state for the batch, then restore them —
            // so the frame loop leaves the entity static while we mutate it.
            std::vector<QString> reEnable;
            if (auto* ass = entity->getAllAnimationStates()) {
                for (const auto& kv : ass->getAnimationStates()) {
                    if (kv.second && kv.second->getEnabled()) {
                        reEnable.push_back(QString::fromStdString(kv.first));
                        kv.second->setEnabled(false);
                    }
                }
            }

            // Build the per-shape commands first so we know which is LAST — only
            // the last re-initialises the entity (deferInit=false on it), the
            // rest defer. This makes redo (Ctrl+Shift+Z) rebuild the pose buffers
            // exactly once at the end too, not just the initial attach; a
            // per-shape re-init would freeze the UI on a multi-submesh mesh.
            std::vector<AddMorphTargetCommand*> cmds;
            for (const FaceRig::FaceRigShape& shape : result->shapes) {
                std::vector<MorphPoseSlice> slices;
                for (const FaceRig::GeometryOwner& o : geo->owners) {
                    MorphPoseSlice slice;
                    slice.submeshHandle = o.handle;
                    for (int i = 0; i < o.count; ++i) {
                        const std::uint32_t gv = o.base + std::uint32_t(i);
                        if (size_t(gv) * 3 + 2 >= shape.userDeltas.size()) break;
                        const float* d = &shape.userDeltas[size_t(gv) * 3];
                        if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f) continue;
                        slice.offsets[static_cast<unsigned int>(i)] =
                            Ogre::Vector3f(d[0], d[1], d[2]);
                    }
                    if (!slice.offsets.empty()) slices.push_back(std::move(slice));
                }
                if (slices.empty()) continue;
                cmds.push_back(new AddMorphTargetCommand(entity, shape.name, slices));
            }
            for (size_t ci = 0; ci + 1 < cmds.size(); ++ci)
                cmds[ci]->setDeferInit(true);   // all but the last defer re-init

            auto* undo = UndoManager::getSingleton();
            auto* stack = undo ? undo->stack() : nullptr;
            if (stack) stack->beginMacro(QStringLiteral("Add ARKit Blendshapes"));
            for (auto* cmd : cmds) { undo->push(cmd); rep.shapesAttached++; }
            if (stack) stack->endMacro();

            // Restore the animation states we disabled (refreshAvailable... in
            // the attach may have recreated the state set, so re-resolve).
            if (auto* ass = entity->getAllAnimationStates()) {
                for (const QString& n : reEnable) {
                    const std::string sn = n.toStdString();
                    if (ass->hasAnimationState(sn))
                        ass->getAnimationState(sn)->setEnabled(true);
                }
            }
            rep.ok = rep.shapesAttached > 0;

            if (!rep.ok) {
                emit self->error(QStringLiteral(
                    "No blendshapes produced any vertex movement."));
                return;
            }

            GamificationManager::noteOperation(
                QStringLiteral("morph"),
                {{QStringLiteral("blendshapes_attached"), rep.shapesAttached}},
                GamificationManager::Surface::Gui);

            QVariantMap map;
            map["shapesAttached"] = rep.shapesAttached;
            map["userVertexCount"] = rep.userVertexCount;
            map["fitMeanResidualPct"] = rep.fitMeanResidualPct;
            map["fitMaxResidualPct"] = rep.fitMaxResidualPct;
            emit self->completed(map);
        }, Qt::QueuedConnection);
    }).detach();

    return true;
}

void FaceRigController::cancel()
{
    if (m_cancel) m_cancel->store(true);
}

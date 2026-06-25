#include "AutoRigController.h"
#include "AutoRig.h"
#include "UniRigPredictor.h"
#include "SkinWeights.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "Manager.h"
#include "OgreWidget.h"
#include "SpaceCamera.h"
#include "UndoManager.h"
#include "PropertiesPanelController.h"
#include "commands/AutoRigCommand.h"

#include <QCoreApplication>
#include <QPointer>
#include <QMetaObject>
#include <thread>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreSceneManager.h>
#include <OgreMeshManager.h>
#include <OgreMaterialManager.h>
#include <OgreHardwareBufferManager.h>

#include <limits>

namespace {

// Read an entity's mesh into tightly-packed world-space triangle vertices.
// (Self-contained — does not depend on Edit Mode's EditableMesh.) Used for the
// marker ray-cast. Returns false if no readable geometry.
bool gatherWorldTriangles(Ogre::Entity* entity, std::vector<Ogre::Vector3>& outTris)
{
    if (!entity || !entity->getMesh()) return false;
    Ogre::MeshPtr mesh = entity->getMesh();
    Ogre::Node* node = entity->getParentSceneNode();
    const Ogre::Affine3 xform = node ? node->_getFullTransform() : Ogre::Affine3::IDENTITY;

    auto readVB = [](Ogre::VertexData* vd, std::vector<Ogre::Vector3>& pos) {
        if (!vd) return;
        const auto* pe = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!pe) return;
        auto vb = vd->vertexBufferBinding->getBuffer(pe->getSource());
        if (!vb) return;
        const size_t stride = vb->getVertexSize();
        auto* base = static_cast<unsigned char*>(vb->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        if (!base) return;
        const size_t start = pos.size();
        pos.resize(start + vd->vertexCount);
        for (size_t i = 0; i < vd->vertexCount; ++i) {
            float* p = nullptr;
            pe->baseVertexPointerToElement(base + i * stride, &p);
            pos[start + i] = Ogre::Vector3(p[0], p[1], p[2]);
        }
        vb->unlock();
    };

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;

        std::vector<Ogre::Vector3> pos;   // local-space vertex positions for this submesh
        Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
        readVB(vd, pos);
        if (pos.empty()) continue;

        Ogre::IndexData* id = sub->indexData;
        if (!id || !id->indexBuffer || id->indexCount < 3) continue;
        auto ib = id->indexBuffer;
        const bool is32 = ib->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
        auto* idxBase = static_cast<unsigned char*>(ib->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        if (!idxBase) continue;
        const auto* i32 = reinterpret_cast<const uint32_t*>(idxBase);
        const auto* i16 = reinterpret_cast<const uint16_t*>(idxBase);
        for (size_t t = 0; t + 2 < id->indexCount; t += 3) {
            const uint32_t a = is32 ? i32[t]   : i16[t];
            const uint32_t b = is32 ? i32[t+1] : i16[t+1];
            const uint32_t c = is32 ? i32[t+2] : i16[t+2];
            if (a >= pos.size() || b >= pos.size() || c >= pos.size()) continue;
            outTris.push_back(xform * pos[a]);
            outTris.push_back(xform * pos[b]);
            outTris.push_back(xform * pos[c]);
        }
        ib->unlock();
    }
    return !outTris.empty();
}

// Möller-Trumbore; returns t>0 on hit else -1.
float rayTri(const Ogre::Vector3& o, const Ogre::Vector3& d,
             const Ogre::Vector3& v0, const Ogre::Vector3& v1, const Ogre::Vector3& v2)
{
    const Ogre::Vector3 e1 = v1 - v0, e2 = v2 - v0;
    const Ogre::Vector3 p = d.crossProduct(e2);
    const float det = e1.dotProduct(p);
    if (std::abs(det) < 1e-8f) return -1.0f;
    const float inv = 1.0f / det;
    const Ogre::Vector3 tv = o - v0;
    const float u = tv.dotProduct(p) * inv;
    if (u < 0 || u > 1) return -1.0f;
    const Ogre::Vector3 q = tv.crossProduct(e1);
    const float v = d.dotProduct(q) * inv;
    if (v < 0 || u + v > 1) return -1.0f;
    const float t = e2.dotProduct(q) * inv;
    return t > 1e-6f ? t : -1.0f;
}

} // namespace

AutoRigController* AutoRigController::m_pSingleton = nullptr;

AutoRigController* AutoRigController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new AutoRigController();
    return m_pSingleton;
}

AutoRigController* AutoRigController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AutoRigController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

AutoRigController::AutoRigController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &AutoRigController::selectionChanged);
}

bool AutoRigController::hasRiggableSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty()) return false;
    Ogre::Entity* first = entities.first();
    if (!first || !first->getMesh()) return false;
    // Riggable == static (no skeleton yet). An already-skinned mesh is
    // intentionally excluded (re-rigging would wipe its existing rig).
    return first->getMesh()->getSkeleton() == nullptr;
}

QVariantMap AutoRigController::autoRigSelected(const QString& templateName,
                                               const QString& upAxis,
                                               bool alsoSkin,
                                               const QString& algo)
{
    QVariantMap result;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Auto-rig requested (%1, algo=%2, up=%3%4)")
            .arg(templateName, algo, upAxis,
                 alsoSkin ? QStringLiteral(", +skin") : QString()));

    auto* sel = SelectionSet::getSingleton();
    const auto entities = sel ? sel->getResolvedEntities() : QList<Ogre::Entity*>{};
    if (entities.isEmpty()) {
        const auto msg = QStringLiteral("No mesh selected.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }
    Ogre::Entity* entity = entities.first();
    if (!entity || !entity->getMesh()) {
        const auto msg = QStringLiteral("Selected entity is no longer valid.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }

    AutoRig::Options opts;
    opts.tmpl = AutoRig::templateFromString(templateName);
    opts.algorithm = AutoRig::algorithmFromString(algo);
    const QString ax = upAxis.trimmed().toLower();
    if (ax == QStringLiteral("x")) opts.upAxis = 0;
    else if (ax == QStringLiteral("z")) opts.upAxis = 2;
    else opts.upAxis = 1;   // y (default)

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.auto_rig"),
        QStringLiteral("UI auto-rig entity=%1 template=%2 algo=%3")
            .arg(QString::fromStdString(entity->getName()),
                 AutoRig::templateToString(opts.tmpl),
                 AutoRig::algorithmToString(opts.algorithm)));

    // Pre-check here so a non-static mesh fails cleanly WITHOUT leaving a
    // no-op entry on the undo stack (QUndoStack::push runs redo()).
    if (entity->getMesh()->hasSkeleton()) {
        const auto msg = QStringLiteral(
            "Mesh already has a skeleton — auto-rig only applies to unrigged "
            "(static) meshes.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }

    m_busy = true;
    emit busyChanged();

    // --- UniRig: run the slow ONNX inference on a WORKER thread so the UI
    // stays responsive + shows a progress bar; build the Ogre skeleton back on
    // the MAIN thread. Pinocchio is instant, so it keeps the synchronous path.
    if (opts.algorithm == AutoRig::Algorithm::UniRig) {
        // Gather geometry now (main thread — locks Ogre HW buffers).
        std::vector<float> verts;
        std::vector<uint32_t> indices;
        AutoRig::gatherGeometry(entity, verts, indices);

        m_rigDownloading = !UniRigPredictor::modelsPresent();
        m_rigProgress = 0; m_rigTotal = 0;
        emit rigProgressChanged();

        m_rigCancel = std::make_shared<std::atomic_bool>(false);
        auto cancel = m_rigCancel;
        const std::string entName = entity->getName();
        const int upAxisVal = opts.upAxis;
        QPointer<AutoRigController> self(this);

        std::thread([self, verts, indices, upAxisVal, templateName, alsoSkin, cancel, entName]() {
            auto progress = [self, cancel](int done, int total) -> bool {
                if (cancel->load()) return false;
                QMetaObject::invokeMethod(qApp, [self, done, total]() {
                    if (!self) return;
                    self->m_rigDownloading = false;
                    self->m_rigProgress = done; self->m_rigTotal = total;
                    emit self->rigProgressChanged();
                }, Qt::QueuedConnection);
                return true;
            };
            QString err;
            std::vector<AutoRig::Joint> joints =
                AutoRig::predictUniRig(verts, indices, upAxisVal, progress, &err);
            // Back to the main thread: build the skeleton (Ogre) or report.
            QMetaObject::invokeMethod(qApp, [self, joints, err, entName,
                                             templateName, upAxisVal, alsoSkin, cancel]() {
                if (!self) return;
                self->m_rigDownloading = false;
                if (cancel->load()) {
                    self->m_busy = false; emit self->busyChanged();
                    emit self->error(QStringLiteral("Auto-rig cancelled."));
                    return;
                }
                if (joints.size() < 2) {
                    // Inference unavailable/failed → fall back to the template
                    // rig on the main thread (synchronous, instant).
                    self->finishUniRigFallback(QString::fromStdString(entName),
                                               err, templateName, upAxisVal, alsoSkin);
                    return;
                }
                self->finishUniRigOnMain(QString::fromStdString(entName), joints,
                                         templateName, upAxisVal, alsoSkin);
            }, Qt::QueuedConnection);
        }).detach();

        result["applied"] = true;       // async — real result arrives via rigged()
        result["pending"] = true;
        return result;
    }

    AutoRig::Report report;
    bool skinned = false;
    try {
        // Run through an undo command so rig (+ optional skin) reverts with
        // Ctrl+Z. push() executes redo() synchronously; read back the report.
        auto* cmd = new AutoRigCommand(entity->getName(), opts, {}, alsoSkin);
        UndoManager::getSingleton()->push(cmd);
        report  = cmd->report();
        skinned = cmd->skinned();
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
    emit selectionChanged();   // skeleton state changed → refresh button bindings

    result["applied"]          = report.applied;
    result["meshName"]         = report.meshName;
    result["skeletonName"]     = report.skeletonName;
    result["template"]         = report.templateName;
    result["algorithm"]        = AutoRig::algorithmToString(report.algorithmUsed);
    result["boneCount"]        = report.boneCount;
    result["verticesSampled"]  = report.verticesSampled;
    result["jointsRecentered"] = report.jointsRecentered;
    result["skinned"]          = skinned;
    if (!report.fallbackReason.isEmpty()) result["fallbackReason"] = report.fallbackReason;
    if (!report.error.isEmpty()) result["error"] = report.error;

    if (report.applied) emit rigged(result);
    else emit error(report.error.isEmpty()
                        ? QStringLiteral("Auto-rig failed") : report.error);

    return result;
}

void AutoRigController::cancelRig()
{
    if (m_rigCancel) m_rigCancel->store(true);
}

void AutoRigController::emitRigResult(const AutoRig::Report& report, bool skinned)
{
    m_busy = false; m_rigDownloading = false;
    emit busyChanged();
    emit rigProgressChanged();
    emit selectionChanged();

    QVariantMap result;
    result["applied"]          = report.applied;
    result["meshName"]         = report.meshName;
    result["skeletonName"]     = report.skeletonName;
    result["template"]         = report.templateName;
    result["algorithm"]        = AutoRig::algorithmToString(report.algorithmUsed);
    result["boneCount"]        = report.boneCount;
    result["verticesSampled"]  = report.verticesSampled;
    result["jointsRecentered"] = report.jointsRecentered;
    result["skinned"]          = skinned;
    if (!report.fallbackReason.isEmpty()) result["fallbackReason"] = report.fallbackReason;
    if (!report.error.isEmpty()) result["error"] = report.error;

    if (report.applied) emit rigged(result);
    else emit error(report.error.isEmpty()
                        ? QStringLiteral("Auto-rig failed") : report.error);
}

void AutoRigController::finishUniRigOnMain(const QString& entityName,
                                           const std::vector<AutoRig::Joint>& joints,
                                           const QString& templateName, int upAxis,
                                           bool alsoSkin)
{
    // MAIN thread: build the Ogre skeleton from worker-predicted joints via the
    // undoable command (using the prePredictedJoints escape hatch so it skips
    // the slow ONNX path and just builds + binds).
    AutoRig::Options opts;
    opts.algorithm = AutoRig::Algorithm::UniRig;
    opts.tmpl = AutoRig::templateFromString(templateName);
    opts.upAxis = upAxis;
    opts.prePredictedJoints = joints;

    AutoRig::Report report; bool skinned = false;
    try {
        auto* cmd = new AutoRigCommand(entityName.toStdString(), opts, {}, alsoSkin);
        UndoManager::getSingleton()->push(cmd);
        report  = cmd->report();
        skinned = cmd->skinned();
    } catch (const std::exception& e) {
        report.applied = false;
        report.error = QString::fromUtf8(e.what());
    }
    emitRigResult(report, skinned);
}

void AutoRigController::finishUniRigFallback(const QString& entityName, const QString& reason,
                                             const QString& templateName, int upAxis, bool alsoSkin)
{
    // MAIN thread: UniRig wasn't usable → template rig, with the reason noted.
    AutoRig::Options opts;
    opts.algorithm = AutoRig::Algorithm::Pinocchio;
    opts.tmpl = AutoRig::templateFromString(templateName);
    opts.upAxis = upAxis;

    AutoRig::Report report; bool skinned = false;
    try {
        auto* cmd = new AutoRigCommand(entityName.toStdString(), opts, {}, alsoSkin);
        UndoManager::getSingleton()->push(cmd);
        report  = cmd->report();
        skinned = cmd->skinned();
    } catch (const std::exception& e) {
        report.applied = false;
        report.error = QString::fromUtf8(e.what());
    }
    if (report.applied && report.fallbackReason.isEmpty())
        report.fallbackReason =
            QStringLiteral("%1 — used the native template rig instead.")
                .arg(reason.isEmpty() ? QStringLiteral("UniRig unavailable") : reason);
    emitRigResult(report, skinned);
}

// ============================ Marker placement ============================

Ogre::Entity* AutoRigController::selectedRiggableEntity() const
{
    auto* sel = SelectionSet::getSingleton();
    const auto ents = sel ? sel->getResolvedEntities() : QList<Ogre::Entity*>{};
    if (ents.isEmpty()) return nullptr;
    Ogre::Entity* e = ents.first();
    if (!e || !e->getMesh() || e->getMesh()->getSkeleton() != nullptr) return nullptr;
    return e;
}

int AutoRigController::markerCount() const
{
    // Slots resolved so far (placed OR skipped) = the cursor position. Drives
    // the "N/total" progress readout in the UI.
    return m_markerCursor;
}

int AutoRigController::markerTotal() const
{
    return static_cast<int>(m_markerOrder.size());
}

int AutoRigController::markerPlacedCount() const
{
    // Only the actually-placed (set) markers — what "Rig from markers" needs.
    return static_cast<int>(m_markers.size());
}

QString AutoRigController::currentMarkerLabel() const
{
    // The slot at the cursor (empty once every slot is resolved).
    if (m_markerCursor < 0 || m_markerCursor >= static_cast<int>(m_markerOrder.size()))
        return QString();
    return AutoRig::markerLabel(m_markerOrder[m_markerCursor]);
}

bool AutoRigController::beginMarkerPlacement(const QString& upAxis)
{
    Ogre::Entity* e = selectedRiggableEntity();
    if (!e) { emit error(QStringLiteral("Select a static (unrigged) mesh first.")); return false; }

    const QString ax = upAxis.trimmed().toLower();
    m_upAxis = (ax == "x") ? 0 : (ax == "z") ? 2 : 1;
    m_markerEntityName = e->getName();
    m_markerOrder = AutoRig::humanoidMarkerOrder();
    m_markers.clear();
    m_markerCursor = 0;
    clearMarkerOverlays();
    m_markerMode = true;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.auto_rig"),
        QStringLiteral("marker placement begin entity=%1")
            .arg(QString::fromStdString(m_markerEntityName)));
    emit markerModeChanged();
    emit markerCountChanged();
    return true;
}

void AutoRigController::cancelMarkerPlacement()
{
    if (!m_markerMode) return;
    m_markerMode = false;
    m_markers.clear();
    m_markerOrder.clear();
    m_markerCursor = 0;
    clearMarkerOverlays();
    emit markerModeChanged();
    emit markerCountChanged();
}

void AutoRigController::notifyRiggingChanged(const std::string& entityName)
{
    // Drop any skeleton-debug overlay on this entity — once the skeleton state
    // flips (rig ↔ unrig on undo/redo) a previously-shown overlay references a
    // skeleton instance that's being recreated/destroyed, which would dangle.
    if (auto* ppc = PropertiesPanelController::instance()) {
        ppc->toggleSkeletonDebug(QString::fromStdString(entityName), false);
        // The skeleton state flipped but the SELECTION didn't, so PPC won't
        // re-emit on its own — poke it so the Skeleton section (gated on
        // PropertiesPanelController.hasSkeletonSelection) appears/disappears.
        ppc->notifySelectionMetadataChanged();
    }
    // Re-evaluate the Inspector Rigging / Skeleton section visibility.
    emit selectionChanged();
}

void AutoRigController::skipCurrentMarker()
{
    if (!m_markerMode) return;
    if (m_markerCursor >= static_cast<int>(m_markerOrder.size())) return;
    // Just advance the cursor past this slot — no marker is stored, so the
    // joint keeps its template fit. (No m_markers entry; the cursor is what
    // makes currentMarkerLabel move on.)
    ++m_markerCursor;
    emit markerCountChanged();
}

void AutoRigController::undoLastMarker()
{
    if (!m_markerMode || m_markerCursor <= 0) return;
    // Step back one slot. If that slot was PLACED (its id is in m_markers),
    // drop the marker too; if it was skipped, there's nothing to remove.
    --m_markerCursor;
    const AutoRig::MarkerId id = m_markerOrder[m_markerCursor];
    for (auto it = m_markers.begin(); it != m_markers.end(); ++it) {
        if (it->id == id) { m_markers.erase(it); break; }
    }
    refreshMarkerOverlays();
    emit markerCountChanged();
}

bool AutoRigController::handleMarkerClick(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_markerMode || !widget) return false;
    if (m_markerCursor < 0 || m_markerCursor >= static_cast<int>(m_markerOrder.size()))
        return true;   // all slots resolved; consume click but do nothing
    const AutoRig::MarkerId curId = m_markerOrder[m_markerCursor];
    const QString cur = AutoRig::markerLabel(curId);

    Ogre::Entity* e = selectedRiggableEntity();
    if (!e || e->getName() != m_markerEntityName) {
        // Selection changed out from under us — abort marker mode.
        cancelMarkerPlacement();
        return false;
    }

    auto* spaceCam = widget->getSpaceCamera();
    auto* cam = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!cam) return true;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return true;

    const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
    const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
    const Ogre::Ray ray = cam->getCameraToViewportRay(nx, ny);

    std::vector<Ogre::Vector3> tris;
    if (!gatherWorldTriangles(e, tris)) return true;

    float bestT = std::numeric_limits<float>::infinity();
    Ogre::Vector3 hit;
    bool found = false;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        const float t = rayTri(ray.getOrigin(), ray.getDirection(), tris[i], tris[i+1], tris[i+2]);
        if (t > 0 && t < bestT) { bestT = t; hit = ray.getOrigin() + ray.getDirection() * t; found = true; }
    }
    if (!found) return true;   // missed the mesh — consume (don't select something else)

    // Store the marker in MESH-LOCAL space (the fit works in local coords).
    Ogre::Node* node = e->getParentSceneNode();
    const Ogre::Vector3 local = node
        ? node->_getFullTransform().inverse() * hit : hit;

    // Record the marker for the current slot and advance the cursor.
    AutoRig::Marker m;
    m.id = curId; m.set = true;
    m.pos = { local.x, local.y, local.z };
    m_markers.push_back(m);
    ++m_markerCursor;

    refreshMarkerOverlays();
    emit markerPlaced(cur);
    emit markerCountChanged();
    return true;
}

QVariantMap AutoRigController::commitMarkerRig(bool alsoSkin)
{
    QVariantMap result;
    Ogre::Entity* entity = selectedRiggableEntity();
    if (!entity || entity->getName() != m_markerEntityName) {
        const auto msg = QStringLiteral("Selected mesh is no longer valid for rigging.");
        emit error(msg); result["applied"] = false; result["error"] = msg;
        cancelMarkerPlacement();
        return result;
    }

    // Collect only the SET markers (placed ones); skipped/unset fall back.
    std::vector<AutoRig::Marker> placed;
    for (const auto& m : m_markers) if (m.set) placed.push_back(m);

    AutoRig::Options opts;
    opts.tmpl   = AutoRig::Template::Humanoid;   // markers are a humanoid concept
    opts.upAxis = m_upAxis;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.auto_rig"),
        QStringLiteral("marker rig commit entity=%1 markers=%2")
            .arg(QString::fromStdString(m_markerEntityName)).arg(placed.size()));

    m_busy = true; emit busyChanged();
    AutoRig::Report report;
    bool skinned = false;
    try {
        // Undoable, same as autoRigSelected — markers ride along in the command.
        auto* cmd = new AutoRigCommand(entity->getName(), opts, placed, alsoSkin);
        UndoManager::getSingleton()->push(cmd);
        report  = cmd->report();
        skinned = cmd->skinned();
    } catch (const Ogre::Exception& ex) {
        m_busy = false; emit busyChanged();
        const auto msg = QString::fromStdString(ex.getFullDescription());
        emit error(QStringLiteral("Ogre error: %1").arg(msg));
        result["applied"] = false; result["error"] = msg;
        return result;
    }

    // Leave marker mode (clears overlays) regardless of outcome.
    m_markerMode = false;
    m_markers.clear();
    m_markerOrder.clear();
    clearMarkerOverlays();
    emit markerModeChanged();

    m_busy = false; emit busyChanged();
    emit selectionChanged();

    result["applied"]         = report.applied;
    result["meshName"]        = report.meshName;
    result["boneCount"]       = report.boneCount;
    result["markersApplied"]  = report.markersApplied;
    result["skinned"]         = skinned;
    if (!report.error.isEmpty()) result["error"] = report.error;

    if (report.applied) emit rigged(result);
    else emit error(report.error.isEmpty() ? QStringLiteral("Auto-rig failed") : report.error);
    return result;
}

void AutoRigController::clearMarkerOverlays()
{
    auto* mgr = Manager::getSingletonPtr();
    Ogre::SceneManager* scene = mgr ? mgr->getSceneMgr() : nullptr;
    for (Ogre::SceneNode* n : m_markerNodes) {
        if (!n) continue;
        n->removeAndDestroyAllChildren();
        if (scene) {
            // Destroy attached entities then the node.
            auto objs = n->getAttachedObjects();
            for (auto* o : objs) scene->destroyMovableObject(o);
            scene->destroySceneNode(n);
        }
    }
    m_markerNodes.clear();
}

void AutoRigController::refreshMarkerOverlays()
{
    clearMarkerOverlays();
    auto* mgr = Manager::getSingletonPtr();
    Ogre::SceneManager* scene = mgr ? mgr->getSceneMgr() : nullptr;
    Ogre::Entity* e = selectedRiggableEntity();
    if (!scene || !e) return;
    Ogre::Node* node = e->getParentSceneNode();

    // Small unit sphere mesh + bright unlit material, created once.
    const std::string meshName = "__AutoRigMarkerSphere__";
    if (!Ogre::MeshManager::getSingleton().resourceExists(meshName)) {
        Ogre::MeshManager::getSingleton().createManual(meshName,
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        // Use Ogre's built-in sphere via the prefab if manual gen is unavailable.
    }
    const std::string matName = "__AutoRigMarkerMat__";
    auto& mm = Ogre::MaterialManager::getSingleton();
    if (!mm.resourceExists(matName)) {
        auto mat = mm.create(matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setDiffuse(Ogre::ColourValue(1.0f, 0.85f, 0.1f, 1.0f));
        pass->setAmbient(Ogre::ColourValue(1.0f, 0.85f, 0.1f, 1.0f));
        pass->setDepthCheckEnabled(false);   // always visible over the mesh
    }

    // Marker world size ~3% of the mesh's bounding radius.
    const Ogre::Real r = e->getBoundingRadius() * 0.03f;

    for (const auto& m : m_markers) {
        if (!m.set) continue;
        const Ogre::Vector3 localPos(
            static_cast<Ogre::Real>(m.pos[0]),
            static_cast<Ogre::Real>(m.pos[1]),
            static_cast<Ogre::Real>(m.pos[2]));
        const Ogre::Vector3 worldPos = node ? node->_getFullTransform() * localPos : localPos;

        Ogre::SceneNode* sn = scene->getRootSceneNode()->createChildSceneNode();
        Ogre::Entity* sphere = nullptr;
        try {
            sphere = scene->createEntity(Ogre::SceneManager::PT_SPHERE);
        } catch (...) { sphere = nullptr; }
        if (sphere) {
            sphere->setMaterialName(matName);
            sphere->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
            sn->attachObject(sphere);
            // Ogre's PT_SPHERE has radius 100; scale to the desired world radius.
            const Ogre::Real s = (r > 1e-4f ? r : 0.02f) / 100.0f;
            sn->setScale(s, s, s);
        }
        sn->setPosition(worldPos);
        m_markerNodes.push_back(sn);
    }
}

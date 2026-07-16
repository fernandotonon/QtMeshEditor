#include "FaceRigController.h"

#include "FaceRig/ArkitTemplate.h"
#include "FaceRig/FaceRigAttach.h"
#include "FaceRig/FaceRigLandmarks.h"
#include "GamificationManager.h"
#include "Manager.h"
#include "OgreWidget.h"
#include "SelectionSet.h"
#include "SpaceCamera.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/MorphCommands.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>

#include <cstdint>
#include <memory>
#include <set>
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

    // MAIN thread: facial-landmark anchors (renders template + user — Ogre) so
    // the worker's fit lands on the real face features. Empty when ONNX/model/
    // face-detection unavailable → the fit runs unanchored (previous behaviour).
    setStatus(QStringLiteral("Detecting face landmarks…"));
    std::vector<float> headV; std::vector<int> headF;
    FaceRig::headSubmesh(*geo, headV, headF);
    const std::vector<FaceRig::NricpLandmark> anchors =
        FaceRig::buildLandmarkAnchors(entity, headV, headF, *tmpl);

    m_geo = geo;
    return runRigAsync(tmpl, maxShapes, maxResidualPct, anchors);
}

bool FaceRigController::runRigAsync(
    const std::shared_ptr<FaceRig::ArkitTemplate>& tmpl,
    int maxShapes, double maxResidualPct,
    const std::vector<FaceRig::NricpLandmark>& anchorsIn)
{
    auto geo = m_geo;
    if (!geo || !geo->valid()) {
        emit error(QStringLiteral("Could not read the mesh geometry."));
        return false;
    }
    auto* sel = SelectionSet::getSingleton();
    const auto entities = sel ? sel->getResolvedEntities()
                              : QList<Ogre::Entity*>{};
    Ogre::Entity* entity = entities.isEmpty() ? nullptr : entities.first();
    if (!entity) { emit error(QStringLiteral("No mesh selected.")); return false; }

    FaceRig::FaceRigOptions opts;
    opts.maxShapes = maxShapes;
    opts.maxFitResidualPct = maxResidualPct;
    auto anchors =
        std::make_shared<std::vector<FaceRig::NricpLandmark>>(anchorsIn);

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.face_rig"),
        QStringLiteral("face_rig entity=%1 verts=%2 template=%3v anchors=%4")
            .arg(QString::fromStdString(entity->getName()))
            .arg(geo->userV.size() / 3).arg(tmpl->vertexCount())
            .arg(anchors->size()));

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
    std::thread([self, geo, tmpl, opts, entName, cancel, anchors]() {
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
                                  geo->headMask, *anchors, progress));

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
            // Hide the entity for the batch so the render loop doesn't touch its
            // pose/skin buffers while we rebuild them — restored after the single
            // _initialise. (Belt-and-braces with the animation-state disable.)
            Ogre::SceneNode* enode = entity->getParentSceneNode();
            const bool wasVisible = enode ? entity->getVisible() : true;
            if (enode) entity->setVisible(false);

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

            // Re-rig = REPLACE, not stack: VAT_POSE keyframes reference poses
            // BY INDEX, so attaching a second same-named set on an already-
            // rigged mesh both duplicates poses AND corrupts the existing
            // keyframe references — the "sliders do nothing" failure. Delete
            // the old same-named targets first, inside the same macro so
            // undo/redo stays atomic.
            std::set<std::string> existing;
            if (auto mesh = entity->getMesh())
                for (const Ogre::Pose* p : mesh->getPoseList())
                    if (p) existing.insert(p->getName());
            std::vector<DeleteMorphTargetCommand*> dels;
            for (const FaceRig::FaceRigShape& shape : result->shapes)
                if (existing.count(shape.name.toStdString()))
                    dels.push_back(new DeleteMorphTargetCommand(entity, shape.name));

            auto* undo = UndoManager::getSingleton();
            auto* stack = undo ? undo->stack() : nullptr;
            if (stack) stack->beginMacro(QStringLiteral("Add ARKit Blendshapes"));
            for (auto* del : dels) undo->push(del);
            for (auto* cmd : cmds) { undo->push(cmd); rep.shapesAttached++; }
            if (stack) stack->endMacro();
            qWarning("[facerig] rig: replaced %zu existing, attached %d shapes "
                     "(fit mean %.3f%% max %.3f%%)",
                     dels.size(), rep.shapesAttached,
                     rep.fitMeanResidualPct, rep.fitMaxResidualPct);
            if (enode) entity->setVisible(wasVisible);

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
            // Amplitude diagnostics: without these, "shapes attached but
            // invisible" (deltas 50x too small) looks identical to success in
            // the UI. jawDisp specifically because jawOpen is the shape users
            // test first.
            double maxDisp = 0, jawDisp = 0;
            for (const auto& sh : result->shapes) {
                maxDisp = std::max(maxDisp, double(sh.maxDisp));
                if (sh.name == QLatin1String("jawOpen"))
                    jawDisp = double(sh.maxDisp);
            }
            map["maxShapeDisp"] = maxDisp;
            map["jawOpenDisp"] = jawDisp;
            qWarning("[facerig] attached=%d jawOpenDisp=%.5f maxDisp=%.5f",
                     rep.shapesAttached, jawDisp, maxDisp);
            emit self->completed(map);
        }, Qt::QueuedConnection);
    }).detach();

    return true;
}

void FaceRigController::cancel()
{
    if (m_cancel) m_cancel->store(true);
}

// ─────────────────── Face-marker editing session ────────────────────

QStringList FaceRigController::markerLabels() const
{
    QStringList out;
    for (const auto& m : m_markers) out << m.label;
    return out;
}

bool FaceRigController::markerPlaced(int index) const
{
    return index >= 0 && index < int(m_markers.size())
           && m_markers[size_t(index)].placed;
}

bool FaceRigController::beginFaceMarkers()
{
    if (m_busy) { emit error(QStringLiteral("Busy.")); return false; }
    auto* sel = SelectionSet::getSingleton();
    const auto entities = sel ? sel->getResolvedEntities()
                              : QList<Ogre::Entity*>{};
    Ogre::Entity* entity = entities.isEmpty() ? nullptr : entities.first();
    if (!entity || !entity->getMesh()) {
        emit error(QStringLiteral("No mesh selected."));
        return false;
    }

    // Geometry + template (same prep as the direct rig).
    m_geo = std::make_shared<FaceRig::FaceRigGeometry>(
        FaceRig::extractGeometry(entity));
    if (!m_geo->valid()) {
        emit error(QStringLiteral("Could not read the mesh geometry."));
        return false;
    }
    m_downloading = !FaceRig::ArkitTemplate::present();
    setStatus(m_downloading ? QStringLiteral("Downloading face template…")
                            : QStringLiteral("Preparing…"));
    const QString path = FaceRig::ArkitTemplate::ensureModelBlocking();
    m_downloading = false;
    if (path.isEmpty()) {
        setStatus(QString());
        emit error(QStringLiteral("ARKit template unavailable."));
        return false;
    }
    m_markerTmpl = std::make_shared<FaceRig::ArkitTemplate>();
    QString loadErr;
    if (!m_markerTmpl->load(path, &loadErr)) {
        setStatus(QString());
        emit error(QStringLiteral("Failed to load ARKit template: %1").arg(loadErr));
        return false;
    }

    // Seed markers: template detection resolves template verts (reliable),
    // user detection seeds positions when confident, else sensible defaults.
    setStatus(QStringLiteral("Detecting face landmarks…"));
    std::vector<float> headV; std::vector<int> headF;
    FaceRig::headSubmesh(*m_geo, headV, headF);
    m_markers = FaceRig::seedFaceMarkers(entity, headV, headF, *m_markerTmpl,
                                         &m_seededConfident);
    setStatus(QString());
    if (m_markers.empty()) {
        emit error(QStringLiteral("Could not prepare face markers."));
        return false;
    }

    m_markerEntityName = entity->getName();
    m_markerMode = true;
    m_selMarker = 0;
    refreshMarkerOverlays();
    emit markerModeChanged();
    emit markersChanged();
    return true;
}

void FaceRigController::selectMarker(int index)
{
    if (!m_markerMode) return;
    m_selMarker = (index >= 0 && index < int(m_markers.size())) ? index : -1;
    refreshMarkerOverlays();
    emit markersChanged();
}

void FaceRigController::cancelFaceMarkers()
{
    if (!m_markerMode) return;
    m_markerMode = false;
    m_markers.clear();
    m_selMarker = -1;
    clearMarkerOverlays();
    setStatus(QString());
    emit markerModeChanged();
    emit markersChanged();
}

bool FaceRigController::handleMarkerClick(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_markerMode || !widget) return false;

    Ogre::Entity* entity = nullptr;
    for (Ogre::Entity* e : Manager::getSingleton()->getEntities())
        if (e && e->getMovableType() == "Entity"
            && e->getName() == m_markerEntityName) { entity = e; break; }
    if (!entity) { cancelFaceMarkers(); return false; }

    auto* spaceCam = widget->getSpaceCamera();
    auto* cam = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!cam) return true;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return true;
    const Ogre::Real nx = Ogre::Real(screenPos.x()) / vw;
    const Ogre::Real ny = Ogre::Real(screenPos.y()) / vh;
    const Ogre::Ray ray = cam->getCameraToViewportRay(nx, ny);

    // Ray-cast to the mesh surface (world-space triangles from the geometry).
    Ogre::Node* node = entity->getParentSceneNode();
    const Ogre::Matrix4 world = node ? node->_getFullTransform()
                                     : Ogre::Matrix4::IDENTITY;
    const auto& V = m_geo->userV; const auto& F = m_geo->userF;
    const int nv = int(V.size()/3);
    float bestT = std::numeric_limits<float>::max();
    Ogre::Vector3 hitLocal; bool found = false;
    for (size_t f = 0; f + 2 < F.size(); f += 3) {
        const int ia=F[f], ib=F[f+1], ic=F[f+2];
        if (ia<0||ib<0||ic<0||ia>=nv||ib>=nv||ic>=nv) continue;
        const Ogre::Vector3 a = world*Ogre::Vector3(V[size_t(ia)*3],V[size_t(ia)*3+1],V[size_t(ia)*3+2]);
        const Ogre::Vector3 b = world*Ogre::Vector3(V[size_t(ib)*3],V[size_t(ib)*3+1],V[size_t(ib)*3+2]);
        const Ogre::Vector3 c = world*Ogre::Vector3(V[size_t(ic)*3],V[size_t(ic)*3+1],V[size_t(ic)*3+2]);
        auto res = Ogre::Math::intersects(ray, a, b, c, true, false);
        if (res.first && res.second < bestT) {
            bestT = res.second;
            const Ogre::Vector3 w = ray.getPoint(res.second);
            hitLocal = world.inverse() * w;
            found = true;
        }
    }
    if (!found) return true;   // missed the mesh — consume

    if (m_selMarker < 0 || m_selMarker >= int(m_markers.size())) {
        // no selection → pick the nearest marker to the hit, don't move it.
        int best = -1; float bd = std::numeric_limits<float>::max();
        for (int i = 0; i < int(m_markers.size()); ++i) {
            const auto& p = m_markers[size_t(i)].userPos;
            const float d = (Ogre::Vector3(p[0],p[1],p[2]) - hitLocal).squaredLength();
            if (d < bd) { bd = d; best = i; }
        }
        m_selMarker = best;
    } else {
        // move the selected marker to the hit point.
        auto& m = m_markers[size_t(m_selMarker)];
        m.userPos = { hitLocal.x, hitLocal.y, hitLocal.z };
        m.placed = true;
        // auto-advance to the next unplaced marker for a smooth flow.
        int next = -1;
        for (int i = 1; i <= int(m_markers.size()); ++i) {
            const int idx = (m_selMarker + i) % int(m_markers.size());
            if (!m_markers[size_t(idx)].placed) { next = idx; break; }
        }
        m_selMarker = next >= 0 ? next : m_selMarker;
    }
    refreshMarkerOverlays();
    emit markersChanged();
    return true;
}

bool FaceRigController::rigFromMarkers(int maxShapes, double maxResidualPct)
{
    if (!m_markerMode) { emit error(QStringLiteral("Not in marker mode.")); return false; }
    auto tmpl = m_markerTmpl;
    if (!tmpl) { emit error(QStringLiteral("Template not loaded.")); return false; }
    const auto anchors = FaceRig::anchorsFromMarkers(m_markers, *tmpl);
    int placedCount = 0;
    for (const auto& m : m_markers) placedCount += m.placed ? 1 : 0;
    qWarning("[facerig] rigFromMarkers: %d/%zu markers placed -> %zu anchors",
             placedCount, m_markers.size(), anchors.size());
    // Leave marker mode (clears overlays) before the rig runs.
    m_markerMode = false;
    m_selMarker = -1;
    clearMarkerOverlays();
    emit markerModeChanged();
    emit markersChanged();
    if (!tmpl) { emit error(QStringLiteral("Template not loaded.")); return false; }
    return runRigAsync(tmpl, maxShapes, maxResidualPct, anchors);
}

void FaceRigController::clearMarkerOverlays()
{
    auto* mgr = Manager::getSingletonPtr();
    Ogre::SceneManager* scene = mgr ? mgr->getSceneMgr() : nullptr;
    for (Ogre::SceneNode* n : m_markerNodes) {
        if (!n) continue;
        if (scene) {
            auto objs = n->getAttachedObjects();
            for (auto* o : objs) scene->destroyMovableObject(o);
            scene->destroySceneNode(n);
        }
    }
    m_markerNodes.clear();
}

void FaceRigController::refreshMarkerOverlays()
{
    clearMarkerOverlays();
    auto* mgr = Manager::getSingletonPtr();
    Ogre::SceneManager* scene = mgr ? mgr->getSceneMgr() : nullptr;
    if (!scene) return;
    Ogre::Entity* entity = nullptr;
    for (Ogre::Entity* e : mgr->getEntities())
        if (e && e->getMovableType() == "Entity"
            && e->getName() == m_markerEntityName) { entity = e; break; }
    if (!entity) return;
    Ogre::Node* node = entity->getParentSceneNode();

    auto& mm = Ogre::MaterialManager::getSingleton();
    auto ensureMat = [&](const std::string& n, const Ogre::ColourValue& c) {
        if (!mm.resourceExists(n)) {
            auto mat = mm.create(n, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            auto* pass = mat->getTechnique(0)->getPass(0);
            // Lighting ON + self-illumination is what actually colours the
            // sphere — with lighting disabled Ogre ignores diffuse/ambient and
            // the markers render plain white (indistinguishable states).
            pass->setLightingEnabled(true);
            pass->setSelfIllumination(c);
            pass->setDiffuse(Ogre::ColourValue::Black);
            pass->setAmbient(Ogre::ColourValue::Black);
            pass->setSpecular(Ogre::ColourValue::Black);
            pass->setDepthCheckEnabled(false);
        }
    };
    ensureMat("__FaceMarkerMat__",       Ogre::ColourValue(1.0f, 0.85f, 0.1f, 1.0f)); // placed
    ensureMat("__FaceMarkerMatSel__",    Ogre::ColourValue(0.2f, 0.9f, 1.0f, 1.0f));  // selected
    ensureMat("__FaceMarkerMatUnset__",  Ogre::ColourValue(0.6f, 0.6f, 0.6f, 1.0f));  // default/unplaced

    const Ogre::Real r = entity->getBoundingRadius() * 0.02f;
    for (int i = 0; i < int(m_markers.size()); ++i) {
        const auto& m = m_markers[size_t(i)];
        const Ogre::Vector3 localPos(m.userPos[0], m.userPos[1], m.userPos[2]);
        const Ogre::Vector3 worldPos = node ? node->_getFullTransform()*localPos : localPos;
        Ogre::SceneNode* sn = scene->getRootSceneNode()->createChildSceneNode();
        Ogre::Entity* sphere = nullptr;
        try { sphere = scene->createEntity(Ogre::SceneManager::PT_SPHERE); } catch (...) {}
        if (sphere) {
            sphere->setMaterialName(i == m_selMarker ? "__FaceMarkerMatSel__"
                                    : m.placed ? "__FaceMarkerMat__"
                                               : "__FaceMarkerMatUnset__");
            sphere->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
            sn->attachObject(sphere);
            const Ogre::Real s = (r > 1e-4f ? r : 0.02f) / 100.0f
                                 * (i == m_selMarker ? 1.5f : 1.0f);
            sn->setScale(s, s, s);
        }
        sn->setPosition(worldPos);
        m_markerNodes.push_back(sn);
    }
}

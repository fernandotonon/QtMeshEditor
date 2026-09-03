#include "SkinWeightController.h"

#include "AnimationControlController.h"
#include "AnimationWidget.h"
#include "BoneWeightOverlay.h"
#include "EditModeController.h"
#include "Manager.h"
#include "SentryReporter.h"
#include "SkeletonEditor.h"
#include "SkinWeightsPost.h"
#include "TexturePaintController.h"
#include "UndoManager.h"

#include <OgreCamera.h>
#include <OgreRay.h>
#include <OgreSceneNode.h>
#include "OgreWidget.h"
#include "SpaceCamera.h"
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSkeleton.h>
#include <OgreSubMesh.h>

#include <QApplication>
#include <QTimer>
#include <QUndoCommand>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

SkinWeightController* g_instance = nullptr;

/// One weight edit (a stroke or a utility op) as a single undo step.
///
/// Snapshots the whole bone-assignment list per owner, like
/// ComputeSkinWeightsCommand — a stroke touches few vertices, but the list is a
/// multimap keyed by vertex, so a sparse diff would have to reconstruct
/// insertion order on undo. Whole-owner is simpler and provably correct.
class WeightEditCommand : public QUndoCommand
{
public:
    WeightEditCommand(std::string entityName,
                      QString label,
                      std::vector<SkinWeightController::OwnerSnapshot> before,
                      std::vector<SkinWeightController::OwnerSnapshot> after)
        : QUndoCommand(std::move(label))
        , m_entityName(std::move(entityName))
        , m_before(std::move(before))
        , m_after(std::move(after))
    {}

    void undo() override { apply(m_before); }
    void redo() override
    {
        // The edit already happened live, so the push-time redo must be a
        // no-op (the TexturePaintStrokeCommand idiom).
        if (m_skipFirstRedo) { m_skipFirstRedo = false; return; }
        apply(m_after);
    }

private:
    void apply(const std::vector<SkinWeightController::OwnerSnapshot>& snap)
    {
        // Resolve by NAME, not a cached pointer: the entity can be rebuilt
        // between undo steps.
        Ogre::Entity* entity = nullptr;
        if (auto* mgr = Manager::getSingletonPtr()) {
            for (auto* obj : mgr->getEntities()) {
                if (!obj || obj->getMovableType() != "Entity") continue;
                auto* e = static_cast<Ogre::Entity*>(obj);
                if (e->getName() == m_entityName) { entity = e; break; }
            }
        }
        if (!entity || !entity->getMesh()) return;
        SkinWeightController::restoreSnapshot(entity->getMesh().get(), snap);
        if (auto* c = SkinWeightController::instance()) c->resyncFromMesh();
    }

    std::string m_entityName;
    std::vector<SkinWeightController::OwnerSnapshot> m_before;
    std::vector<SkinWeightController::OwnerSnapshot> m_after;
    bool m_skipFirstRedo = true;
};

} // namespace

SkinWeightController::SkinWeightController(QObject* parent) : QObject(parent) {}
SkinWeightController::~SkinWeightController() = default;

SkinWeightController* SkinWeightController::instance()
{
    if (!g_instance) g_instance = new SkinWeightController();
    return g_instance;
}

SkinWeightController* SkinWeightController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void SkinWeightController::kill()
{
    delete g_instance;
    g_instance = nullptr;
}

// --- settings --------------------------------------------------------------

void SkinWeightController::setWeightPaintEnabled(bool on)
{
    if (on == m_enabled) return;
    m_enabled = on;
    if (on) {
        if (!ensureSession())
            m_status = QStringLiteral("Select a skinned mesh and a bone first.");

        // Painting without the heat map gives no feedback, so entering paint
        // mode turns the overlay ON. The inverse (hiding the overlay leaves
        // paint mode) lives in AnimationWidget::toggleBoneWeights, the single
        // point every overlay path funnels through.
        //
        // Guarded on isBoneWeightsShown so this cannot re-enter: that method
        // only calls back into setWeightPaintEnabled on the OFF branch, and
        // m_enabled is already true here, so the early-return would catch it
        // anyway — but not creating the cycle at all is cheaper to reason about.
        if (m_entity) {
            if (auto* animWidget = findAnimationWidget()) {
                if (!animWidget->isBoneWeightsShown(m_entity))
                    animWidget->toggleBoneWeights(m_entity, true);
            }
        }
    } else {
        if (m_strokeActive) endStroke();
    }

    // Show per-vertex dots while painting. The heat map is depth-check-off so it
    // bleeds through the surface, making it ambiguous which side of the mesh a
    // colour is on; the dots are depth-TESTED, so occlusion by the geometry
    // gives back the depth cue and makes individual vertices easier to target.
    // Done BEFORE closeSession() so m_entity is still set on the way out.
    if (auto* overlay = findOverlay())
        overlay->setShowVertices(on);

    if (!on)
        closeSession();
    SentryReporter::addBreadcrumb("scene.skel.weight.enable",
        QStringLiteral("enabled=%1").arg(on));
    emit weightPaintChanged();
}

void SkinWeightController::setBrushMode(int mode)
{
    const int m = std::clamp(mode, 0, 2);
    if (m == m_brushMode) return;
    m_brushMode = m;
    SentryReporter::addBreadcrumb("scene.skel.weight.mode",
        QStringLiteral("mode=%1").arg(m));
    emit weightPaintChanged();
}

void SkinWeightController::setMaxInfluences(int n)
{
    const int v = std::clamp(n, 1, 8);
    if (v == m_maxInfluences) return;
    m_maxInfluences = v;
    emit weightPaintChanged();
}

QString SkinWeightController::activeBoneName() const
{
    auto* acc = AnimationControlController::instance();
    return acc ? acc->selectedBone() : QString();
}

/// Bone that absorbs weight taken off the active bone when the active bone is a
/// vertex's ONLY influence.
///
/// The active bone's PARENT is the anatomically sensible recipient: weight
/// leaving a forearm belongs on the upper arm, not on some unrelated bone. A
/// row must sum to 1, so without a recipient a sole influence at 1.0 can never
/// be reduced — that was the "cannot subtract once it hits 1.0" bug.
///
/// Falls back to any other bone in the skeleton when the active bone is a ROOT
/// (no parent), so a root-weighted vertex is still paintable; -1 only when the
/// skeleton has a single bone, where the row genuinely has nowhere else to go.
int SkinWeightController::fallbackBoneHandle(int forBoneHandle) const
{
    // Relative to the bone BEING WRITTEN, not the UI's active bone: the numeric
    // setter names its own bone and may target one that is not selected, and a
    // fallback belonging to a different bone would move weight somewhere the
    // user never touched.
    const int active = forBoneHandle >= 0 ? forBoneHandle : activeBoneHandle();
    if (!m_entity || !m_entity->hasSkeleton() || active < 0) return -1;

    Ogre::Skeleton* skel = m_entity->getSkeleton();
    if (!skel) return -1;

    Ogre::Bone* bone = nullptr;
    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        Ogre::Bone* b = skel->getBone(i);
        if (b && static_cast<int>(b->getHandle()) == active) { bone = b; break; }
    }
    if (!bone) return -1;

    if (auto* parent = dynamic_cast<Ogre::Bone*>(bone->getParent()))
        return static_cast<int>(parent->getHandle());

    // Root bone: pick the first other bone so the vertex is not stuck at 1.0.
    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        Ogre::Bone* b = skel->getBone(i);
        if (b && static_cast<int>(b->getHandle()) != active)
            return static_cast<int>(b->getHandle());
    }
    return -1;
}

int SkinWeightController::activeBoneHandle() const
{
    auto* acc = AnimationControlController::instance();
    if (!acc) return -1;
    // selectedBonePtr re-resolves the skeleton live — a cached Bone* is unsafe
    // because Entity::_initialise(true) destroys the SkeletonInstance.
    Ogre::Bone* bone = acc->selectedBonePtr();
    return bone ? static_cast<int>(bone->getHandle()) : -1;
}

// --- session ---------------------------------------------------------------

bool SkinWeightController::ensureSession()
{
    Ogre::Entity* entity = SkeletonEditor::selectedSkinnedEntity();
    if (!entity) { m_status = QStringLiteral("No skinned mesh selected."); return false; }
    if (m_haveData && m_entity == entity) return true;

    QString err;
    SkinEvaluate::EvalData data;
    if (!SkinEvaluate::extract(entity, data, &err)) {
        m_status = QStringLiteral("Could not read weights: %1").arg(err);
        m_haveData = false;
        m_entity = nullptr;
        return false;
    }
    m_entity = entity;
    m_data = std::move(data);
    m_haveData = true;
    m_haveAdjacency = false;         // rebuilt lazily for the new mesh
    m_adjacency.clear();
    m_status = QStringLiteral("%1 vertices, %2 bones.")
                   .arg(m_data.weights.size()).arg(m_data.totalBones);
    return true;
}

void SkinWeightController::closeSession()
{
    m_entity = nullptr;
    m_data = {};
    m_haveData = false;
    m_adjacency.clear();
    m_haveAdjacency = false;
    m_hoverWeight = -1.0;
    m_hoverVertex = -1;
    emit hoverChanged();
}

void SkinWeightController::resyncFromMesh()
{
    if (!m_entity) return;
    QString err;
    SkinEvaluate::EvalData data;
    if (!SkinEvaluate::extract(m_entity, data, &err)) return;
    m_data = std::move(data);
    m_haveData = true;
    refreshOverlay();
    emit weightPaintChanged();
}

const std::vector<std::vector<int>>& SkinWeightController::adjacency()
{
    if (!m_haveAdjacency && m_haveData) {
        m_adjacency = SkinWeightsPost::buildAdjacency(
            static_cast<int>(m_data.weights.size()),
            m_data.indices.data(), m_data.indices.size());
        m_haveAdjacency = true;
    }
    return m_adjacency;
}

std::vector<std::uint8_t> SkinWeightController::lockedBoneFlags() const
{
    std::vector<std::uint8_t> flags;
    if (!m_haveData) return flags;
    flags.assign(static_cast<size_t>(std::max(0, m_data.totalBones)), 0u);
    // Locks are stored by NAME so they survive a skeleton rebind; resolve to
    // handles here.
    for (const QString& name : m_lockedBones) {
        const int h = m_data.boneNames.indexOf(name);
        if (h >= 0 && static_cast<size_t>(h) < flags.size()) flags[static_cast<size_t>(h)] = 1u;
    }
    return flags;
}

// --- mesh write-back -------------------------------------------------------

std::vector<SkinWeightController::OwnerSnapshot>
SkinWeightController::captureSnapshot(Ogre::Mesh* mesh)
{
    std::vector<OwnerSnapshot> out;
    if (!mesh) return out;
    // Mesh-level (shared) list only when some submesh actually uses it.
    bool anyShared = false;
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i)
        if (mesh->getSubMesh(i) && mesh->getSubMesh(i)->useSharedVertices) anyShared = true;
    if (anyShared) {
        OwnerSnapshot o;
        o.submeshIndex = -1;
        o.assignments = mesh->getBoneAssignments();
        out.push_back(std::move(o));
    }
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* sub = mesh->getSubMesh(i);
        if (!sub || sub->useSharedVertices) continue;
        OwnerSnapshot o;
        o.submeshIndex = static_cast<int>(i);
        o.assignments = sub->getBoneAssignments();
        out.push_back(std::move(o));
    }
    return out;
}

void SkinWeightController::restoreSnapshot(Ogre::Mesh* mesh,
                                           const std::vector<OwnerSnapshot>& snapshot)
{
    if (!mesh) return;
    for (const auto& o : snapshot) {
        Ogre::SubMesh* sub = nullptr;
        if (o.submeshIndex >= 0) {
            if (o.submeshIndex >= mesh->getNumSubMeshes()) continue;
            sub = mesh->getSubMesh(static_cast<unsigned short>(o.submeshIndex));
            if (!sub) continue;
        }
        if (sub) sub->clearBoneAssignments();
        else     mesh->clearBoneAssignments();
        for (const auto& kv : o.assignments) {
            if (sub) sub->addBoneAssignment(kv.second);
            else     mesh->addBoneAssignment(kv.second);
        }
        // Safe here (and required): this re-packs BLEND bytes into the SAME
        // vertex buffer the live SkeletonInstance references. It is the
        // VertexData-swap case that shatters a live mesh, not this.
        if (sub) sub->_compileBoneAssignments();
        else     mesh->_compileBoneAssignments();
    }
}

void SkinWeightController::flushToMesh()
{
    if (!m_haveData || !m_entity || !m_entity->getMesh()) return;
    Ogre::MeshPtr mesh = m_entity->getMesh();

    // Walk owners in the SAME order SkinEvaluate::extract used (shared block
    // first, then each non-shared submesh) so the global vertex index maps back
    // onto the right owner-local index. Getting this wrong would scatter
    // weights onto unrelated vertices.
    size_t base = 0;
    auto writeOwner = [&](Ogre::SubMesh* sub, size_t vertexCount) {
        if (sub) sub->clearBoneAssignments();
        else     mesh->clearBoneAssignments();
        for (size_t v = 0; v < vertexCount; ++v) {
            const size_t gv = base + v;
            if (gv >= m_data.weights.size()) break;
            const auto& vw = m_data.weights[gv];
            for (int k = 0; k < vw.count; ++k) {
                if (vw.boneIndices[k] < 0 || vw.weights[k] <= 0.0) continue;
                Ogre::VertexBoneAssignment vba;
                vba.vertexIndex = static_cast<unsigned int>(v);
                vba.boneIndex   = static_cast<unsigned short>(vw.boneIndices[k]);
                vba.weight      = static_cast<float>(vw.weights[k]);
                if (sub) sub->addBoneAssignment(vba);
                else     mesh->addBoneAssignment(vba);
            }
        }
        // Once per owner per flush — this re-packs the whole owner's blend
        // buffer, so calling it per dab would crawl on a real mesh.
        if (sub) sub->_compileBoneAssignments();
        else     mesh->_compileBoneAssignments();
        base += vertexCount;
    };

    if (mesh->sharedVertexData)
        writeOwner(nullptr, mesh->sharedVertexData->vertexCount);
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* sub = mesh->getSubMesh(i);
        if (!sub || sub->useSharedVertices || !sub->vertexData) continue;
        writeOwner(sub, sub->vertexData->vertexCount);
    }
    refreshOverlay();
}

void SkinWeightController::scheduleFlush()
{
    if (m_flushScheduled) return;
    m_flushScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_flushScheduled = false;
        flushToMesh();
    });
}

void SkinWeightController::refreshOverlay()
{
    // The overlay reads the mesh's assignments directly, so it only needs a
    // nudge after a flush. Deliberately not a hard dependency: weight painting
    // still works with the overlay hidden.
    if (!m_entity) return;
    m_entity->_updateAnimation();

    // Restamp the heat map so the stroke is visible AS it is painted. The
    // overlay caches per-vertex colours at build time, so _updateAnimation()
    // alone only moves the existing (stale) colours with the mesh — the weights
    // would not appear to change until something forced a full rebuild.
    if (auto* overlay = findOverlay())
    {
        if (overlay->isVisible())
            overlay->refreshColours();
    }
}

/// The heat-map overlay for the painted entity, or nullptr.
///
/// BoneWeightOverlay is owned by AnimationWidget, which has no singleton, so it
/// is reached the way MCPServer does: findChild off a top-level window. A
/// missing widget/overlay is NORMAL (headless tests, or the heat map toggled
/// off) and must stay silent — painting never depends on the overlay existing.
AnimationWidget* SkinWeightController::findAnimationWidget() const
{
    for (QWidget* top : QApplication::topLevelWidgets())
    {
        // The widget may BE a top-level (no parent) or a descendant of one, so
        // check both. Only descending missed a parentless AnimationWidget.
        if (auto* w = qobject_cast<AnimationWidget*>(top))
            return w;
        if (auto* w = top->findChild<AnimationWidget*>())
            return w;
    }
    return nullptr;
}

BoneWeightOverlay* SkinWeightController::findOverlay() const
{
    if (!m_entity)
        return nullptr;
    auto* animWidget = findAnimationWidget();
    return animWidget ? animWidget->getBoneWeightOverlay(m_entity) : nullptr;
}

void SkinWeightController::pushUndo(const QString& label,
                                    std::vector<OwnerSnapshot> before)
{
    if (!m_entity || !m_entity->getMesh()) return;
    auto after = captureSnapshot(m_entity->getMesh().get());
    auto* cmd = new WeightEditCommand(m_entity->getName(), label,
                                      std::move(before), std::move(after));
    if (auto* um = UndoManager::getSingleton()) um->push(cmd);
    else delete cmd;
}

bool SkinWeightController::runUndoableOp(const QString& label,
                                         const std::function<int()>& op)
{
    if (!ensureSession()) return false;
    auto before = captureSnapshot(m_entity->getMesh().get());
    const int changed = op();
    if (changed <= 0) {
        m_status = QStringLiteral("%1: nothing to change.").arg(label);
        emit weightPaintChanged();
        return false;
    }
    flushToMesh();                     // ops are one-shot; flush immediately
    pushUndo(label, std::move(before));
    m_status = QStringLiteral("%1: %2 vertices.").arg(label).arg(changed);
    SentryReporter::addBreadcrumb("scene.skel.weight.op",
        QStringLiteral("%1 verts=%2").arg(label).arg(changed));
    emit weightPaintChanged();
    return true;
}

// --- stroke ----------------------------------------------------------------

bool SkinWeightController::hitTestLocalPoint(OgreWidget* widget,
                                             const QPoint& screenPos,
                                             double outLocal[3]) const
{
    if (!m_haveData || !m_entity || !widget) return false;
    if (m_data.indices.empty() || m_data.positions.empty()) return false;
    auto* spaceCam = widget->getSpaceCamera();
    auto* camera = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!camera) return false;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return false;

    const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
    const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
    const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);

    Ogre::SceneNode* node = m_entity->getParentSceneNode();
    const Ogre::Affine3 worldToLocal =
        node ? node->_getFullTransform().inverse() : Ogre::Affine3::IDENTITY;
    const Ogre::Vector3 o = worldToLocal * ray.getOrigin();
    Ogre::Vector3 d = worldToLocal.linear() * ray.getDirection();
    d.normalise();

    // Moller-Trumbore over the session's own triangle list, so the hit is in
    // the same index space as m_data.weights.
    const float* P = m_data.positions.data();
    auto vertexAt = [P](std::uint32_t i) {
        return Ogre::Vector3(P[i * 3 + 0], P[i * 3 + 1], P[i * 3 + 2]);
    };
    Ogre::Real bestT = std::numeric_limits<Ogre::Real>::infinity();
    bool found = false;
    const size_t triCount = m_data.indices.size() / 3;
    const size_t vertCount = m_data.positions.size() / 3;
    for (size_t t = 0; t < triCount; ++t) {
        const std::uint32_t i0 = m_data.indices[t * 3 + 0];
        const std::uint32_t i1 = m_data.indices[t * 3 + 1];
        const std::uint32_t i2 = m_data.indices[t * 3 + 2];
        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) continue;
        const Ogre::Vector3 v0 = vertexAt(i0);
        const Ogre::Vector3 e1 = vertexAt(i1) - v0;
        const Ogre::Vector3 e2 = vertexAt(i2) - v0;
        const Ogre::Vector3 pvec = d.crossProduct(e2);
        const Ogre::Real det = e1.dotProduct(pvec);
        if (std::abs(det) < 1e-8f) continue;
        const Ogre::Real invDet = 1.0f / det;
        const Ogre::Vector3 tvec = o - v0;
        const Ogre::Real u = tvec.dotProduct(pvec) * invDet;
        if (u < 0.0f || u > 1.0f) continue;
        const Ogre::Vector3 qvec = tvec.crossProduct(e1);
        const Ogre::Real v = d.dotProduct(qvec) * invDet;
        if (v < 0.0f || u + v > 1.0f) continue;
        const Ogre::Real tHit = e2.dotProduct(qvec) * invDet;
        if (tHit <= 0.0f || tHit >= bestT) continue;
        bestT = tHit;
        const Ogre::Vector3 hit = o + d * tHit;
        outLocal[0] = hit.x; outLocal[1] = hit.y; outLocal[2] = hit.z;
        found = true;
    }
    return found;
}

bool SkinWeightController::beginStroke(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_enabled || m_strokeActive) return false;
    if (!ensureSession()) return false;
    if (activeBoneHandle() < 0) {
        m_status = QStringLiteral("Select a bone to paint.");
        emit weightPaintChanged();
        return false;
    }
    // Snapshot up-front so the whole stroke is one undo step.
    m_strokeBefore = captureSnapshot(m_entity->getMesh().get());
    m_strokeActive = true;
    m_strokeDirty = false;
    SentryReporter::addBreadcrumb("scene.skel.weight.stroke", QStringLiteral("begin"));
    updateStroke(widget, screenPos);   // first dab lands on press
    return true;
}

void SkinWeightController::updateStroke(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_strokeActive || !m_haveData) return;
    const int bone = activeBoneHandle();
    if (bone < 0) return;

    double hit[3] = {0, 0, 0};
    if (!hitTestLocalPoint(widget, screenPos, hit)) {
        return;                        // off-mesh: no dab (matches vertex paint)
    }

    auto* em = EditModeController::instance();
    WeightPaintOps::DabOptions o;
    // Brush settings come from EditModeController, the canonical owner, so both
    // brushes share one set of controls.
    o.radius   = em ? em->vertexPaintRadius()   : 0.02;
    o.strength = em ? em->vertexPaintStrength() : 0.5;
    o.falloff  = em ? em->vertexPaintFalloff()  : 0.5;
    o.shape    = (em && em->vertexPaintShape() == EditModeController::ShapeSquare)
                     ? WeightPaintOps::BrushShape::Square
                     : WeightPaintOps::BrushShape::Round;
    o.mode = static_cast<WeightPaintOps::BrushMode>(m_brushMode);
    o.maxInfluences = m_maxInfluences;
    o.fallbackBoneHandle = fallbackBoneHandle();

    const double center[3] = {hit[0], hit[1], hit[2]};
    const auto locked = lockedBoneFlags();
    const int n = WeightPaintOps::applyDab(
        m_data.positions.data(), static_cast<int>(m_data.weights.size()),
        m_data.weights, center, bone, o, locked,
        o.mode == WeightPaintOps::BrushMode::Blur ? adjacency()
                                                  : std::vector<std::vector<int>>{});
    if (n > 0) {
        m_strokeDirty = true;
        scheduleFlush();               // coalesced, never per dab
    }
    // Keep the readout live while painting.
    updateHover(widget, screenPos);
}

void SkinWeightController::endStroke()
{
    if (!m_strokeActive) return;
    m_strokeActive = false;
    if (!m_strokeDirty) { m_strokeBefore.clear(); return; }

    // Flush before snapshotting "after", or the undo would capture a state the
    // mesh has not reached yet.
    m_flushScheduled = false;
    flushToMesh();
    pushUndo(QStringLiteral("Paint Skin Weights"), std::move(m_strokeBefore));
    m_strokeBefore.clear();
    m_strokeDirty = false;
    SentryReporter::addBreadcrumb("scene.skel.weight.stroke", QStringLiteral("end"));
    emit weightPaintChanged();
}

void SkinWeightController::updateHover(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_haveData) return;
    double hit[3] = {0, 0, 0};
    if (!hitTestLocalPoint(widget, screenPos, hit)) {
        if (m_hoverVertex != -1 || m_hoverWeight >= 0.0) {
            m_hoverVertex = -1;
            m_hoverWeight = -1.0;      // -1 reads as "off mesh", not "zero weight"
            emit hoverChanged();
        }
        return;
    }
    // Nearest vertex to the hit; brute force, matching the vertex-colour brush.
    int best = -1;
    double bestD2 = 0.0;
    const int count = static_cast<int>(m_data.weights.size());
    for (int v = 0; v < count; ++v) {
        const double dx = m_data.positions[v * 3 + 0] - hit[0];
        const double dy = m_data.positions[v * 3 + 1] - hit[1];
        const double dz = m_data.positions[v * 3 + 2] - hit[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (best < 0 || d2 < bestD2) { bestD2 = d2; best = v; }
    }
    const int bone = activeBoneHandle();
    const double w = (best >= 0 && bone >= 0)
        ? WeightPaintOps::weightOf(m_data.weights[static_cast<size_t>(best)], bone)
        : -1.0;
    if (best != m_hoverVertex || std::abs(w - m_hoverWeight) > 1e-6) {
        m_hoverVertex = best;
        m_hoverWeight = w;
        emit hoverChanged();
    }
}

// --- utility ops -----------------------------------------------------------

bool SkinWeightController::normalizeAll()
{
    return runUndoableOp(QStringLiteral("Normalize Weights"), [this]() {
        WeightPaintOps::normalize(m_data.weights);
        return static_cast<int>(m_data.weights.size());
    });
}

bool SkinWeightController::smoothAll(int iterations)
{
    return runUndoableOp(QStringLiteral("Smooth Weights"), [this, iterations]() {
        const auto& adj = adjacency();
        if (adj.size() != m_data.weights.size()) return 0;
        WeightPaintOps::smooth(m_data.weights, adj, std::max(1, iterations));
        return static_cast<int>(m_data.weights.size());
    });
}

bool SkinWeightController::limitInfluencesAll(int maxInfluences)
{
    return runUndoableOp(QStringLiteral("Limit Weights"), [this, maxInfluences]() {
        WeightPaintOps::limitInfluences(m_data.weights, maxInfluences);
        return static_cast<int>(m_data.weights.size());
    });
}

bool SkinWeightController::mirrorAll(int axis, double tolerance)
{
    return runUndoableOp(QStringLiteral("Mirror Weights"), [this, axis, tolerance]() {
        return WeightPaintOps::mirrorByPosition(
            m_data.positions.data(), static_cast<int>(m_data.weights.size()),
            m_data.weights, axis, /*pivot=*/0.0, tolerance, lockedBoneFlags());
    });
}

bool SkinWeightController::fillConnectedAtHover(int maxHops)
{
    if (m_hoverVertex < 0) {
        m_status = QStringLiteral("Hover the mesh first to pick a fill seed.");
        emit weightPaintChanged();
        return false;
    }
    const int bone = activeBoneHandle();
    if (bone < 0) return false;
    auto* em = EditModeController::instance();
    const double strength = em ? em->vertexPaintStrength() : 0.5;
    const double falloff = em ? em->vertexPaintFalloff() : 0.5;
    const int seed = m_hoverVertex;
    return runUndoableOp(QStringLiteral("Fill Connected"),
                         [this, seed, bone, strength, falloff, maxHops]() {
        return WeightPaintOps::fillConnected(m_data.weights, adjacency(), seed,
                                             bone, strength, falloff, maxHops,
                                             lockedBoneFlags());
    });
}

// --- bone locking ----------------------------------------------------------

bool SkinWeightController::isBoneLocked(const QString& boneName) const
{
    return std::find(m_lockedBones.begin(), m_lockedBones.end(), boneName)
           != m_lockedBones.end();
}

void SkinWeightController::setBoneLocked(const QString& boneName, bool locked)
{
    if (boneName.isEmpty()) return;
    const bool already = isBoneLocked(boneName);
    if (locked == already) return;
    if (locked) m_lockedBones.push_back(boneName);
    else m_lockedBones.erase(
        std::remove(m_lockedBones.begin(), m_lockedBones.end(), boneName),
        m_lockedBones.end());
    SentryReporter::addBreadcrumb("scene.skel.weight.lock",
        QStringLiteral("%1 locked=%2").arg(boneName).arg(locked));
    emit weightPaintChanged();
}

QStringList SkinWeightController::lockedBoneNames() const
{
    QStringList out;
    for (const QString& n : m_lockedBones) out << n;
    return out;
}

// --- per-vertex inspector --------------------------------------------------

QStringList SkinWeightController::vertexWeights(int vertexIndex) const
{
    QStringList out;
    if (!m_haveData || vertexIndex < 0
        || static_cast<size_t>(vertexIndex) >= m_data.weights.size()) return out;
    const auto& vw = m_data.weights[static_cast<size_t>(vertexIndex)];
    // Sort descending so the dominant influence reads first.
    std::vector<std::pair<double, int>> rows;
    for (int k = 0; k < vw.count; ++k)
        if (vw.boneIndices[k] >= 0) rows.emplace_back(vw.weights[k], vw.boneIndices[k]);
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [w, handle] : rows) {
        const QString name = (handle < m_data.boneNames.size())
                                 ? m_data.boneNames[handle]
                                 : QStringLiteral("bone%1").arg(handle);
        out << QStringLiteral("%1=%2").arg(name).arg(w, 0, 'f', 4);
    }
    return out;
}

bool SkinWeightController::setVertexWeight(int vertexIndex, const QString& boneName,
                                           double weight)
{
    if (!ensureSession()) return false;
    if (vertexIndex < 0 || static_cast<size_t>(vertexIndex) >= m_data.weights.size())
        return false;
    const int handle = m_data.boneNames.indexOf(boneName);
    if (handle < 0) return false;
    if (isBoneLocked(boneName)) {
        m_status = QStringLiteral("%1 is locked.").arg(boneName);
        emit weightPaintChanged();
        return false;
    }
    const int idx = vertexIndex;
    const double w = std::clamp(weight, 0.0, 1.0);
    const auto locked = lockedBoneFlags();
    const int fallback = fallbackBoneHandle(handle);
    return runUndoableOp(QStringLiteral("Set Vertex Weight"),
                         [this, idx, handle, w, locked, fallback]() {
        auto& vw = m_data.weights[static_cast<size_t>(idx)];
        const double before = WeightPaintOps::weightOf(vw, handle);
        // NOT setWeight + normalizeRow: normalizeRow rescales every entry
        // including the one just written, so lowering a SOLE influence
        // renormalised it straight back to 1.0 — the same "cannot subtract at
        // 1.0" bug the brush had, reached through the numeric setter instead.
        WeightPaintOps::writeWeightHoldingTarget(vw, handle, w, locked, fallback);
        return std::abs(WeightPaintOps::weightOf(vw, handle) - before) > 1e-9 ? 1 : 0;
    });
}

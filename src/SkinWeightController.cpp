#include "SkinWeightController.h"

#include "AnimationControlController.h"
#include "EditModeController.h"
#include "Manager.h"
#include "SentryReporter.h"
#include "SkeletonEditor.h"
#include "SkinWeightsPost.h"
#include "TexturePaintController.h"
#include "UndoManager.h"

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSkeleton.h>
#include <OgreSubMesh.h>

#include <QTimer>
#include <QUndoCommand>

#include <algorithm>
#include <cmath>

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
    } else {
        if (m_strokeActive) endStroke();
        closeSession();
    }
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

    // Reuse the paint controller's screen->mesh-local resolver rather than
    // duplicating a ray/triangle intersection.
    auto* tpc = TexturePaintController::instance();
    Ogre::Vector3 localPos, localNormal;
    if (!tpc || !tpc->hitTestLocalPoint(widget, screenPos, localPos, localNormal)) {
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

    const double center[3] = {localPos.x, localPos.y, localPos.z};
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
    auto* tpc = TexturePaintController::instance();
    Ogre::Vector3 localPos, localNormal;
    if (!tpc || !tpc->hitTestLocalPoint(widget, screenPos, localPos, localNormal)) {
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
        const double dx = m_data.positions[v * 3 + 0] - localPos.x;
        const double dy = m_data.positions[v * 3 + 1] - localPos.y;
        const double dz = m_data.positions[v * 3 + 2] - localPos.z;
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
    return runUndoableOp(QStringLiteral("Set Vertex Weight"), [this, idx, handle, w]() {
        auto& vw = m_data.weights[static_cast<size_t>(idx)];
        if (!WeightPaintOps::setWeight(vw, handle, w)) return 0;
        WeightPaintOps::normalizeRow(vw);
        return 1;
    });
}

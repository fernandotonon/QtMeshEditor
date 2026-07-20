#include "commands/SkeletonBoneCommands.h"

#include "SkeletonEditor.h"

#include "AnimationControlController.h"
#include "Manager.h"
#include "PropertiesPanelController.h"
#include "SentryReporter.h"
#include "SelectionSet.h"
#include "UndoManager.h"

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreHardwareBuffer.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>
#include <OgreSkeletonManager.h>
#include <OgreAnimationState.h>
#include <OgreSubMesh.h>
#include <OgreSubEntity.h>

#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr Ogre::Real kDisconnectGap = 0.1f;
constexpr Ogre::Real kConnectEpsilon = 0.05f;

Ogre::Entity* resolveEntityByName(const std::string& name)
{
    if (name.empty()) return nullptr;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (Ogre::Entity* ent : mgr->getEntities()) {
        if (!ent || ent->getMovableType() != "Entity") continue;
        if (ent->getName() == name) return ent;
    }
    return nullptr;
}

// Parent tip in parent-local space. Prefer the average of sibling bind
// positions; for a sole child the bind (initial) position IS the tip —
// disconnect keeps that initial tip and only offsets the live position.
Ogre::Vector3 parentTipLocal(Ogre::Bone* parent, Ogre::Bone* bone)
{
    Ogre::Vector3 tip = Ogre::Vector3::ZERO;
    int tipCount = 0;
    for (auto* sibling : parent->getChildren()) {
        auto* sb = static_cast<Ogre::Bone*>(sibling);
        if (sb == bone) continue;
        tip += sb->getInitialPosition();
        ++tipCount;
    }
    if (tipCount > 0)
        return tip / static_cast<Ogre::Real>(tipCount);

    Ogre::Vector3 pos = bone->getInitialPosition();
    if (pos.squaredLength() < 1e-12f)
        pos = Ogre::Vector3(0, kDisconnectGap, 0);
    return pos;
}

Ogre::SkeletonPtr skeletonResource(Ogre::Entity* entity)
{
    if (!entity || !entity->getMesh() || !entity->getMesh()->hasSkeleton())
        return {};
    return entity->getMesh()->getSkeleton();
}

void gatherBoneAssignments(Ogre::Mesh* mesh, SkeletonEditor::Snapshot& out)
{
    if (!mesh) return;
    out.meshAssignments.clear();
    out.submeshAssignments.clear();
    out.submeshAssignments.resize(mesh->getNumSubMeshes());

    const auto& shared = mesh->getBoneAssignments();
    out.meshAssignments.clear();
    out.meshAssignments.reserve(shared.size());
    for (const auto& kv : shared)
        out.meshAssignments.push_back(kv.second);

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub || sub->useSharedVertices) continue;
        const auto& subAssign = sub->getBoneAssignments();
        out.submeshAssignments[si].clear();
        out.submeshAssignments[si].reserve(subAssign.size());
        for (const auto& kv : subAssign)
            out.submeshAssignments[si].push_back(kv.second);
    }
}

void applyBoneAssignments(Ogre::Mesh* mesh, const SkeletonEditor::Snapshot& snap)
{
    if (!mesh) return;

    mesh->clearBoneAssignments();
    for (const auto& vba : snap.meshAssignments)
        mesh->addBoneAssignment(vba);
    if (!snap.meshAssignments.empty() || mesh->sharedVertexData)
        mesh->_compileBoneAssignments();

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub || sub->useSharedVertices) continue;
        if (si >= snap.submeshAssignments.size()) continue;
        sub->clearBoneAssignments();
        for (const auto& vba : snap.submeshAssignments[si])
            sub->addBoneAssignment(vba);
        if (!snap.submeshAssignments[si].empty())
            sub->_compileBoneAssignments();
    }
}

std::unordered_set<std::string> collectDescendants(const Ogre::Skeleton* skel,
                                                     const std::string& rootName)
{
    std::unordered_set<std::string> out;
    if (!skel || !skel->hasBone(rootName)) return out;

    std::vector<Ogre::Bone*> stack;
    stack.push_back(skel->getBone(rootName));
    while (!stack.empty()) {
        Ogre::Bone* bone = stack.back();
        stack.pop_back();
        out.insert(bone->getName());
        for (auto* child : bone->getChildren())
            stack.push_back(static_cast<Ogre::Bone*>(child));
    }
    return out;
}

unsigned short nearestKeptAncestor(unsigned short handle,
                                   const std::unordered_map<unsigned short, unsigned short>& parentByHandle,
                                   const std::unordered_set<unsigned short>& removed,
                                   const std::unordered_map<unsigned short, unsigned short>& oldToNew)
{
    unsigned short current = handle;
    while (removed.count(current)) {
        auto pit = parentByHandle.find(current);
        if (pit == parentByHandle.end() || pit->second == std::numeric_limits<unsigned short>::max())
            return std::numeric_limits<unsigned short>::max();
        current = pit->second;
    }
    auto it = oldToNew.find(current);
    return it == oldToNew.end() ? std::numeric_limits<unsigned short>::max() : it->second;
}

void worldToNewParentLocal(Ogre::Bone* bone,
                           Ogre::Bone* newParent,
                           Ogre::Vector3& outPos,
                           Ogre::Quaternion& outRot,
                           Ogre::Vector3& outScale)
{
    const Ogre::Vector3 wPos = bone->_getDerivedPosition();
    const Ogre::Quaternion wRot = bone->_getDerivedOrientation();
    const Ogre::Vector3 pScale = newParent->_getDerivedScale();
    const Ogre::Vector3 cScale = bone->_getDerivedScale();
    outPos = newParent->convertWorldToLocalPosition(wPos);
    outRot = newParent->convertWorldToLocalOrientation(wRot);
    outScale = Ogre::Vector3(
        cScale.x / std::max(pScale.x, Ogre::Real(1e-8)),
        cScale.y / std::max(pScale.y, Ogre::Real(1e-8)),
        cScale.z / std::max(pScale.z, Ogre::Real(1e-8)));
}

struct RestPoseTRS {
    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    Ogre::Quaternion orientation = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3 scale = Ogre::Vector3::UNIT_SCALE;
};

struct ImportedRestCache {
    std::vector<std::pair<std::string, RestPoseTRS>> bones;
    std::vector<SkeletonEditor::Snapshot::BindVertexBuffer> bindVertexBuffers;
};

// Keyed by mesh name so skeleton resource rebuilds keep the original import.
std::unordered_map<std::string, ImportedRestCache>& importedRestCaches()
{
    static std::unordered_map<std::string, ImportedRestCache> caches;
    return caches;
}

std::string restCacheKey(Ogre::Entity* entity)
{
    if (!entity || !entity->getMesh()) return {};
    return entity->getMesh()->getName();
}

void readBindVertexBuffer(const Ogre::VertexData* vd,
                          SkeletonEditor::Snapshot::BindVertexBuffer& out)
{
    out.positions.clear();
    out.normals.clear();
    if (!vd || vd->vertexCount == 0) return;

    const size_t n = vd->vertexCount;
    out.positions.resize(n * 3, 0.f);

    const auto* posElem = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (posElem) {
        auto vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
        auto* base = static_cast<unsigned char*>(
            vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t i = 0; i < n; ++i) {
            float* p = nullptr;
            posElem->baseVertexPointerToElement(base + i * vbuf->getVertexSize(), &p);
            out.positions[i * 3 + 0] = p[0];
            out.positions[i * 3 + 1] = p[1];
            out.positions[i * 3 + 2] = p[2];
        }
        vbuf->unlock();
    }

    const auto* normElem = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
    if (normElem) {
        out.normals.resize(n * 3, 0.f);
        auto vbuf = vd->vertexBufferBinding->getBuffer(normElem->getSource());
        auto* base = static_cast<unsigned char*>(
            vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t i = 0; i < n; ++i) {
            float* p = nullptr;
            normElem->baseVertexPointerToElement(base + i * vbuf->getVertexSize(), &p);
            out.normals[i * 3 + 0] = p[0];
            out.normals[i * 3 + 1] = p[1];
            out.normals[i * 3 + 2] = p[2];
        }
        vbuf->unlock();
    }
}

void writeBindVertexBuffer(Ogre::VertexData* vd,
                           const SkeletonEditor::Snapshot::BindVertexBuffer& in)
{
    if (!vd || in.positions.size() < vd->vertexCount * 3) return;
    const size_t n = vd->vertexCount;

    const auto* posElem = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (posElem) {
        auto vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
        auto* base = static_cast<unsigned char*>(
            vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));
        for (size_t i = 0; i < n; ++i) {
            float* p = nullptr;
            posElem->baseVertexPointerToElement(base + i * vbuf->getVertexSize(), &p);
            p[0] = in.positions[i * 3 + 0];
            p[1] = in.positions[i * 3 + 1];
            p[2] = in.positions[i * 3 + 2];
        }
        vbuf->unlock();
    }

    const auto* normElem = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
    if (normElem && in.normals.size() >= n * 3) {
        auto vbuf = vd->vertexBufferBinding->getBuffer(normElem->getSource());
        auto* base = static_cast<unsigned char*>(
            vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));
        for (size_t i = 0; i < n; ++i) {
            float* p = nullptr;
            normElem->baseVertexPointerToElement(base + i * vbuf->getVertexSize(), &p);
            p[0] = in.normals[i * 3 + 0];
            p[1] = in.normals[i * 3 + 1];
            p[2] = in.normals[i * 3 + 2];
        }
        vbuf->unlock();
    }
}

std::vector<SkeletonEditor::Snapshot::BindVertexBuffer> captureMeshBindVertices(Ogre::Mesh* mesh)
{
    std::vector<SkeletonEditor::Snapshot::BindVertexBuffer> out;
    if (!mesh) return out;
    if (mesh->sharedVertexData) {
        SkeletonEditor::Snapshot::BindVertexBuffer b;
        b.submeshIndex = -1;
        readBindVertexBuffer(mesh->sharedVertexData, b);
        out.push_back(std::move(b));
    }
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub || sub->useSharedVertices || !sub->vertexData) continue;
        SkeletonEditor::Snapshot::BindVertexBuffer b;
        b.submeshIndex = static_cast<int>(si);
        readBindVertexBuffer(sub->vertexData, b);
        out.push_back(std::move(b));
    }
    return out;
}

void restoreMeshBindVertices(Ogre::Mesh* mesh,
                             const std::vector<SkeletonEditor::Snapshot::BindVertexBuffer>& buffers)
{
    if (!mesh) return;
    Ogre::AxisAlignedBox box;
    bool any = false;
    for (const auto& b : buffers) {
        Ogre::VertexData* vd = nullptr;
        if (b.submeshIndex < 0)
            vd = mesh->sharedVertexData;
        else if (static_cast<unsigned short>(b.submeshIndex) < mesh->getNumSubMeshes()) {
            Ogre::SubMesh* sub = mesh->getSubMesh(static_cast<unsigned short>(b.submeshIndex));
            if (sub && !sub->useSharedVertices)
                vd = sub->vertexData;
        }
        if (!vd) continue;
        writeBindVertexBuffer(vd, b);
        for (size_t i = 0; i + 2 < b.positions.size(); i += 3) {
            box.merge(Ogre::Vector3(b.positions[i], b.positions[i + 1], b.positions[i + 2]));
            any = true;
        }
    }
    if (any) {
        mesh->_setBounds(box, false);
        mesh->_setBoundingSphereRadius(
            std::max(box.getMaximum().length(), box.getMinimum().length()));
    }
}

/// Copy software-skinned positions/normals into the mesh bind buffers so the
/// visible posed shape becomes the new rest geometry. Required because LBS at
/// the binding pose is identity — changing bone initials alone always shows
/// the authored T-pose verts again.
///
/// Caller must: disable animation states, reset non-target bones to the current
/// bind, and pose target bones with setManuallyControlled(true). This function
/// does not re-enable clips (setBindingPose must run while they stay muted).
bool bakeSkinnedPoseIntoBindMesh(Ogre::Entity* entity, QString* error)
{
    if (!entity || !entity->hasSkeleton() || !entity->getMesh()) {
        if (error) *error = QStringLiteral("No skinned entity to bake");
        return false;
    }

    Ogre::Mesh* mesh = entity->getMesh().get();

    if (Ogre::SkeletonInstance* inst = entity->getSkeleton()) {
        for (Ogre::Bone* root : inst->getRootBones())
            root->_update(true, true);
    }

    // Skip setAnimationState inside cacheBoneMatrices — a reset() there would
    // fight our manually posed bones if the dirty-frame gate re-enters it.
    const bool prevSkip = entity->getSkipAnimationStateUpdate();
    entity->setSkipAnimationStateUpdate(true);
    entity->addSoftwareAnimationRequest(true);
    entity->_updateAnimation();

    auto copySkinnedToBind = [](Ogre::VertexData* bind, const Ogre::VertexData* skinned) -> bool {
        if (!bind || !skinned || bind->vertexCount != skinned->vertexCount)
            return false;
        SkeletonEditor::Snapshot::BindVertexBuffer tmp;
        readBindVertexBuffer(skinned, tmp);
        writeBindVertexBuffer(bind, tmp);
        return true;
    };

    bool ok = true;
    bool any = false;
    if (mesh->sharedVertexData) {
        any = true;
        Ogre::VertexData* skinned = entity->_getSkelAnimVertexData();
        if (!copySkinnedToBind(mesh->sharedVertexData, skinned))
            ok = false;
    }
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub || sub->useSharedVertices || !sub->vertexData) continue;
        any = true;
        Ogre::VertexData* skinned = entity->getSubEntity(si)->_getSkelAnimVertexData();
        if (!copySkinnedToBind(sub->vertexData, skinned))
            ok = false;
    }

    entity->removeSoftwareAnimationRequest(true);
    entity->setSkipAnimationStateUpdate(prevSkip);

    if (!any || !ok) {
        if (error) *error = QStringLiteral("Failed to read software-skinned vertices");
        return false;
    }

    // Refresh AABB from the newly baked bind verts.
    // Do NOT call mesh->_dirtyState(): that bumps getStateCount() and forces
    // Entity::_initialise(true) on the next frame, which destroys TagPoints and
    // makes the Skeleton overlay disappear while the checkbox stays checked.
    restoreMeshBindVertices(mesh, captureMeshBindVertices(mesh));
    return true;
}

void rebakeTrackKeyframes(Ogre::NodeAnimationTrack* track,
                          const RestPoseTRS& oldRest,
                          const RestPoseTRS& newRest)
{
    if (!track) return;
    for (unsigned short k = 0; k < track->getNumKeyFrames(); ++k) {
        auto* kf = track->getNodeKeyFrame(k);
        const Ogre::Vector3 posePos = oldRest.position + kf->getTranslate();
        const Ogre::Quaternion poseOri = oldRest.orientation * kf->getRotation();
        const Ogre::Vector3 poseScl(
            oldRest.scale.x * kf->getScale().x,
            oldRest.scale.y * kf->getScale().y,
            oldRest.scale.z * kf->getScale().z);

        kf->setTranslate(posePos - newRest.position);
        kf->setRotation(newRest.orientation.Inverse() * poseOri);
        kf->setScale(Ogre::Vector3(
            poseScl.x / std::max(newRest.scale.x, Ogre::Real(1e-8)),
            poseScl.y / std::max(newRest.scale.y, Ogre::Real(1e-8)),
            poseScl.z / std::max(newRest.scale.z, Ogre::Real(1e-8))));
    }
}

void rebakeAnimationsForBone(Ogre::Skeleton* skel,
                             const std::string& boneName,
                             const RestPoseTRS& oldRest,
                             const RestPoseTRS& newRest)
{
    if (!skel || !skel->hasBone(boneName)) return;
    const unsigned short boneHandle = skel->getBone(boneName)->getHandle();
    for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
        Ogre::Animation* anim = skel->getAnimation(ai);
        if (!anim) continue;
        for (const auto& [handle, track] : anim->_getNodeTrackList()) {
            if (!track) continue;
            Ogre::Node* node = track->getAssociatedNode();
            const bool nameMatch = node && node->getName() == boneName;
            const bool handleMatch = handle == boneHandle;
            if (!nameMatch && !handleMatch) continue;
            rebakeTrackKeyframes(track, oldRest, newRest);
        }
    }
}

bool applyRestPoseMap(Ogre::Entity* entity,
                      const std::unordered_map<std::string, RestPoseTRS>& newRests,
                      QString* error,
                      bool rebakeAnimations,
                      bool bakeMesh)
{
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel || newRests.empty()) {
        if (error) *error = QStringLiteral("No skeleton or bones to update");
        return false;
    }

    // Validate every target bone exists before mutating any tracks/bind.
    for (const auto& [name, newRest] : newRests) {
        Q_UNUSED(newRest);
        if (!skel->hasBone(name)) {
            if (error) *error = QStringLiteral("Bone not found: %1").arg(QString::fromStdString(name));
            return false;
        }
    }

    // Mute clips for the whole bind write. Re-enabling mid-bake used to let
    // setAnimationState()/setBindingPose capture mid-clip locals as the new
    // bind, and the mesh bake never stuck visually.
    struct AnimMute {
        Ogre::Entity* ent = nullptr;
        std::vector<std::pair<std::string, bool>> saved;
        explicit AnimMute(Ogre::Entity* e) : ent(e)
        {
            if (!ent) return;
            if (Ogre::AnimationStateSet* states = ent->getAllAnimationStates()) {
                for (const auto& pair : states->getAnimationStates()) {
                    saved.push_back({pair.first, pair.second->getEnabled()});
                    pair.second->setEnabled(false);
                }
            }
        }
        ~AnimMute()
        {
            if (!ent) return;
            if (Ogre::AnimationStateSet* states = ent->getAllAnimationStates()) {
                for (const auto& [name, enabled] : saved) {
                    if (!states->hasAnimationState(name)) continue;
                    states->getAnimationState(name)->setEnabled(enabled);
                }
            }
        }
    } animMute(entity);

    // Pose from a clean bind: reset every bone, then apply only the new rests.
    // Baking on top of a mid-clip body pose would freeze that clip into the mesh.
    if (Ogre::SkeletonInstance* inst = entity->getSkeleton()) {
        for (unsigned short i = 0; i < inst->getNumBones(); ++i)
            inst->getBone(i)->setManuallyControlled(false);
        inst->reset(true);
        for (const auto& [name, newRest] : newRests) {
            if (!inst->hasBone(name)) continue;
            Ogre::Bone* ib = inst->getBone(name);
            ib->setManuallyControlled(true);
            ib->setPosition(newRest.position);
            ib->setOrientation(newRest.orientation);
            ib->setScale(newRest.scale);
        }
        for (Ogre::Bone* root : inst->getRootBones())
            root->_update(true, true);
    }

    // LBS at the binding pose is identity — without baking the posed
    // skinned verts into the mesh, changing bone initials alone always
    // shows the authored T-pose geometry again (bones look posed, mesh
    // snaps back). Capture/gizmo commits bake; reset restores cached verts.
    if (bakeMesh) {
        QString bakeErr;
        if (!bakeSkinnedPoseIntoBindMesh(entity, &bakeErr)) {
            if (error) *error = bakeErr;
            return false;
        }
    }

    for (const auto& [name, newRest] : newRests) {
        Ogre::Bone* bone = skel->getBone(name);
        RestPoseTRS oldRest{bone->getInitialPosition(), bone->getInitialOrientation(),
                            bone->getInitialScale()};
        // Capture Rest / Reset: rebake so clip world motion is unchanged.
        // Gizmo CommitBind: do NOT rebake — preserving absolute poses would
        // immediately undo the user's rest edit once the clip is re-applied.
        if (rebakeAnimations)
            rebakeAnimationsForBone(skel.get(), name, oldRest, newRest);
        bone->setPosition(newRest.position);
        bone->setOrientation(newRest.orientation);
        bone->setScale(newRest.scale);
        bone->setInitialState();
    }
    skel->setBindingPose();

    // Sync the LIVE SkeletonInstance in place. Do NOT call entity->_initialise(true):
    // that destroys TagPoints and leaves SkeletonDebug joint entities floating.
    // Copy EVERY bone's new initial from the resource so setBindingPose does not
    // freeze leftover clip locals into inverse-bind matrices.
    if (Ogre::SkeletonInstance* inst = entity->getSkeleton()) {
        for (unsigned short i = 0; i < inst->getNumBones(); ++i) {
            Ogre::Bone* ib = inst->getBone(i);
            if (!skel->hasBone(ib->getName())) continue;
            Ogre::Bone* rb = skel->getBone(ib->getName());
            ib->setManuallyControlled(true);
            ib->setPosition(rb->getInitialPosition());
            ib->setOrientation(rb->getInitialOrientation());
            ib->setScale(rb->getInitialScale());
        }
        for (Ogre::Bone* root : inst->getRootBones())
            root->_update(true, true);
        inst->setBindingPose();
        for (unsigned short i = 0; i < inst->getNumBones(); ++i)
            inst->getBone(i)->setManuallyControlled(false);
        inst->reset(true);
        for (Ogre::Bone* root : inst->getRootBones())
            root->_update(true, true);
    }

    entity->_updateAnimation();
    return true;
}

} // namespace

SkeletonEditor* SkeletonEditor::s_singleton = nullptr;

SkeletonEditor* SkeletonEditor::getSingleton()
{
    if (!s_singleton)
        s_singleton = new SkeletonEditor();
    return s_singleton;
}

SkeletonEditor* SkeletonEditor::getSingletonPtr() { return s_singleton; }

SkeletonEditor* SkeletonEditor::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = getSingleton();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void SkeletonEditor::kill()
{
    if (s_singleton) {
        if (s_singleton->m_editRestPoseMode)
            s_singleton->setEditRestPoseMode(false);
        s_singleton->setShowRestPoseGhost(false);
    }
    delete s_singleton;
    s_singleton = nullptr;
    clearImportedRestCache();
}

SkeletonEditor::SkeletonEditor(QObject* parent) : QObject(parent) {}

void SkeletonEditor::ensureEditRestSelectionHook()
{
    if (m_editRestSelectionHooked) return;
    auto* sel = SelectionSet::getSingletonPtr();
    if (!sel) return;
    connect(sel, &SelectionSet::selectionChanged, this, [this]() {
        if (!m_editRestPoseMode) return;
        // Remute for the newly selected entity; unmute the previous one.
        applyEditRestAnimMute(false);
        applyEditRestAnimMute(true);
    });
    m_editRestSelectionHooked = true;
}

Ogre::Entity* SkeletonEditor::selectedSkinnedEntity()
{
    auto* sel = SelectionSet::getSingletonPtr();
    if (!sel) return nullptr;
    for (Ogre::Entity* ent : sel->getResolvedEntities()) {
        if (ent && ent->hasSkeleton()) return ent;
    }
    return nullptr;
}

QString SkeletonEditor::uniqueBoneName(const Ogre::Skeleton* skel, const QString& base)
{
    if (!skel) return base;
    const std::string baseStd = base.toStdString();
    if (!skel->hasBone(baseStd)) return base;
    for (int i = 1; i < 10000; ++i) {
        const QString candidate = QStringLiteral("%1.%2").arg(base).arg(i, 3, 10, QChar('0'));
        if (!skel->hasBone(candidate.toStdString())) return candidate;
    }
    return base + QStringLiteral(".001");
}

unsigned short SkeletonEditor::nextBoneHandle(const Ogre::Skeleton* skel)
{
    unsigned short maxHandle = 0;
    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        const unsigned short h = skel->getBone(i)->getHandle();
        if (h >= maxHandle) maxHandle = static_cast<unsigned short>(h + 1);
    }
    return maxHandle;
}

Ogre::Vector3 SkeletonEditor::defaultChildLocalPosition(const Ogre::Bone* parent)
{
    if (!parent) return Ogre::Vector3(0, 0.1f, 0);
    const auto& children = parent->getChildren();
    if (!children.empty()) {
        Ogre::Vector3 sum = Ogre::Vector3::ZERO;
        for (auto* child : children)
            sum += static_cast<Ogre::Bone*>(child)->getPosition();
        return sum / static_cast<Ogre::Real>(children.size());
    }
    return Ogre::Vector3(0, 0.1f, 0);
}

SkeletonEditor::Snapshot SkeletonEditor::captureSnapshot(Ogre::Entity* entity)
{
    Snapshot snap;
    if (!entity || !entity->getMesh() || !entity->getMesh()->hasSkeleton())
        return snap;

    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    Ogre::Mesh* mesh = entity->getMesh().get();
    snap.skeletonName = skel->getName();
    snap.meshName = mesh->getName();

    auto boneIt = skel->getBoneIterator();
    while (boneIt.hasMoreElements()) {
        Ogre::Bone* bone = boneIt.getNext();
        Snapshot::BoneData bd;
        bd.name = bone->getName();
        bd.handle = bone->getHandle();
        if (bone->getParent())
            bd.parentName = bone->getParent()->getName();
        bd.position = bone->getPosition();
        bd.orientation = bone->getOrientation();
        bd.scale = bone->getScale();
        bd.initialPosition = bone->getInitialPosition();
        bd.initialOrientation = bone->getInitialOrientation();
        bd.initialScale = bone->getInitialScale();
        snap.bones.push_back(std::move(bd));
    }

    for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
        Ogre::Animation* anim = skel->getAnimation(ai);
        Snapshot::AnimationData ad;
        ad.name = anim->getName();
        ad.length = anim->getLength();
        ad.interpolationMode = anim->getInterpolationMode();
        ad.rotationInterpolationMode = anim->getRotationInterpolationMode();
        for (const auto& [handle, track] : anim->_getNodeTrackList()) {
            Snapshot::TrackData td;
            td.handle = handle;
            if (track->getAssociatedNode())
                td.boneName = track->getAssociatedNode()->getName();
            td.useShortestRotationPath = track->getUseShortestRotationPath();
            for (unsigned short k = 0; k < track->getNumKeyFrames(); ++k) {
                const auto* kf = track->getNodeKeyFrame(k);
                td.keyframes.push_back({kf->getTime(), kf->getTranslate(), kf->getRotation(), kf->getScale()});
            }
            ad.tracks.push_back(std::move(td));
        }
        snap.animations.push_back(std::move(ad));
    }

    gatherBoneAssignments(mesh, snap);
    snap.bindVertexBuffers = captureMeshBindVertices(mesh);
    return snap;
}

bool SkeletonEditor::restoreSnapshot(Ogre::Entity* entity, const Snapshot& snapshot, QString* error)
{
    if (!entity || !entity->getMesh()) {
        if (error) *error = QStringLiteral("No entity/mesh to restore");
        return false;
    }
    if (snapshot.bones.empty()) {
        if (error) *error = QStringLiteral("Empty skeleton snapshot");
        return false;
    }

    auto& skelMgr = Ogre::SkeletonManager::getSingleton();
    const std::string skelName = snapshot.skeletonName.empty()
        ? entity->getMesh()->getName() + "_skel_restore"
        : snapshot.skeletonName;

    if (skelMgr.resourceExists(skelName))
        skelMgr.remove(skelName);

    Ogre::SkeletonPtr skel = skelMgr.create(
        skelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    std::unordered_map<std::string, Ogre::Bone*> bonesByName;
    for (const auto& bd : snapshot.bones) {
        Ogre::Bone* bone = skel->createBone(bd.name, bd.handle);
        bone->setPosition(bd.initialPosition);
        bone->setOrientation(bd.initialOrientation);
        bone->setScale(bd.initialScale);
        bonesByName[bd.name] = bone;
    }
    for (const auto& bd : snapshot.bones) {
        if (bd.parentName.empty()) continue;
        auto pit = bonesByName.find(bd.parentName);
        auto bit = bonesByName.find(bd.name);
        if (pit != bonesByName.end() && bit != bonesByName.end())
            pit->second->addChild(bit->second);
    }
    for (const auto& bd : snapshot.bones) {
        auto it = bonesByName.find(bd.name);
        if (it == bonesByName.end()) continue;
        it->second->setInitialState();
    }
    skel->setBindingPose();

    for (const auto& bd : snapshot.bones) {
        auto it = bonesByName.find(bd.name);
        if (it == bonesByName.end()) continue;
        it->second->setPosition(bd.position);
        it->second->setOrientation(bd.orientation);
        it->second->setScale(bd.scale);
    }

    for (const auto& ad : snapshot.animations) {
        if (skel->hasAnimation(ad.name)) continue;
        Ogre::Animation* anim = skel->createAnimation(ad.name, ad.length);
        anim->setInterpolationMode(ad.interpolationMode);
        anim->setRotationInterpolationMode(ad.rotationInterpolationMode);
        for (const auto& td : ad.tracks) {
            auto* track = anim->createNodeTrack(td.handle);
            try {
                if (!td.boneName.empty() && skel->hasBone(td.boneName))
                    track->setAssociatedNode(skel->getBone(td.boneName));
                else
                    track->setAssociatedNode(skel->getBone(td.handle));
            } catch (...) {
            }
            track->setUseShortestRotationPath(td.useShortestRotationPath);
            for (const auto& kf : td.keyframes) {
                auto* dst = track->createNodeKeyFrame(kf.time);
                dst->setTranslate(kf.translate);
                dst->setRotation(kf.rotation);
                dst->setScale(kf.scale);
            }
        }
    }
    skel->_fireLoadingComplete();

    Ogre::Mesh* mesh = entity->getMesh().get();
    mesh->_notifySkeleton(skel);
    applyBoneAssignments(mesh, snapshot);
    if (!snapshot.bindVertexBuffers.empty())
        restoreMeshBindVertices(mesh, snapshot.bindVertexBuffers);
    entity->_initialise(true);
    return true;
}

SkeletonEditor::Result SkeletonEditor::createBone(Ogre::Entity* entity, const CreateOptions& opts)
{
    Result result;
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel) {
        result.error = QStringLiteral("Entity has no skeleton");
        return result;
    }

    Ogre::Bone* parent = nullptr;
    if (!opts.parentBoneName.isEmpty()) {
        if (!skel->hasBone(opts.parentBoneName.toStdString())) {
            result.error = QStringLiteral("Parent bone not found: %1").arg(opts.parentBoneName);
            return result;
        }
        parent = skel->getBone(opts.parentBoneName.toStdString());
    } else {
        const auto roots = skel->getRootBones();
        if (!roots.empty()) parent = roots.front();
    }

    const QString newName = [&]() {
        if (!opts.forcedName.isEmpty() && !skel->hasBone(opts.forcedName.toStdString()))
            return opts.forcedName;
        return uniqueBoneName(skel.get(), opts.baseName);
    }();
    const unsigned short handle = nextBoneHandle(skel.get());
    Ogre::Bone* bone = skel->createBone(newName.toStdString(), handle);
    if (parent)
        parent->addChild(bone);
    bone->setPosition(defaultChildLocalPosition(parent));
    bone->setOrientation(Ogre::Quaternion::IDENTITY);
    bone->setScale(Ogre::Vector3::UNIT_SCALE);
    bone->setInitialState();

    entity->_initialise(true);

    result.ok = true;
    result.boneName = newName;
    return result;
}

bool SkeletonEditor::rebuildSkeletonWithoutBones(Ogre::Entity* entity,
                                                 const std::vector<std::string>& removeNames,
                                                 bool removeChildren,
                                                 bool transferWeightsToParent,
                                                 QString* error)
{
    Ogre::SkeletonPtr oldSkel = skeletonResource(entity);
    if (!oldSkel || removeNames.empty()) {
        if (error) *error = QStringLiteral("Nothing to remove");
        return false;
    }

    std::unordered_set<std::string> removeSet;
    for (const auto& name : removeNames) {
        if (!oldSkel->hasBone(name)) continue;
        if (removeChildren) {
            auto desc = collectDescendants(oldSkel.get(), name);
            removeSet.insert(desc.begin(), desc.end());
        } else {
            removeSet.insert(name);
        }
    }
    if (removeSet.empty()) {
        if (error) *error = QStringLiteral("Bone not found");
        return false;
    }

    Snapshot snap = captureSnapshot(entity);

    std::unordered_map<std::string, std::string> parentByName;
    std::unordered_map<std::string, unsigned short> handleByName;
    std::unordered_map<unsigned short, unsigned short> parentByHandle;
    for (const auto& bd : snap.bones) {
        parentByName[bd.name] = bd.parentName;
        handleByName[bd.name] = bd.handle;
        if (!bd.parentName.empty() && handleByName.count(bd.parentName))
            parentByHandle[bd.handle] = handleByName[bd.parentName];
    }

    Snapshot filtered;
    filtered.skeletonName = snap.skeletonName;
    filtered.meshName = snap.meshName;
    filtered.submeshAssignments = snap.submeshAssignments;
    filtered.bindVertexBuffers = snap.bindVertexBuffers;

    std::unordered_map<unsigned short, unsigned short> oldToNew;
    unsigned short nextHandle = 0;
    for (const auto& bd : snap.bones) {
        if (removeSet.count(bd.name)) continue;
        Snapshot::BoneData kept = bd;
        if (!kept.parentName.empty() && removeSet.count(kept.parentName)) {
            std::string parent = kept.parentName;
            while (!parent.empty() && removeSet.count(parent))
                parent = parentByName[parent];
            kept.parentName = parent;
        }
        oldToNew[bd.handle] = nextHandle;
        kept.handle = nextHandle++;
        filtered.bones.push_back(std::move(kept));
    }

    if (filtered.bones.empty()) {
        if (error) *error = QStringLiteral("Cannot remove every bone in the skeleton");
        return false;
    }

    for (const auto& ad : snap.animations) {
        Snapshot::AnimationData fad;
        fad.name = ad.name;
        fad.length = ad.length;
        fad.interpolationMode = ad.interpolationMode;
        fad.rotationInterpolationMode = ad.rotationInterpolationMode;
        for (const auto& td : ad.tracks) {
            auto it = oldToNew.find(td.handle);
            if (it == oldToNew.end()) continue;
            Snapshot::TrackData ktd = td;
            ktd.handle = it->second;
            fad.tracks.push_back(std::move(ktd));
        }
        filtered.animations.push_back(std::move(fad));
    }

    std::unordered_set<unsigned short> removedHandles;
    for (const auto& bd : snap.bones) {
        if (removeSet.count(bd.name)) removedHandles.insert(bd.handle);
    }

    auto remapAssignments = [&](std::vector<Ogre::VertexBoneAssignment>& assignments) {
        std::map<unsigned int, std::map<unsigned short, float>> perVertex;
        for (const auto& vba : assignments) {
            unsigned short target = vba.boneIndex;
            if (removedHandles.count(target)) {
                if (!transferWeightsToParent) continue;
                target = nearestKeptAncestor(target, parentByHandle, removedHandles, oldToNew);
                if (target == std::numeric_limits<unsigned short>::max()) continue;
            } else {
                auto it = oldToNew.find(target);
                if (it == oldToNew.end()) continue;
                target = it->second;
            }
            perVertex[vba.vertexIndex][target] += vba.weight;
        }
        assignments.clear();
        for (const auto& [vert, influences] : perVertex) {
            float sum = 0.f;
            for (const auto& [h, w] : influences) sum += w;
            if (sum <= 1e-8f) continue;
            for (const auto& [h, w] : influences) {
                Ogre::VertexBoneAssignment vba;
                vba.vertexIndex = vert;
                vba.boneIndex = h;
                vba.weight = w / sum;
                assignments.push_back(vba);
            }
        }
    };

    filtered.meshAssignments = snap.meshAssignments;
    remapAssignments(filtered.meshAssignments);
    for (auto& subAssign : filtered.submeshAssignments) {
        if (!subAssign.empty()) remapAssignments(subAssign);
    }

    std::unordered_map<std::string, std::string> originalParent;
    for (const auto& bd : snap.bones)
        originalParent[bd.name] = bd.parentName;

    auto reparentLocals = [&](bool bindingPose) {
        if (bindingPose)
            oldSkel->setBindingPose();
        for (auto& kept : filtered.bones) {
            const auto pit = originalParent.find(kept.name);
            if (pit == originalParent.end() || pit->second == kept.parentName)
                continue;
            if (kept.parentName.empty()) continue;
            if (!oldSkel->hasBone(kept.name) || !oldSkel->hasBone(kept.parentName))
                continue;
            Ogre::Bone* bone = oldSkel->getBone(kept.name);
            Ogre::Bone* newParent = oldSkel->getBone(kept.parentName);
            Ogre::Vector3& pos = bindingPose ? kept.initialPosition : kept.position;
            Ogre::Quaternion& rot = bindingPose ? kept.initialOrientation : kept.orientation;
            Ogre::Vector3& scale = bindingPose ? kept.initialScale : kept.scale;
            worldToNewParentLocal(bone, newParent, pos, rot, scale);
        }
    };
    reparentLocals(false);
    reparentLocals(true);

    return restoreSnapshot(entity, filtered, error);
}

SkeletonEditor::Result SkeletonEditor::removeBone(Ogre::Entity* entity,
                                                  const QString& boneName,
                                                  const RemoveOptions& opts)
{
    Result result;
    if (!entity || boneName.isEmpty()) {
        result.error = QStringLiteral("Invalid entity or bone name");
        return result;
    }
    QString err;
    if (!rebuildSkeletonWithoutBones(entity, {boneName.toStdString()}, opts.removeChildren,
                                     opts.transferWeightsToParent, &err)) {
        result.error = err;
        return result;
    }
    result.ok = true;
    result.boneName = boneName;
    return result;
}

SkeletonEditor::Result SkeletonEditor::renameBone(Ogre::Entity* entity,
                                                    const QString& oldName,
                                                    const QString& newName)
{
    Result result;
    if (!entity || oldName.isEmpty() || newName.isEmpty() || oldName == newName) {
        result.error = QStringLiteral("Invalid bone name");
        return result;
    }
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel) {
        result.error = QStringLiteral("Entity has no skeleton");
        return result;
    }
    if (!skel->hasBone(oldName.toStdString())) {
        result.error = QStringLiteral("Bone not found: %1").arg(oldName);
        return result;
    }
    if (skel->hasBone(newName.toStdString())) {
        result.error = QStringLiteral("Bone already exists: %1").arg(newName);
        return result;
    }

    Snapshot snap = captureSnapshot(entity);
    const std::string old = oldName.toStdString();
    const std::string neu = newName.toStdString();
    bool found = false;
    for (auto& bd : snap.bones) {
        if (bd.name == old) {
            bd.name = neu;
            found = true;
        }
        if (bd.parentName == old)
            bd.parentName = neu;
    }
    for (auto& ad : snap.animations) {
        for (auto& td : ad.tracks) {
            if (td.boneName == old)
                td.boneName = neu;
        }
    }
    if (!found) {
        result.error = QStringLiteral("Bone not found in snapshot");
        return result;
    }

    QString err;
    if (!restoreSnapshot(entity, snap, &err)) {
        result.error = err;
        return result;
    }

    result.ok = true;
    result.boneName = newName;
    return result;
}

SkeletonEditor::Result SkeletonEditor::duplicateBone(Ogre::Entity* entity,
                                                     const QString& sourceBoneName)
{
    Result result;
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel || sourceBoneName.isEmpty()) {
        result.error = QStringLiteral("Invalid entity or bone name");
        return result;
    }
    if (!skel->hasBone(sourceBoneName.toStdString())) {
        result.error = QStringLiteral("Bone not found: %1").arg(sourceBoneName);
        return result;
    }

    Ogre::Bone* src = skel->getBone(sourceBoneName.toStdString());
    CreateOptions opts;
    opts.baseName = uniqueBoneName(skel.get(), sourceBoneName);
    if (src->getParent())
        opts.parentBoneName = QString::fromStdString(src->getParent()->getName());

    result = createBone(entity, opts);
    if (!result.ok) return result;

    skel = skeletonResource(entity);
    if (!skel || !skel->hasBone(result.boneName.toStdString())) {
        result.ok = false;
        result.error = QStringLiteral("Duplicate bone missing after create");
        return result;
    }
    src = skel->getBone(sourceBoneName.toStdString());
    Ogre::Bone* dup = skel->getBone(result.boneName.toStdString());
    dup->setPosition(src->getPosition());
    dup->setOrientation(src->getOrientation());
    dup->setScale(src->getScale());
    dup->setInitialState();
    entity->_initialise(true);
    return result;
}

void SkeletonEditor::refreshAfterEdit(const std::string& entityName, const QString& selectBone)
{
    if (auto* anim = AnimationControlController::instance()) {
        const QString ent = QString::fromStdString(entityName);
        if (anim->selectedEntityName() == ent)
            anim->rebindSelectedSkeleton();
        else
            anim->bindSkeletonForEntity(ent);
        anim->refreshBoneList(selectBone);
    }
    if (auto* ppc = PropertiesPanelController::instance())
        ppc->refreshSkeletonOverlays(QString::fromStdString(entityName));
    if (auto* editor = getSingletonPtr())
        emit editor->skeletonStructureChanged();
}

bool SkeletonEditor::ensureEntitySkeleton(Ogre::Entity* entity, QString* error)
{
    if (!entity || !entity->getMesh()) {
        if (error) *error = QStringLiteral("No entity/mesh");
        return false;
    }
    if (entity->getMesh()->hasSkeleton() && entity->getMesh()->getSkeleton())
        return true;

    auto& skelMgr = Ogre::SkeletonManager::getSingleton();
    const std::string skelName = entity->getMesh()->getName() + "_skel";
    if (skelMgr.resourceExists(skelName))
        skelMgr.remove(skelName);
    Ogre::SkeletonPtr skel = skelMgr.create(
        skelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    Ogre::Bone* root = skel->createBone("Root", 0);
    root->setPosition(Ogre::Vector3::ZERO);
    root->setOrientation(Ogre::Quaternion::IDENTITY);
    root->setScale(Ogre::Vector3::UNIT_SCALE);
    root->setInitialState();
    skel->setBindingPose();
    skel->_fireLoadingComplete();
    entity->getMesh()->_notifySkeleton(skel);
    entity->_initialise(true);
    return true;
}

SkeletonEditor::Result SkeletonEditor::reparentBone(Ogre::Entity* entity,
                                                    const QString& boneName,
                                                    const QString& newParentName,
                                                    const ReparentOptions& opts)
{
    Result result;
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel || boneName.isEmpty()) {
        result.error = QStringLiteral("Invalid entity or bone name");
        return result;
    }
    const std::string boneStd = boneName.toStdString();
    if (!skel->hasBone(boneStd)) {
        result.error = QStringLiteral("Bone not found: %1").arg(boneName);
        return result;
    }
    if (!newParentName.isEmpty() && !skel->hasBone(newParentName.toStdString())) {
        result.error = QStringLiteral("Parent bone not found: %1").arg(newParentName);
        return result;
    }
    if (boneName == newParentName) {
        result.error = QStringLiteral("Cannot parent a bone to itself");
        return result;
    }
    if (!newParentName.isEmpty()) {
        const auto descendants = collectDescendants(skel.get(), boneStd);
        if (descendants.count(newParentName.toStdString())) {
            result.error = QStringLiteral("Cannot parent under a descendant (cycle)");
            return result;
        }
    }

    Ogre::Bone* bone = skel->getBone(boneStd);
    const std::string oldParent = bone->getParent() ? bone->getParent()->getName() : std::string{};
    const std::string newParentStd = newParentName.toStdString();
    if (oldParent == newParentStd) {
        result.ok = true;
        result.boneName = boneName;
        return result;
    }

    Snapshot snap = captureSnapshot(entity);
    Snapshot::BoneData* target = nullptr;
    for (auto& bd : snap.bones) {
        if (bd.name == boneStd) {
            target = &bd;
            break;
        }
    }
    if (!target) {
        result.error = QStringLiteral("Bone missing from snapshot");
        return result;
    }

    if (opts.keepWorld && !newParentStd.empty() && skel->hasBone(newParentStd)) {
        Ogre::Bone* newParent = skel->getBone(newParentStd);
        worldToNewParentLocal(bone, newParent,
                              target->position, target->orientation, target->scale);
        skel->setBindingPose();
        worldToNewParentLocal(bone, newParent,
                              target->initialPosition, target->initialOrientation, target->initialScale);
    } else if (opts.keepWorld && newParentStd.empty()) {
        // Detach to root: convert world → skeleton-root local (identity parent).
        target->position = bone->_getDerivedPosition();
        target->orientation = bone->_getDerivedOrientation();
        target->scale = bone->_getDerivedScale();
        skel->setBindingPose();
        target->initialPosition = bone->_getDerivedPosition();
        target->initialOrientation = bone->_getDerivedOrientation();
        target->initialScale = bone->_getDerivedScale();
    }
    // keep-local: leave TRS as captured; only parentName changes.

    target->parentName = newParentStd;

    QString err;
    if (!restoreSnapshot(entity, snap, &err)) {
        result.error = err;
        return result;
    }
    result.ok = true;
    result.boneName = boneName;
    return result;
}

SkeletonEditor::Result SkeletonEditor::detachBone(Ogre::Entity* entity, const QString& boneName)
{
    ReparentOptions opts;
    opts.keepWorld = true;
    return reparentBone(entity, boneName, {}, opts);
}

SkeletonEditor::Result SkeletonEditor::splitBone(Ogre::Entity* entity,
                                                 const QString& boneName,
                                                 float t)
{
    Result result;
    t = std::clamp(t, 0.05f, 0.95f);
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel || boneName.isEmpty()) {
        result.error = QStringLiteral("Invalid entity or bone name");
        return result;
    }
    const std::string boneStd = boneName.toStdString();
    if (!skel->hasBone(boneStd)) {
        result.error = QStringLiteral("Bone not found: %1").arg(boneName);
        return result;
    }

    Ogre::Bone* bone = skel->getBone(boneStd);
    Ogre::Vector3 axis = Ogre::Vector3::ZERO;
    const auto& children = bone->getChildren();
    if (!children.empty()) {
        for (auto* child : children)
            axis += static_cast<Ogre::Bone*>(child)->getInitialPosition();
        axis /= static_cast<Ogre::Real>(children.size());
    }
    if (axis.squaredLength() < 1e-10f)
        axis = Ogre::Vector3(0, 0.1f, 0);

    Snapshot snap = captureSnapshot(entity);
    const unsigned short oldHandle = bone->getHandle();
    const QString childName = uniqueBoneName(skel.get(), boneName + QStringLiteral("_split"));
    const unsigned short newHandle = nextBoneHandle(skel.get());

    Snapshot::BoneData splitBd;
    splitBd.name = childName.toStdString();
    splitBd.handle = newHandle;
    splitBd.parentName = boneStd;
    // Insert the new joint at fraction t along bone→tip (axis).
    splitBd.position = axis * t;
    splitBd.orientation = Ogre::Quaternion::IDENTITY;
    splitBd.scale = Ogre::Vector3::UNIT_SCALE;
    splitBd.initialPosition = splitBd.position;
    splitBd.initialOrientation = Ogre::Quaternion::IDENTITY;
    splitBd.initialScale = Ogre::Vector3::UNIT_SCALE;

    // Original bone keeps its parent; former children move under the split
    // joint and are re-expressed relative to that joint (world tip unchanged).
    for (auto& bd : snap.bones) {
        if (bd.parentName == boneStd) {
            bd.parentName = splitBd.name;
            bd.position = bd.position - axis * t;
            bd.initialPosition = bd.initialPosition - axis * t;
        }
    }
    snap.bones.push_back(splitBd);

    auto remapAssignments = [&](std::vector<Ogre::VertexBoneAssignment>& assignments) {
        std::vector<Ogre::VertexBoneAssignment> out;
        out.reserve(assignments.size() * 2);
        for (const auto& vba : assignments) {
            if (vba.boneIndex != oldHandle) {
                out.push_back(vba);
                continue;
            }
            Ogre::VertexBoneAssignment a = vba;
            a.weight = vba.weight * (1.f - t);
            Ogre::VertexBoneAssignment b = vba;
            b.boneIndex = newHandle;
            b.weight = vba.weight * t;
            if (a.weight > 1e-8f) out.push_back(a);
            if (b.weight > 1e-8f) out.push_back(b);
        }
        assignments.swap(out);
    };
    remapAssignments(snap.meshAssignments);
    for (auto& sub : snap.submeshAssignments)
        remapAssignments(sub);

    QString err;
    if (!restoreSnapshot(entity, snap, &err)) {
        result.error = err;
        return result;
    }
    result.ok = true;
    result.boneName = childName;
    return result;
}

bool SkeletonEditor::isBoneConnected(Ogre::Entity* entity, const QString& boneName)
{
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel || boneName.isEmpty() || !skel->hasBone(boneName.toStdString()))
        return false;
    Ogre::Bone* bone = skel->getBone(boneName.toStdString());
    Ogre::Bone* parent = static_cast<Ogre::Bone*>(bone->getParent());
    if (!parent) return true; // roots are trivially "connected"

    const Ogre::Vector3 tip = parentTipLocal(parent, bone);
    // Live head vs parent tip. Disconnect keeps bind (initial) at the tip and
    // only offsets position, so sole children remain detectable.
    const Ogre::Real gap = (bone->getPosition() - tip).length();
    return gap < kConnectEpsilon;
}

SkeletonEditor::Result SkeletonEditor::setBoneConnected(Ogre::Entity* entity,
                                                        const QString& boneName,
                                                        bool connected)
{
    Result result;
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel || boneName.isEmpty()) {
        result.error = QStringLiteral("Invalid entity or bone name");
        return result;
    }
    const std::string boneStd = boneName.toStdString();
    if (!skel->hasBone(boneStd)) {
        result.error = QStringLiteral("Bone not found: %1").arg(boneName);
        return result;
    }
    Ogre::Bone* bone = skel->getBone(boneStd);
    Ogre::Bone* parent = static_cast<Ogre::Bone*>(bone->getParent());
    if (!parent) {
        result.ok = true;
        result.boneName = boneName;
        return result;
    }

    Snapshot snap = captureSnapshot(entity);
    Snapshot::BoneData* target = nullptr;
    for (auto& bd : snap.bones) {
        if (bd.name == boneStd) {
            target = &bd;
            break;
        }
    }
    if (!target) {
        result.error = QStringLiteral("Bone missing from snapshot");
        return result;
    }

    const Ogre::Vector3 tip = parentTipLocal(parent, bone);
    Ogre::Vector3 dir = tip;
    if (dir.squaredLength() < 1e-10f)
        dir = Ogre::Vector3::UNIT_Y;
    else
        dir.normalise();

    if (connected) {
        target->position = tip;
        target->initialPosition = tip;
    } else {
        // Keep bind tip in initialPosition; offset only the live pose so
        // isBoneConnected can recover the tip for sole children.
        target->initialPosition = tip;
        target->position = tip + dir * kDisconnectGap;
    }

    QString err;
    if (!restoreSnapshot(entity, snap, &err)) {
        result.error = err;
        return result;
    }
    result.ok = true;
    result.boneName = boneName;
    return result;
}

SkeletonEditor::Result SkeletonEditor::attachBonesToEntity(Ogre::Entity* srcEntity,
                                                          const QStringList& boneNames,
                                                          Ogre::Entity* dstEntity,
                                                          const AttachOptions& opts)
{
    Q_UNUSED(opts);
    Result result;
    if (!srcEntity || !dstEntity || boneNames.isEmpty()) {
        result.error = QStringLiteral("Invalid attach arguments");
        return result;
    }
    if (srcEntity == dstEntity) {
        result.error = QStringLiteral("Source and destination must differ");
        return result;
    }
    Ogre::SkeletonPtr srcSkel = skeletonResource(srcEntity);
    if (!srcSkel) {
        result.error = QStringLiteral("Source has no skeleton");
        return result;
    }
    QString err;
    // Validate sources before mutating destination (ensureEntitySkeleton
    // may create a skeleton — don't leave that behind on a bad request).
    std::unordered_set<std::string> toCopy;
    for (const QString& name : boneNames) {
        if (!srcSkel->hasBone(name.toStdString())) {
            result.error = QStringLiteral("Source bone not found: %1").arg(name);
            return result;
        }
        auto desc = collectDescendants(srcSkel.get(), name.toStdString());
        toCopy.insert(desc.begin(), desc.end());
    }
    if (toCopy.empty()) {
        result.error = QStringLiteral("No bones to attach");
        return result;
    }

    if (!ensureEntitySkeleton(dstEntity, &err)) {
        result.error = err;
        return result;
    }

    Snapshot dstSnap = captureSnapshot(dstEntity);
    Ogre::SkeletonPtr dstSkel = skeletonResource(dstEntity);
    if (!dstSkel) {
        result.error = QStringLiteral("Destination skeleton missing after ensure");
        return result;
    }

    std::unordered_map<std::string, std::string> renameMap;
    QString firstNewName;
    unsigned short nextHandle = 0;
    for (const auto& bd : dstSnap.bones)
        nextHandle = std::max<unsigned short>(nextHandle, static_cast<unsigned short>(bd.handle + 1));

    // Parent-before-child order (handle order is not guaranteed hierarchical).
    std::vector<Ogre::Bone*> remaining;
    remaining.reserve(toCopy.size());
    for (const std::string& name : toCopy) {
        if (srcSkel->hasBone(name))
            remaining.push_back(srcSkel->getBone(name));
    }
    std::vector<Ogre::Bone*> ordered;
    ordered.reserve(remaining.size());
    std::unordered_set<std::string> placed;
    while (ordered.size() < remaining.size()) {
        bool progress = false;
        for (Ogre::Bone* b : remaining) {
            if (!b || placed.count(b->getName()))
                continue;
            Ogre::Node* parent = b->getParent();
            const bool parentOk = !parent
                || !toCopy.count(parent->getName())
                || placed.count(parent->getName());
            if (!parentOk)
                continue;
            ordered.push_back(b);
            placed.insert(b->getName());
            progress = true;
        }
        if (!progress)
            break; // Shouldn't happen for a tree; fall through with partial order.
    }
    for (Ogre::Bone* b : remaining) {
        if (b && !placed.count(b->getName()))
            ordered.push_back(b);
    }

    std::string dstRootParent;
    if (!dstSnap.bones.empty()) {
        // Attach under first root of destination.
        for (const auto& bd : dstSnap.bones) {
            if (bd.parentName.empty()) {
                dstRootParent = bd.name;
                break;
            }
        }
        if (dstRootParent.empty())
            dstRootParent = dstSnap.bones.front().name;
    }

    for (Ogre::Bone* srcBone : ordered) {
        const std::string srcName = srcBone->getName();
        QString unique = uniqueBoneName(dstSkel.get(), QString::fromStdString(srcName));
        // Also avoid collisions with bones we're about to add in this batch.
        while (true) {
            bool clash = false;
            for (const auto& kv : renameMap) {
                if (kv.second == unique.toStdString()) { clash = true; break; }
            }
            if (!clash) {
                for (const auto& bd : dstSnap.bones) {
                    if (bd.name == unique.toStdString()) { clash = true; break; }
                }
            }
            if (!clash) break;
            unique = uniqueBoneName(dstSkel.get(), unique + QStringLiteral("_"));
        }
        renameMap[srcName] = unique.toStdString();
        if (firstNewName.isEmpty())
            firstNewName = unique;

        Snapshot::BoneData bd;
        bd.name = unique.toStdString();
        bd.handle = nextHandle++;
        if (srcBone->getParent() && toCopy.count(srcBone->getParent()->getName())) {
            auto pit = renameMap.find(srcBone->getParent()->getName());
            bd.parentName = (pit != renameMap.end()) ? pit->second : dstRootParent;
        } else {
            bd.parentName = dstRootParent;
        }
        bd.position = srcBone->getPosition();
        bd.orientation = srcBone->getOrientation();
        bd.scale = srcBone->getScale();
        bd.initialPosition = srcBone->getInitialPosition();
        bd.initialOrientation = srcBone->getInitialOrientation();
        bd.initialScale = srcBone->getInitialScale();
        dstSnap.bones.push_back(bd);
    }

    if (!restoreSnapshot(dstEntity, dstSnap, &err)) {
        result.error = err;
        return result;
    }
    result.ok = true;
    result.boneName = firstNewName;
    return result;
}

bool SkeletonEditor::hasSkeletonSelection() const
{
    return selectedSkinnedEntity() != nullptr;
}

QString SkeletonEditor::selectedBoneName() const
{
    if (auto* anim = AnimationControlController::instance())
        return anim->selectedBone();
    return {};
}

QString SkeletonEditor::selectedBoneParentName() const
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return {};
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel) return {};
    const QString selected = selectedBoneName();
    if (selected.isEmpty() || !skel->hasBone(selected.toStdString()))
        return {};
    Ogre::Bone* bone = skel->getBone(selected.toStdString());
    Ogre::Node* parent = bone ? bone->getParent() : nullptr;
    if (!parent) return {};
    return QString::fromStdString(parent->getName());
}

QStringList SkeletonEditor::reparentCandidateParents() const
{
    QStringList out;
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return out;
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel) return out;
    const QString selected = selectedBoneName();
    std::unordered_set<std::string> blocked;
    if (!selected.isEmpty() && skel->hasBone(selected.toStdString()))
        blocked = collectDescendants(skel.get(), selected.toStdString());
    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        const std::string name = skel->getBone(i)->getName();
        if (blocked.count(name)) continue;
        out << QString::fromStdString(name);
    }
    return out;
}

QVariantList SkeletonEditor::attachTargetEntities() const
{
    QVariantList out;
    Ogre::Entity* src = selectedSkinnedEntity();
    const std::string srcName = src ? src->getName() : std::string{};
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return out;
    for (Ogre::Entity* ent : mgr->getEntities()) {
        if (!ent || ent->getMovableType() != "Entity") continue;
        if (ent->getName() == srcName) continue;
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), QString::fromStdString(ent->getName()));
        entry.insert(QStringLiteral("hasSkeleton"), ent->hasSkeleton());
        out.append(entry);
    }
    return out;
}

void SkeletonEditor::ensureImportedRestCache(Ogre::Entity* entity)
{
    const std::string key = restCacheKey(entity);
    if (key.empty()) return;
    auto& caches = importedRestCaches();
    if (caches.find(key) != caches.end()) return;

    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel) return;

    ImportedRestCache cache;
    auto boneIt = skel->getBoneIterator();
    while (boneIt.hasMoreElements()) {
        Ogre::Bone* bone = boneIt.getNext();
        cache.bones.push_back({bone->getName(),
                               {bone->getInitialPosition(), bone->getInitialOrientation(),
                                bone->getInitialScale()}});
    }
    cache.bindVertexBuffers = captureMeshBindVertices(entity->getMesh().get());
    caches.emplace(key, std::move(cache));
}

void SkeletonEditor::clearImportedRestCache(Ogre::Entity* entity)
{
    auto& caches = importedRestCaches();
    if (!entity) {
        caches.clear();
        return;
    }
    const std::string key = restCacheKey(entity);
    if (!key.empty())
        caches.erase(key);
}

SkeletonEditor::Result SkeletonEditor::captureRestPose(Ogre::Entity* entity, const QStringList& boneNames)
{
    Result result;
    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel) {
        result.error = QStringLiteral("Entity has no skeleton");
        return result;
    }
    ensureImportedRestCache(entity);

    Ogre::SkeletonInstance* inst = entity->getSkeleton();
    std::unordered_map<std::string, RestPoseTRS> newRests;

    auto addBone = [&](const std::string& name) {
        if (!skel->hasBone(name)) return;
        Ogre::Bone* src = nullptr;
        if (inst && inst->hasBone(name))
            src = inst->getBone(name);
        else
            src = skel->getBone(name);
        newRests[name] = {src->getPosition(), src->getOrientation(), src->getScale()};
    };

    if (boneNames.isEmpty()) {
        auto boneIt = skel->getBoneIterator();
        while (boneIt.hasMoreElements())
            addBone(boneIt.getNext()->getName());
    } else {
        for (const QString& n : boneNames)
            addBone(n.toStdString());
    }

    QString err;
    if (!applyRestPoseMap(entity, newRests, &err, /*rebakeAnimations=*/true,
                          /*bakeMesh=*/true)) {
        result.error = err;
        return result;
    }
    result.ok = true;
    return result;
}

SkeletonEditor::Result SkeletonEditor::resetRestPose(Ogre::Entity* entity)
{
    Result result;
    ensureImportedRestCache(entity);
    const std::string key = restCacheKey(entity);
    auto& caches = importedRestCaches();
    auto it = caches.find(key);
    if (it == caches.end() || it->second.bones.empty()) {
        result.error = QStringLiteral("No imported rest pose cached");
        return result;
    }

    Ogre::SkeletonPtr skel = skeletonResource(entity);
    if (!skel) {
        result.error = QStringLiteral("Entity has no skeleton");
        return result;
    }

    std::unordered_map<std::string, RestPoseTRS> newRests;
    for (const auto& [name, trs] : it->second.bones) {
        if (!skel->hasBone(name))
            continue; // renamed/removed since import — skip stale cache entries
        newRests[name] = trs;
    }
    if (newRests.empty()) {
        result.error = QStringLiteral("Cached rest pose has no matching bones");
        return result;
    }

    // Restore authored bind-pose verts before re-applying imported bone TRS.
    if (!it->second.bindVertexBuffers.empty())
        restoreMeshBindVertices(entity->getMesh().get(), it->second.bindVertexBuffers);

    QString err;
    if (!applyRestPoseMap(entity, newRests, &err, /*rebakeAnimations=*/true,
                          /*bakeMesh=*/false)) {
        result.error = err;
        return result;
    }
    result.ok = true;
    return result;
}

SkeletonEditor::Result SkeletonEditor::commitBoneRestPose(Ogre::Entity* entity,
                                                         const QString& boneName,
                                                         const Ogre::Vector3& pos,
                                                         const Ogre::Quaternion& orient,
                                                         const Ogre::Vector3& scale)
{
    Result result;
    if (!entity || boneName.isEmpty()) {
        result.error = QStringLiteral("Invalid entity or bone");
        return result;
    }
    ensureImportedRestCache(entity);
    std::unordered_map<std::string, RestPoseTRS> newRests;
    newRests[boneName.toStdString()] = {pos, orient, scale};
    QString err;
    // Bake posed mesh into bind + update bone bind. Do not rebake clips to
    // preserve absolute poses (that snaps the mesh back to the pre-edit look).
    if (!applyRestPoseMap(entity, newRests, &err, /*rebakeAnimations=*/false,
                          /*bakeMesh=*/true)) {
        result.error = err;
        return result;
    }
    result.ok = true;
    result.boneName = boneName;
    return result;
}

void SkeletonEditor::setEditRestPoseMode(bool on)
{
    if (m_editRestPoseMode == on) return;
    if (on)
        ensureEditRestSelectionHook();
    m_editRestPoseMode = on;
    applyEditRestAnimMute(on);
    SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.rest_pose.edit_mode"),
                                  on ? QStringLiteral("on") : QStringLiteral("off"));
    emit editRestPoseModeChanged();
}

void SkeletonEditor::setShowRestPoseGhost(bool on)
{
    if (m_showRestPoseGhost == on) return;
    m_showRestPoseGhost = on;
    syncRestPoseGhostOverlay();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.skel.rest_pose.ghost"),
                                  on ? QStringLiteral("on") : QStringLiteral("off"));
    emit showRestPoseGhostChanged();
}

void SkeletonEditor::applyEditRestAnimMute(bool mute)
{
    if (!mute) {
        if (!m_editRestMutedEntity.isEmpty()) {
            Ogre::Entity* ent = resolveEntityByName(m_editRestMutedEntity.toStdString());
            if (ent) {
                for (const QString& name : m_editRestMutedAnims) {
                    if (!ent->hasAnimationState(name.toStdString())) continue;
                    ent->getAnimationState(name.toStdString())->setEnabled(true);
                }
            }
        }
        m_editRestMutedEntity.clear();
        m_editRestMutedAnims.clear();
        return;
    }

    Ogre::Entity* ent = selectedSkinnedEntity();
    if (!ent) return;
    m_editRestMutedEntity = QString::fromStdString(ent->getName());
    m_editRestMutedAnims.clear();
    if (Ogre::AnimationStateSet* states = ent->getAllAnimationStates()) {
        for (const auto& pair : states->getAnimationStates()) {
            if (!pair.second->getEnabled()) continue;
            m_editRestMutedAnims.append(QString::fromStdString(pair.first));
            pair.second->setEnabled(false);
        }
    }
    // Settle bones at bind so the gizmo edits rest, not a frozen anim frame.
    if (Ogre::SkeletonInstance* skel = ent->getSkeleton())
        skel->reset(true);
    ent->_updateAnimation();
}

void SkeletonEditor::syncRestPoseGhostOverlay()
{
    if (auto* ppc = PropertiesPanelController::instance())
        ppc->setRestPoseGhostVisible(m_showRestPoseGhost);
}

bool SkeletonEditor::createBoneForSelected(const QString& parentBoneName)
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;

    CreateOptions opts;
    opts.parentBoneName = parentBoneName;
    if (opts.parentBoneName.isEmpty()) {
        if (auto* anim = AnimationControlController::instance())
            opts.parentBoneName = anim->selectedBone();
    }

    auto* cmd = new CreateBoneCommand(entity->getName(), opts);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::removeSelectedBone(bool removeChildren, bool transferWeightsToParent)
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    const QString bone = selectedBoneName();
    if (bone.isEmpty()) return false;

    RemoveOptions opts;
    opts.removeChildren = removeChildren;
    opts.transferWeightsToParent = transferWeightsToParent;

    auto* cmd = new RemoveBoneCommand(entity->getName(), bone, opts);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::renameSelectedBone(const QString& newName)
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    const QString oldName = selectedBoneName();
    if (oldName.isEmpty() || newName.isEmpty()) return false;

    auto* cmd = new RenameBoneCommand(entity->getName(), oldName, newName);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::duplicateSelectedBone()
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    const QString source = selectedBoneName();
    if (source.isEmpty()) return false;

    auto* cmd = new DuplicateBoneCommand(entity->getName(), source);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::reparentSelectedBone(const QString& newParentName, bool keepWorld)
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    const QString bone = selectedBoneName();
    if (bone.isEmpty()) return false;

    ReparentOptions opts;
    opts.keepWorld = keepWorld;
    auto* cmd = new ReparentBoneCommand(entity->getName(), bone, newParentName, opts);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::detachSelectedBone()
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    const QString bone = selectedBoneName();
    if (bone.isEmpty()) return false;

    ReparentOptions opts;
    opts.keepWorld = true;
    auto* cmd = new ReparentBoneCommand(entity->getName(), bone, {}, opts);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::splitSelectedBone(float t)
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    const QString bone = selectedBoneName();
    if (bone.isEmpty()) return false;

    auto* cmd = new SplitBoneCommand(entity->getName(), bone, t);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::setSelectedBoneConnected(bool connected)
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    const QString bone = selectedBoneName();
    if (bone.isEmpty()) return false;

    auto* cmd = new ConnectBoneCommand(entity->getName(), bone, connected);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::isSelectedBoneConnected() const
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    return isBoneConnected(entity, selectedBoneName());
}

bool SkeletonEditor::attachSelectedBoneToEntity(const QString& dstEntityName)
{
    Ogre::Entity* src = selectedSkinnedEntity();
    if (!src) return false;
    const QString bone = selectedBoneName();
    if (bone.isEmpty() || dstEntityName.isEmpty()) return false;

    auto* cmd = new AttachBoneToEntityCommand(
        src->getName(), QStringList{bone}, dstEntityName.toStdString(), {});
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::captureRestPoseForSelected()
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    auto* cmd = new SetRestPoseCommand(entity->getName(), SetRestPoseCommand::Op::CaptureAll);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::resetRestPoseForSelected()
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    auto* cmd = new SetRestPoseCommand(entity->getName(), SetRestPoseCommand::Op::Reset);
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

bool SkeletonEditor::snapSelectedBonesToCurrentPose()
{
    Ogre::Entity* entity = selectedSkinnedEntity();
    if (!entity) return false;
    const QString bone = selectedBoneName();
    if (bone.isEmpty()) return false;
    auto* cmd = new SetRestPoseCommand(entity->getName(), SetRestPoseCommand::Op::SnapSelected,
                                       QStringList{bone});
    UndoManager::getSingleton()->push(cmd);
    return cmd->applied();
}

void SkeletonEditor::requestBoneContextMenu(int globalX, int globalY)
{
    emit boneContextMenuRequested(globalX, globalY);
}

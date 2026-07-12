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
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>
#include <OgreSkeletonManager.h>

#include <QSet>
#include <QStringList>

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {

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
    delete s_singleton;
    s_singleton = nullptr;
}

SkeletonEditor::SkeletonEditor(QObject* parent) : QObject(parent) {}

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
        bone->setPosition(bd.position);
        bone->setOrientation(bd.orientation);
        bone->setScale(bd.scale);
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

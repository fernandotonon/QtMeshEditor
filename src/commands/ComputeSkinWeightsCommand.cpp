#include "commands/ComputeSkinWeightsCommand.h"
#include "Manager.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>

ComputeSkinWeightsCommand::ComputeSkinWeightsCommand(std::string entityName,
                                                     SkinWeightsOptions opts,
                                                     SkinWeights::Algorithm algo,
                                                     QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mOpts(opts)
    , mAlgo(algo)
{
    setText(QStringLiteral("Compute Skin Weights"));
}

Ogre::Entity* ComputeSkinWeightsCommand::resolveEntity() const
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    // Manager::getEntities() already filters to real Ogre::Entity
    // objects at collection time (collectEntitiesRecursive only
    // appends attached objects whose getMovableType() == "Entity"),
    // so every element here is a genuine Entity — the
    // getMovableType() re-check below is a belt-and-suspenders
    // guard, not load-bearing.
    for (Ogre::Entity* e : mgr->getEntities()) {
        if (e && e->getMovableType() == "Entity"
            && e->getName() == mEntityName)
            return e;
    }
    return nullptr;
}

void ComputeSkinWeightsCommand::captureSnapshot(
    Ogre::Mesh* mesh, std::vector<OwnerSnapshot>& out) const
{
    out.clear();
    if (!mesh) return;

    // Mesh-level (shared-vertex) assignments — owner index -1.
    bool anyShared = false;
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        if (mesh->getSubMesh(si) && mesh->getSubMesh(si)->useSharedVertices) {
            anyShared = true;
            break;
        }
    }
    if (anyShared) {
        OwnerSnapshot s;
        s.submeshIndex = -1;
        for (const auto& kv : mesh->getBoneAssignments())
            s.assignments.insert(kv);
        out.push_back(std::move(s));
    }

    // Per-submesh (non-shared) assignments.
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub || sub->useSharedVertices) continue;
        OwnerSnapshot s;
        s.submeshIndex = si;
        for (const auto& kv : sub->getBoneAssignments())
            s.assignments.insert(kv);
        out.push_back(std::move(s));
    }
}

void ComputeSkinWeightsCommand::restoreSnapshot(
    Ogre::Mesh* mesh, const std::vector<OwnerSnapshot>& snap) const
{
    if (!mesh) return;
    for (const auto& s : snap) {
        if (s.submeshIndex < 0) {
            mesh->clearBoneAssignments();
            for (const auto& kv : s.assignments)
                mesh->addBoneAssignment(kv.second);
            // _compileBoneAssignments re-packs BLEND_INDICES /
            // BLEND_WEIGHTS into the (unchanged) shared vertex buffer
            // and rebuilds sharedBlendIndexToBoneIndexMap from the
            // restored list. Unlike the UV-unwrap restore — which
            // swapped the entire VertexData out from under a live
            // SkeletonInstance — here the buffer object is the same
            // one the skeleton already references, so recompiling is
            // both correct and safe: it just rewrites the blend
            // bytes in place to match the restored weights.
            mesh->_compileBoneAssignments();
        } else if (s.submeshIndex < static_cast<int>(mesh->getNumSubMeshes())) {
            Ogre::SubMesh* sub = mesh->getSubMesh(
                static_cast<unsigned short>(s.submeshIndex));
            if (!sub) continue;
            sub->clearBoneAssignments();
            for (const auto& kv : s.assignments)
                sub->addBoneAssignment(kv.second);
            sub->_compileBoneAssignments();
        }
    }
}

void ComputeSkinWeightsCommand::redo()
{
    Ogre::Entity* entity = resolveEntity();
    if (!entity || !entity->getMesh()) {
        mReport.applied = false;
        mReport.error = QStringLiteral("entity not found / no mesh");
        return;
    }
    Ogre::Mesh* mesh = entity->getMesh().get();

    if (!mCaptured) {
        // First execution: snapshot the pre-skin weights, run the
        // compute, then snapshot the post-skin weights for replay.
        captureSnapshot(mesh, mBefore);
        mReport = SkinWeights::computeAndApply(entity, mOpts, mAlgo);
        captureSnapshot(mesh, mAfter);
        mCaptured = true;
    } else {
        // Subsequent redo (after an undo): replay the captured
        // "after" state without recomputing.
        restoreSnapshot(mesh, mAfter);
    }
}

void ComputeSkinWeightsCommand::undo()
{
    if (!mCaptured) return;
    Ogre::Entity* entity = resolveEntity();
    if (!entity || !entity->getMesh()) return;
    restoreSnapshot(entity->getMesh().get(), mBefore);
}

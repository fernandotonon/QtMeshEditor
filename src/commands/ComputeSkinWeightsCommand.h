#ifndef COMPUTE_SKIN_WEIGHTS_COMMAND_H
#define COMPUTE_SKIN_WEIGHTS_COMMAND_H

#include <QUndoCommand>
#include <QString>

#include "SkinWeights.h"

#include <map>
#include <memory>
#include <vector>

namespace Ogre {
    class Entity;
    class Mesh;
    struct VertexBoneAssignment;
}

/**
 * Undoable wrapper around `SkinWeights::computeAndApply` (issue #402
 * follow-up). The auto-skin operation rewrites every submesh's bone
 * assignments (and, for shared-vertex meshes, the mesh-level list);
 * this command snapshots all of those lists plus the
 * `blendIndexToBoneIndexMap`s before the first redo so undo restores
 * the exact pre-skin weights.
 *
 * `redo()` runs the compute (first time) or replays the captured
 * "after" state (subsequent redos). `undo()` pastes the "before"
 * snapshot back. Both paths reinstall the index maps directly and
 * deliberately do NOT call `_compileBoneAssignments` on restore —
 * the captured BLEND_INDICES/WEIGHTS bytes in the vertex buffer are
 * already consistent with the snapshot, and recompiling against a
 * live `SkeletonInstance` would shatter the on-screen mesh (same
 * hazard the UV-unwrap restore documents).
 *
 * The command targets the entity by name so it survives scene
 * rebuilds the way the other entity-scoped commands do.
 */
class ComputeSkinWeightsCommand : public QUndoCommand
{
public:
    ComputeSkinWeightsCommand(std::string entityName,
                              SkinWeightsOptions opts,
                              SkinWeights::Algorithm algo
                                  = SkinWeights::Algorithm::SkinTokens,
                              QUndoCommand* parent = nullptr);

    /// Precomputed mode (the async GUI path): the heavy compute
    /// already ran on a worker (SkinWeights::runJob); the first
    /// redo() only COMMITS the given result on the main thread
    /// (snapshotting around it for undo, like the compute mode).
    ComputeSkinWeightsCommand(std::string entityName,
                              SkinWeightsOptions opts,
                              std::shared_ptr<SkinWeights::ComputeJob> job,
                              std::shared_ptr<SkinWeights::JobResult> result,
                              QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    /// The report produced by the first `redo()`. Lets the
    /// controller surface "N bones, M verts, X assignments" to the
    /// UI after pushing the command.
    const SkinWeightsReport& report() const { return mReport; }
    bool applied() const { return mReport.applied; }

private:
    // One snapshot per assignment owner. `submeshIndex == -1` means
    // the mesh-level (shared-vertex) list. We only snapshot the
    // assignment list itself — `restoreSnapshot` calls
    // `_compileBoneAssignments`, which rebuilds the
    // blendIndexToBoneIndexMap and re-packs the vertex buffer's
    // BLEND bytes from that list, so the index map need not be
    // captured separately.
    struct OwnerSnapshot {
        int submeshIndex = 0;
        std::multimap<size_t, Ogre::VertexBoneAssignment> assignments;
    };

    Ogre::Entity* resolveEntity() const;
    void captureSnapshot(Ogre::Mesh* mesh,
                         std::vector<OwnerSnapshot>& out) const;
    void restoreSnapshot(Ogre::Mesh* mesh,
                         const std::vector<OwnerSnapshot>& snap) const;

    std::string             mEntityName;
    SkinWeightsOptions      mOpts;
    SkinWeights::Algorithm  mAlgo = SkinWeights::Algorithm::SkinTokens;
    // Precomputed mode payload (null in compute mode).
    std::shared_ptr<SkinWeights::ComputeJob> mJob;
    std::shared_ptr<SkinWeights::JobResult>  mResult;
    SkinWeightsReport       mReport;

    std::vector<OwnerSnapshot> mBefore;   // pre-skin weights
    std::vector<OwnerSnapshot> mAfter;    // post-skin weights (for redo replay)
    bool mCaptured = false;
};

#endif // COMPUTE_SKIN_WEIGHTS_COMMAND_H

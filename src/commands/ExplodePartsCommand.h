#ifndef EXPLODE_PARTS_COMMAND_H
#define EXPLODE_PARTS_COMMAND_H

#include <QUndoCommand>
#include <QString>

#include <OgreMesh.h>
#include <OgreVector3.h>

#include <string>
#include <vector>

namespace Ogre { class Entity; class SceneNode; }

/**
 * Undoable PartOps explode (#859/#862): replaces a fused multi-submesh entity
 * with one sibling scene node + entity per submesh (part), each offset outward
 * so the assembly "explodes" for inspection and independent editing.
 *
 * Explode consumes the source: on redo the fused entity's node is destroyed and
 * N part nodes are created at the source node's world transform, each translated
 * by its explode offset (in the source node's local frame). On undo the part
 * nodes are destroyed and the fused node is recreated bound to the ORIGINAL mesh
 * (kept resident for the command's lifetime).
 *
 * The heavy geometry work (per-part mesh build + offsets) runs once on the first
 * redo (`PartOpsScene::explodeEntity`) and is cached on the command, so later
 * redos just recreate nodes from the cached meshes. Each part node is named
 * `<source>_<partName>` (Manager uniquifies); node name == entity name.
 *
 * Runs in Object mode. Like SplitMeshCommand it clears the SelectionSet before
 * destroying entities (the node survives a swap but sub-entity refs would
 * dangle), and reselects the resulting nodes.
 */
class ExplodePartsCommand : public QUndoCommand
{
public:
    /** @param entityName  the fused entity to explode (== its node name).
     *  @param distance     explode offset multiplier (× assembly diagonal). */
    ExplodePartsCommand(std::string entityName, float distance,
                        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool ok() const { return mOk; }
    const QString& error() const { return mError; }
    int createdParts() const { return static_cast<int>(mParts.size()); }
    /** Names of the part nodes created by the last redo (for reselection/UI). */
    const std::vector<std::string>& partNodeNames() const { return mPartNodeNames; }

private:
    Ogre::Entity* resolveSourceEntity() const;
    void buildOnce();

    std::string mEntityName;
    float       mDistance = 0.5f;

    struct PartCache {
        Ogre::MeshPtr mesh;   ///< single-submesh part mesh (resident for redo).
        QString name;         ///< part display name.
        Ogre::Vector3 offset; ///< explode translation in source-node local frame.
    };
    std::vector<PartCache> mParts;

    // Source node LOCAL transform captured at explode time (undo restores the
    // fused node with it; each part starts here + its offset). Parts are
    // reparented under the SAME parent as the source, so local TRS is correct.
    Ogre::Vector3    mSrcPos = Ogre::Vector3::ZERO;
    Ogre::Quaternion mSrcOrient = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3    mSrcScale = Ogre::Vector3::UNIT_SCALE;
    // Name of the source node's parent (empty if it was a direct child of the
    // scene root) so parts and the restored fused node keep the same grouping.
    std::string      mParentNodeName;

    Ogre::MeshPtr mOriginalMesh;          ///< fused mesh, resident for undo.
    std::vector<std::string> mPartNodeNames; ///< current part node names (post-redo).

    bool mBuilt = false;
    bool mOk = false;
    QString mError;
};

#endif // EXPLODE_PARTS_COMMAND_H

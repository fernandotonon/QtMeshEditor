#ifndef ADD_PRINT_PEGS_COMMAND_H
#define ADD_PRINT_PEGS_COMMAND_H

#include <QUndoCommand>
#include <QString>

#include <OgreMesh.h>

#include "SubMeshOps.h"

#include <string>
#include <vector>

namespace Ogre { class Entity; class SceneNode; }

/**
 * Undoable PartOps print-prep (#859/#863): adds cylindrical alignment pegs to an
 * already-SPLIT entity so its parts snap together for 3D printing. Each part
 * that shares a stable boundary with another gains a male-peg / female-socket
 * connector merged into its geometry.
 *
 * Adding pegs merges NEW triangles into existing submeshes (the part count is
 * unchanged), so — like SplitMeshCommand — this swaps the whole mesh on the
 * scene node rather than mutating buffers in place (the safe path for a geometry
 * change). redo() runs `PartOpsMesh::addPrintPegsToEntity` once (cached), then
 * swaps the pegged mesh onto the node; undo() restores the resident pre-peg mesh.
 * Node and entity share a name, so the command targets by that name and survives
 * scene rebuilds. Runs in Object mode.
 */
class AddPrintPegsCommand : public QUndoCommand
{
public:
    AddPrintPegsCommand(std::string entityName,
                        SubMeshOps::PegOptions opts,
                        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool ok() const { return mOk; }
    const QString& error() const { return mError; }
    int peggedBoundaries() const { return mPeggedBoundaries; }
    int totalPegs() const { return mTotalPegs; }
    const std::vector<QString>& warnings() const { return mWarnings; }

private:
    Ogre::Entity* resolveEntity() const;
    Ogre::Entity* swapEntityMesh(const Ogre::MeshPtr& mesh);

    std::string mEntityName;
    SubMeshOps::PegOptions mOpts;

    Ogre::SceneNode* mReselectNode = nullptr;
    Ogre::MeshPtr mOriginalMesh;   ///< pre-peg mesh, resident for undo.
    Ogre::MeshPtr mPeggedMesh;     ///< built once on first redo.
    bool          mBuilt = false;
    bool          mOk = false;
    QString       mError;
    int           mPeggedBoundaries = 0;
    int           mTotalPegs = 0;
    std::vector<QString> mWarnings;
};

#endif // ADD_PRINT_PEGS_COMMAND_H

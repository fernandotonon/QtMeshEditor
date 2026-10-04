#ifndef LATTICE_COMMANDS_H
#define LATTICE_COMMANDS_H

#include <QUndoCommand>
#include <QString>
#include <QJsonObject>

#include <OgreVector.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * Undo commands for the lattice deformer (LatticeController).
 *
 * Two levels, mirroring Blender's lattice workflow:
 *
 *  - **LatticeGridCommand** — one edit INSIDE a live session: a control-point
 *    drag, reset, load, resolution or interpolation change. Snapshots the whole
 *    `qtmesh-lattice-v1` document before and after, so a resolution change
 *    (which reshapes the point array) is as undoable as a drag. Stamped with
 *    the controller's SESSION ID: a command from an earlier, cancelled session
 *    on the same mesh is a no-op in a later one (a later session's rest shape
 *    is different, so replaying old points into it would bend the mesh
 *    unasked). Also a no-op once the session has closed — the bake command
 *    owns the mesh state then.
 *
 *  - **LatticeApplyCommand** — the BAKE. Captures dense per-submesh rest
 *    positions AND rest normals plus the deformed positions (the whole mesh,
 *    not a sparse map — a lattice touches most vertices). Undo writes the rest
 *    positions/normals back WITHOUT recomputing normals, so authored (hard /
 *    custom) normals survive a bake→undo; redo re-deforms and recomputes.
 *    Resolves its entity by NAME on every undo/redo, never a cached pointer,
 *    and abandons any live lattice session on that entity first (its frozen
 *    rest snapshot would no longer describe the mesh).
 */
namespace Ogre { class Mesh; }

namespace LatticeCmd {
using Positions = std::vector<std::vector<Ogre::Vector3>>; ///< [submesh][vertex], mesh-local.

/// Write `positions` (and, when non-null, `normals`) into the named entity's
/// vertex buffers via EditableMesh. `recomputeNormals` = derive normals from
/// the new positions (a deform); false = keep/restore the given normals.
/// `expectedMesh` (when non-null) must be the entity's CURRENT Ogre::Mesh —
/// a same-name replacement with a compatible layout is refused rather than
/// overwritten. Returns false when the entity is gone, replaced, or the
/// layout no longer matches.
bool writePositionsToEntity(const std::string& entityName, const Positions& positions,
                            const Positions* normals, bool recomputeNormals,
                            const Ogre::Mesh* expectedMesh, QString* error = nullptr);
}

class LatticeGridCommand : public QUndoCommand
{
public:
    LatticeGridCommand(std::string entityName,
                       uint64_t sessionId,
                       QJsonObject before,
                       QJsonObject after,
                       const QString& description,
                       QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    std::string mEntityName;
    uint64_t mSessionId = 0;
    QJsonObject mBefore;
    QJsonObject mAfter;
    bool mFirstRedo = true;
};

class LatticeApplyCommand : public QUndoCommand
{
public:
    /** @param alreadyApplied  true when the deformed positions are already on
     *  the GPU (the interactive session) — the first redo() is then skipped. */
    /** @param meshIdentity  the entity's Ogre::Mesh at record time; undo/redo
     *  refuse to write when the entity now carries a different mesh. */
    LatticeApplyCommand(std::string entityName,
                        const Ogre::Mesh* meshIdentity,
                        LatticeCmd::Positions rest,
                        LatticeCmd::Positions restNormals,
                        LatticeCmd::Positions deformed,
                        QJsonObject lattice,
                        bool alreadyApplied,
                        QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

    bool ok() const { return mOk; }
    const QString& error() const { return mError; }
    const QJsonObject& lattice() const { return mLattice; }

private:
    std::string mEntityName;
    const Ogre::Mesh* mMeshIdentity = nullptr;
    LatticeCmd::Positions mRest;
    LatticeCmd::Positions mRestNormals;
    LatticeCmd::Positions mDeformed;
    QJsonObject mLattice;
    bool mSkipFirstRedo = false;
    bool mOk = true;
    QString mError;
};

#endif // LATTICE_COMMANDS_H
